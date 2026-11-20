#include "quick_look_hotkey.h"
#include "app_prefs.h"
#include "picker_appearance.h"
#include "../common/runtime_log.h"

#include <commctrl.h>
#include <exdisp.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <string>
#include <cstdlib>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace pulse::app {
namespace {
// Posted from the hook callback to this thread: the shell lookup below talks COM
// and must not run inside a low level hook, which Windows silently removes when
// a callback takes too long.
constexpr UINT kPreviewMessage = WM_APP + 300;
// Polls the cursor for hover preview. Coarse enough to stay cheap, fine enough
// that the settle delay feels immediate.
constexpr UINT_PTR kHoverTimer = 2;
constexpr UINT kHoverPollMs = 100;

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

struct ShellView {
    ComPtr<IFolderView2> folder;
    HWND window = nullptr; // IShellView::GetWindow, hosts the item list control
};

ShellView ViewOf(const ComPtr<IWebBrowser2>& browser) {
    ComPtr<IServiceProvider> provider;
    ComPtr<IShellBrowser> shell_browser;
    ComPtr<IShellView> view;
    ShellView result;
    if (FAILED(browser.As(&provider)) ||
        FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shell_browser))) ||
        FAILED(shell_browser->QueryActiveShellView(&view)) || !view)
        return {};
    view->GetWindow(&result.window);
    if (FAILED(view.As(&result.folder))) result.window = nullptr;
    return result;
}

// The shell view behind `foreground`: the desktop's view for Progman/WorkerW,
// otherwise the Explorer window whose shell HWND is that same top level window.
ShellView FindView(HWND foreground, Surface surface) {
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
            if (SUCCEEDED(dispatch.As(&browser)) && browser) {
                ShellView view = ViewOf(browser);
                if (view.folder) return view;
            }
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
        ShellView view = ViewOf(browser);
        if (view.folder) return view;
    }
    return {};
}

// Resolve an item index inside a shell view to a filesystem path.
std::wstring PathForIndex(const ComPtr<IFolderView2>& view, int index) {
    if (index < 0) return {};
    ComPtr<IFolderView> folder_view;
    if (FAILED(view.As(&folder_view))) return {};
    PITEMID_CHILD child = nullptr;
    if (FAILED(folder_view->Item(index, &child)) || !child) return {};
    ComPtr<IPersistFolder2> folder;
    HRESULT hr = folder_view->GetFolder(IID_PPV_ARGS(&folder));
    PIDLIST_ABSOLUTE parent = nullptr;
    if (SUCCEEDED(hr)) hr = folder->GetCurFolder(&parent);
    if (FAILED(hr) || !parent) {
        CoTaskMemFree(child);
        return {};
    }
    PIDLIST_ABSOLUTE absolute = ILCombine(parent, child);
    CoTaskMemFree(child);
    CoTaskMemFree(parent);
    if (!absolute) return {};
    PWSTR path = nullptr;
    hr = SHGetNameFromIDList(absolute, SIGDN_FILESYSPATH, &path);
    CoTaskMemFree(absolute);
    if (FAILED(hr) || !path) return {};
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

// The item the shell currently has focus on, as a filesystem path. This is the
// item a Space press actually acts on. Reading GetSelection()[0] instead is
// wrong: the selection array keeps its insertion order and can still contain a
// previously selected item while the focused item has moved elsewhere, which
// made Space preview the last file instead of the one under the cursor.
std::wstring FocusedPath(const ComPtr<IFolderView2>& view) {
    ComPtr<IFolderView> folder_view;
    if (FAILED(view.As(&folder_view))) return {};
    int index = -1;
    if (FAILED(folder_view->GetFocusedItem(&index))) return {};
    return PathForIndex(view, index);
}

// List-view control that hosts the shell view's items (SysListView32 on most
// builds). LVM_HITTEST gives us the item under the cursor for hover preview.
HWND ItemListWindow(HWND view_window) {
    if (!view_window) return nullptr;
    wchar_t name[64]{};
    GetClassNameW(view_window, name, ARRAYSIZE(name));
    if (wcscmp(name, L"SysListView32") == 0) return view_window;
    return FindWindowExW(view_window, nullptr, L"SysListView32", nullptr);
}

// Filesystem path of the item under the cursor, or empty when the cursor is on
// empty space, a non-file item, or our own preview window.
std::wstring PathAtPoint(HWND view_window, const ComPtr<IFolderView2>& view, POINT cursor) {
    HWND list = ItemListWindow(view_window);
    if (!list) return {};
    POINT client = cursor;
    ScreenToClient(list, &client);
    LVHITTESTINFO hit{};
    hit.pt = client;
    if (SendMessageW(list, LVM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit)) < 0 || hit.iItem < 0)
        return {};
    return PathForIndex(view, hit.iItem);
}

