#include "quick_look_hotkey.h"
#include "../common/runtime_log.h"

#include <exdisp.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <string>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace pulse::app {
namespace {
// Posted from the hook callback to this thread: the shell lookup below talks COM
// and must not run inside a low level hook, which Windows silently removes when
// a callback takes too long.
constexpr UINT kPreviewMessage = WM_APP + 300;

enum class Surface { None, Explorer, Desktop };

Surface Classify(HWND window) {
    if (!window) return Surface::None;
    const HWND root = GetAncestor(window, GA_ROOT);
    if (!root) return Surface::None;
    wchar_t name[64]{};
    if (!GetClassNameW(root, name, ARRAYSIZE(name))) return Surface::None;
    if (wcscmp(name, L"CabinetWClass") == 0 || wcscmp(name, L"ExploreWClass") == 0) return Surface::Explorer;
    if (wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0) return Surface::Desktop;
    return Surface::None;
}

bool ModifierDown() {
    for (const int key : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN})
        if (GetAsyncKeyState(key) & 0x8000) return true;
    return false;
}

// Renaming turns the shell view into a text box, where Space has to stay a
// space. GetGUIThreadInfo reaches windows owned by another thread; GetFocus
// does not.
bool FocusIsTextInput(HWND foreground) {
    const DWORD thread = GetWindowThreadProcessId(foreground, nullptr);
    if (!thread) return false;
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(thread, &info) || !info.hwndFocus) return false;
    wchar_t name[64]{};
    if (!GetClassNameW(info.hwndFocus, name, ARRAYSIZE(name))) return false;
    return wcsstr(name, L"Edit") != nullptr || wcsstr(name, L"EDIT") != nullptr;
}

ComPtr<IFolderView2> ViewOf(const ComPtr<IWebBrowser2>& browser) {
    ComPtr<IServiceProvider> provider;
    ComPtr<IShellBrowser> shell_browser;
    ComPtr<IShellView> view;
    ComPtr<IFolderView2> folder_view;
    if (FAILED(browser.As(&provider)) ||
        FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shell_browser))) ||
        FAILED(shell_browser->QueryActiveShellView(&view)) || !view || FAILED(view.As(&folder_view)))
        return {};
    return folder_view;
}

// The shell view behind `foreground`: the desktop's view for Progman/WorkerW,
// otherwise the Explorer window whose shell HWND is that same top level window.
ComPtr<IFolderView2> FindView(HWND foreground, Surface surface) {
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows))) || !windows)
        return {};
    if (surface == Surface::Desktop) {
        VARIANT location;
        VariantInit(&location);
        location.vt = VT_I4;
        location.lVal = CSIDL_DESKTOP;
        VARIANT empty;
        VariantInit(&empty);
        long handle = 0;
        ComPtr<IDispatch> dispatch;
        if (SUCCEEDED(windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &handle, SWFO_NEEDDISPATCH, &dispatch)) &&
            dispatch) {
            ComPtr<IWebBrowser2> browser;
            if (SUCCEEDED(dispatch.As(&browser)) && browser)
                if (auto view = ViewOf(browser)) return view;
        }
        return {};
    }
    const HWND root = GetAncestor(foreground, GA_ROOT);
    long count = 0;
    if (FAILED(windows->get_Count(&count)) || count <= 0) return {};
    for (long i = 0; i < count; ++i) {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> item;
        if (FAILED(windows->Item(index, &item)) || !item) continue;
        ComPtr<IWebBrowser2> browser;
        if (FAILED(item.As(&browser)) || !browser) continue;
        SHANDLE_PTR handle = 0;
        if (FAILED(browser->get_HWND(&handle)) || !handle) continue;
        if (reinterpret_cast<HWND>(handle) != root) continue;
        if (auto view = ViewOf(browser)) return view;
    }
    return {};
}