int ReadHoverDelayMs() {
    return NormalizeHoverPreviewDelay(ReadUserPickerAppearanceReadOnly().snapshot.hover_preview_delay_ms);
}

// First selected item of the shell view, as a filesystem path. Empty when
// nothing is selected or the selection has no filesystem location (a virtual
// folder such as This PC).
std::wstring SelectedPath(HWND foreground, Surface surface) {
    ShellView shell = FindView(foreground, surface);
    if (!shell.folder) return {};
    if (std::wstring focused = FocusedPath(shell.folder); !focused.empty()) return focused;
    ComPtr<IShellItemArray> items;
    if (shell.folder->GetSelection(FALSE, &items) != S_OK || !items) return {};
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

bool LaunchPreview(const std::wstring& path, HANDLE* process_out = nullptr) {
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
    if (process_out) *process_out = process.hProcess;
    else CloseHandle(process.hProcess);
    return true;
}

// True when the point is on our own preview window (or its transparent owner).
// Hover must ignore its own window or it would dismiss the preview it just
// opened as soon as that preview appears under the cursor.
bool IsQuickLookWindow(HWND window) {
    for (HWND current = window; current; current = GetAncestor(current, GA_PARENT)) {
        wchar_t name[64]{};
        if (!GetClassNameW(current, name, ARRAYSIZE(name))) break;
        if (wcscmp(name, L"Pulse.QuickPreview") == 0 || wcscmp(name, L"PulseQuickLookHost") == 0) return true;
    }
    return false;
}
} // namespace

QuickLookHotkey::QuickLookHotkey() {
    accept_injected_ = GetEnvironmentVariableW(L"PULSE_TEST_QUICK_LOOK_KEYS", nullptr, 0) > 0;
    hover_delay_ms_ = ReadHoverDelayMs();
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
    // Any earlier preview (hover or a previous Space press) must be replaced:
    // leaving several Quick Look windows alive is what made Space appear to show
    // the file from a previous press instead of the one now selected.
    CloseHoverPreview();
    HANDLE process = nullptr;
    if (!LaunchPreview(path, &process)) return false;
    hover_process_ = process;
    hover_path_ = path;
    hover_shown_ = true;
    hover_shown_at_ = GetTickCount64();
    diagnostics::runtime::Event("quick_look_space", {{"previewed", 1}, {"error", 0}});
    return true;
}

namespace {
struct CloseByProcess {
    DWORD pid;
    bool posted = false;
};

BOOL CALLBACK PostCloseToProcess(HWND hwnd, LPARAM lparam) {
    auto* state = reinterpret_cast<CloseByProcess*>(lparam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == state->pid) {
        wchar_t name[64]{};
        if (GetClassNameW(hwnd, name, ARRAYSIZE(name)) &&
            wcscmp(name, L"PulseQuickLookHost") == 0) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            state->posted = true;
            return FALSE;
        }
    }
    return TRUE;
}
} // namespace

void QuickLookHotkey::CloseHoverPreview() {
    hover_pending_ = false;
    hover_shown_ = false;
    hover_path_.clear();
    if (hover_process_) {
        // Ask the preview to shut down through its own message loop so COM and
        // the hosted handler release cleanly; fall back to termination if the
        // window is not up yet.
        CloseByProcess state{GetProcessId(hover_process_), false};
        EnumWindows(PostCloseToProcess, reinterpret_cast<LPARAM>(&state));
        if (state.posted) {
            WaitForSingleObject(hover_process_, 500);
        } else {
            TerminateProcess(hover_process_, 0);
            WaitForSingleObject(hover_process_, 500);
        }
        CloseHandle(hover_process_);
        hover_process_ = nullptr;
    }
}

void QuickLookHotkey::CheckHover() {
    POINT cursor{0, 0};
    GetCursorPos(&cursor);
    const HWND under = WindowFromPoint(cursor);

    // Once the preview is open it often sits under the cursor. Keep it open in
    // that case; the pointer moving back onto the shell is what dismisses it.
    if (IsQuickLookWindow(under)) return;

    const Surface surface = Classify(under);
    if (surface == Surface::None) {
        CloseHoverPreview();
        return;
    }
    const HWND root = GetAncestor(under, GA_ROOT);
    const ShellView shell = FindView(root, surface);
    if (!shell.folder || !shell.window) {
        CloseHoverPreview();
        return;
    }
    const std::wstring path = PathAtPoint(shell.window, shell.folder, cursor);
    if (path.empty()) {
        CloseHoverPreview();
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (hover_shown_) {
        if (path == hover_path_) return; // staying on the item: keep it open
        // Moved to a different item: replace the old preview.
        CloseHoverPreview();
    }
    if (!hover_pending_ || path != hover_path_ || std::abs(cursor.x - hover_anchor_.x) > 8 ||
        std::abs(cursor.y - hover_anchor_.y) > 8) {
        hover_pending_ = true;
        hover_path_ = path;
        hover_anchor_ = cursor;
        hover_moved_at_ = now;
        return;
    }
    if (now - hover_moved_at_ < static_cast<ULONGLONG>(hover_delay_ms_)) return;
    hover_pending_ = false;
    HANDLE process = nullptr;
    if (LaunchPreview(path, &process)) {
        hover_process_ = process;
        hover_shown_ = true;
        hover_shown_at_ = now;
        diagnostics::runtime::Event("quick_look_hover", {{"previewed", 1}, {"error", 0}});
    }
}

// Marked injected, so the hook passes it straight through: a Space we swallowed
// that turned out to have nothing to preview still reaches Explorer.
void QuickLookHotkey::ReinjectSpace() {
    reinjecting_ = true;
    INPUT input[2]{};
    input[0].type = input[1].type = INPUT_KEYBOARD;
    input[0].ki.wVk = input[1].ki.wVk = VK_SPACE;
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, input, sizeof(INPUT));
    reinjecting_ = false;
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
    // Hover polling uses a timer on this thread's (hidden) message queue.
    SetTimer(nullptr, kHoverTimer, kHoverPollMs, nullptr);
    while (self.hook_ && !self.stopping_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!message.hwnd && message.message == kPreviewMessage) {
            if (!self.Preview(reinterpret_cast<HWND>(message.wParam))) self.ReinjectSpace();
        } else if (!message.hwnd && message.message == WM_TIMER && message.wParam == kHoverTimer) {
            self.CheckHover();
        } else {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    KillTimer(nullptr, kHoverTimer);
    self.CloseHoverPreview();
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
    // Injected keys are not the user's: automation, an on-screen keyboard or our
    // own ReinjectSpace. The diagnostics variable opts them in so a scripted run
    // can drive the same path; the reinjection above always stays out.
    if ((key.flags & LLKHF_INJECTED) && !(self->accept_injected_ && !self->reinjecting_))
        return CallNextHookEx(nullptr, code, message, data);
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