// First selected item of the shell view, as a filesystem path. Empty when
// nothing is selected or the selection has no filesystem location (a virtual
// folder such as This PC).
std::wstring SelectedPath(HWND foreground, Surface surface) {
    ComPtr<IFolderView2> view = FindView(foreground, surface);
    if (!view) return {};
    ComPtr<IShellItemArray> items;
    if (view->GetSelection(FALSE, &items) != S_OK || !items) return {};
    DWORD selected = 0;
    if (FAILED(items->GetCount(&selected)) || selected == 0) return {};
    ComPtr<IShellItem> first;
    if (FAILED(items->GetItemAt(0, &first)) || !first) return {};
    PWSTR path = nullptr;
    if (FAILED(first->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return {};
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

bool LaunchPreview(const std::wstring& path) {
    wchar_t module[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, module, ARRAYSIZE(module))) return false;
    std::wstring command = L"\"" + std::wstring(module) + L"\" --quick-look \"" + path + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(module, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        diagnostics::runtime::Event("quick_look_space", {{"launched", 0}, {"error", GetLastError()}});
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
} // namespace

QuickLookHotkey::QuickLookHotkey() {
    thread_ = CreateThread(nullptr, 0, Thread, this, 0, nullptr);
    if (!thread_) diagnostics::runtime::Event("quick_look_space", {{"enabled", 0}, {"error", GetLastError()}});
}
QuickLookHotkey::~QuickLookHotkey() { Stop(); }

void QuickLookHotkey::Stop() {
    stopping_ = true;
    if (!thread_) return;
    // The id exists only once the thread owns a message queue; posting WM_QUIT to
    // zero would drop it and the join below would wait forever.
    DWORD id = 0;
    for (int attempt = 0; attempt < 500 && (id = thread_id_.load()) == 0; ++attempt) Sleep(1);
    if (id) PostThreadMessageW(id, WM_QUIT, 0, 0);
    WaitForSingleObject(thread_, INFINITE);
    CloseHandle(thread_);
    thread_ = nullptr;
}

bool QuickLookHotkey::Preview(HWND foreground) {
    const Surface surface = Classify(foreground);
    if (surface == Surface::None) return false;
    const std::wstring path = SelectedPath(foreground, surface);
    if (path.empty()) return false;
    if (LaunchPreview(path)) diagnostics::runtime::Event("quick_look_space", {{"previewed", 1}, {"error", 0}});
    return true;
}

// Marked injected, so the hook passes it straight through: a Space we swallowed
// that turned out to have nothing to preview still reaches Explorer.
void QuickLookHotkey::ReinjectSpace() {
    INPUT input[2]{};
    input[0].type = input[1].type = INPUT_KEYBOARD;
    input[0].ki.wVk = input[1].ki.wVk = VK_SPACE;
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, input, sizeof(INPUT));
}

DWORD WINAPI QuickLookHotkey::Thread(void* value) {
    auto& self = *static_cast<QuickLookHotkey*>(value);
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    self.thread_id_ = GetCurrentThreadId();
    if (self.stopping_) return 0;
    current_ = &self;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        diagnostics::runtime::Event("quick_look_space", {{"enabled", 0}, {"error", GetLastError()}});
        current_ = nullptr;
        return 0;
    }
    self.hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, Keyboard, GetModuleHandleW(nullptr), 0);
    self.active_ = self.hook_ != nullptr;
    diagnostics::runtime::Event("quick_look_space",
        {{"enabled", self.active_.load()}, {"error", self.hook_ ? ERROR_SUCCESS : GetLastError()}});
    while (self.hook_ && !self.stopping_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!message.hwnd && message.message == kPreviewMessage) {
            if (!self.Preview(reinterpret_cast<HWND>(message.wParam))) self.ReinjectSpace();
        } else {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (self.hook_) {
        UnhookWindowsHookEx(self.hook_);
        self.hook_ = nullptr;
    }
    CoUninitialize();
    self.active_ = false;
    current_ = nullptr;
    diagnostics::runtime::Event("quick_look_space", {{"enabled", 0}, {"error", 0}});
    return 0;
}

LRESULT CALLBACK QuickLookHotkey::Keyboard(int code, WPARAM message, LPARAM data) {
    auto* self = current_;
    if (code != HC_ACTION || !self || self->stopping_) return CallNextHookEx(nullptr, code, message, data);
    const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
    if (key.vkCode != VK_SPACE) return CallNextHookEx(nullptr, code, message, data);
    if (key.flags & LLKHF_INJECTED) return CallNextHookEx(nullptr, code, message, data);
    const bool pressed = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool released = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (!pressed && !released) return CallNextHookEx(nullptr, code, message, data);
    if (released) {
        const bool swallowed = self->swallowed_;
        self->swallowed_ = false;
        self->space_down_ = false;
        return swallowed ? 1 : CallNextHookEx(nullptr, code, message, data);
    }
    if (self->space_down_) return CallNextHookEx(nullptr, code, message, data);
    self->space_down_ = true;
    // Everything else keeps Explorer's own Space behaviour (rename, type ahead).
    if (ModifierDown()) return CallNextHookEx(nullptr, code, message, data);
    const HWND foreground = GetForegroundWindow();
    if (Classify(foreground) == Surface::None) return CallNextHookEx(nullptr, code, message, data);
    if (FocusIsTextInput(foreground)) return CallNextHookEx(nullptr, code, message, data);
    const auto id = self->thread_id_.load();
    if (!id) return CallNextHookEx(nullptr, code, message, data);
    self->swallowed_ = true;
    PostThreadMessageW(id, kPreviewMessage, reinterpret_cast<WPARAM>(foreground), 0);
    return 1;
}
} // namespace pulse::app
