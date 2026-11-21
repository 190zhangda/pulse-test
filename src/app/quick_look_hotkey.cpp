#include "quick_look_hotkey.h"
#include "app_prefs.h"
#include "picker_appearance.h"
#include "../common/runtime_log.h"

#include <commctrl.h>
#include <exdisp.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <string>
#include <cstdlib>
#include <cwchar>
#include <string.h>

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

// Folder windows of the desktop shell. A third party desktop organiser (which
// draws the icons in its own window and leaves the shell's list view empty) is
// recognised by this class name as well, so both the icon layer it draws and
// the shell's own list view count as the desktop.
bool IsDesktopBackgroundClass(const wchar_t* name) {
    return name && (wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0 ||
        wcsstr(name, L"DefView") != nullptr || wcsstr(name, L"SHELLDLL_") != nullptr ||
        wcsstr(name, L"Coodesker") != nullptr);
}

Surface Classify(HWND window) {
    if (!window) return Surface::None;
    const HWND root = GetAncestor(window, GA_ROOT);
    if (!root) return Surface::None;
    wchar_t name[64]{};
    if (!GetClassNameW(root, name, ARRAYSIZE(name))) return Surface::None;
    if (wcscmp(name, L"CabinetWClass") == 0 || wcscmp(name, L"ExploreWClass") == 0) return Surface::Explorer;
    if (IsDesktopBackgroundClass(name)) return Surface::Desktop;
    return Surface::None;
}

// The desktop's own window is not always the root of what the cursor is on: an
// icon organiser may draw into a window of its own that sits in the desktop's
// process, so the ancestors and then the owning process are consulted too.
Surface ClassifyAt(HWND window) {
    if (const Surface direct = Classify(window); direct != Surface::None) return direct;
    for (HWND current = window; current; current = GetAncestor(current, GA_PARENT)) {
        wchar_t name[64]{};
        if (!GetClassNameW(current, name, ARRAYSIZE(name))) break;
        if (IsDesktopBackgroundClass(name)) return Surface::Desktop;
        if (wcscmp(name, L"CabinetWClass") == 0 || wcscmp(name, L"ExploreWClass") == 0) return Surface::Explorer;
    }
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
    std::wstring path;     // the folder itself, when no shell view could be had
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
        // FindWindowSW answers RPC_E_* on machines where the desktop's browser
        // is not registered with the running shell (or is owned by an organiser's
        // substitute). Without the view the item under the cursor cannot be named
        // at all, so fall back to a listing of the desktop folder itself.
        ShellView fallback;
        PWSTR folder = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &folder)) && folder) {
            fallback.window = nullptr;
            fallback.path = folder;
            CoTaskMemFree(folder);
        }
        return fallback;
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

// First selected item of the view, in view order. When the view reports no
// focused item this is the item Space acts on.
std::wstring SelectedPathOf(const ComPtr<IFolderView2>& view) {
    ComPtr<IShellItemArray> items;
    if (!view || view->GetSelection(FALSE, &items) != S_OK || !items) return {};
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

// The desktop folder itself (never a virtual folder such as This PC).
std::wstring DesktopFolderPath() {
    PWSTR folder = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &folder)) && folder) result = folder;
    if (folder) CoTaskMemFree(folder);
    return result;
}

// Window that publishes the desktop's icons to the accessibility tree. The
// classic def view keeps its items in a SysListView32, which an icon organiser
// leaves empty; the icons the user sees live under the DirectUIHWND that the
// shell also creates inside a second def view. That one is preferred, so the
// search walks every desktop def view for a DirectUIHWND instead of taking the
// first one it meets.
BOOL CALLBACK FindDirectUIHost(HWND window, LPARAM found) {
    wchar_t name[64]{};
    if (!GetClassNameW(window, name, ARRAYSIZE(name))) return TRUE;
    if (wcscmp(name, L"DirectUIHWND") == 0) {
        *reinterpret_cast<HWND*>(found) = window;
        return FALSE;
    }
    return TRUE;
}

HWND DirectUIHostUnder(HWND parent) {
    HWND found = nullptr;
    EnumChildWindows(parent, FindDirectUIHost, reinterpret_cast<LPARAM>(&found));
    return found;
}

HWND DesktopItemHost() {
    HWND def_view = nullptr;
    for (HWND worker = FindWindowExW(nullptr, nullptr, L"WorkerW", nullptr); worker;
         worker = FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) {
        for (HWND view = FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr); view;
             view = FindWindowExW(worker, view, L"SHELLDLL_DefView", nullptr)) {
            if (!def_view) def_view = view;
            if (HWND list = DirectUIHostUnder(view)) return list;
        }
    }
    if (HWND view = FindWindowExW(nullptr, nullptr, L"SHELLDLL_DefView", nullptr)) {
        if (!def_view) def_view = view;
        if (HWND list = DirectUIHostUnder(view)) return list;
    }
    return def_view;
}

// Accessibility names drop a known extension ("notes" for notes.txt), so the
// name is matched against the folder's entries as a whole and as a stem.
std::wstring ResolveDesktopName(const std::wstring& folder, const std::wstring& name) {
    if (name.empty()) return {};
    const std::wstring direct = folder + L"\\" + name;
    if (GetFileAttributesW(direct.c_str()) != INVALID_FILE_ATTRIBUTES) return direct;
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return {};
    std::wstring result;
    do {
        if (entry.cFileName[0] == L'.' && (entry.cFileName[1] == 0 || entry.cFileName[1] == L'.')) continue;
        std::wstring candidate = entry.cFileName;
        const size_t dot = candidate.find_last_of(L'.');
        const std::wstring stem = dot == std::wstring::npos ? candidate : candidate.substr(0, dot);
        if (_wcsicmp(candidate.c_str(), name.c_str()) == 0 || _wcsicmp(stem.c_str(), name.c_str()) == 0) {
            result = folder + L"\\" + candidate;
            break;
        }
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    return result;
}

// The desktop's selected icon, read from the shell's own accessibility tree. An
// icon organiser may draw the icons in its own window and leave the classic list
// control empty, so that control cannot answer which icon Space acts on; the
// selected state the shell publishes can. The last selected item wins, which is
// the one a single click leaves marked.
std::wstring DesktopAccessibilityPath() {
    const HWND host = DesktopItemHost();
    if (!host) return {};
    const std::wstring folder = DesktopFolderPath();
    if (folder.empty()) return {};
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))) ||
        !automation)
        return {};
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(host, &root)) || !root) return {};
    VARIANT list_item;
    VariantInit(&list_item);
    list_item.vt = VT_I4;
    list_item.lVal = UIA_ListItemControlTypeId;
    ComPtr<IUIAutomationCondition> condition;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, list_item, &condition)) || !condition)
        return {};
    ComPtr<IUIAutomationElementArray> items;
    if (FAILED(root->FindAll(TreeScope_Descendants, condition.Get(), &items)) || !items) return {};
    int count = 0;
    if (FAILED(items->get_Length(&count)) || count <= 0) return {};
    std::wstring selected;
    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> item;
        if (FAILED(items->GetElement(i, &item)) || !item) continue;
        ComPtr<IUnknown> raw;
        if (FAILED(item->GetCurrentPattern(UIA_SelectionItemPatternId, &raw)) || !raw) continue;
        ComPtr<IUIAutomationSelectionItemPattern> selection;
        if (FAILED(raw.As(&selection)) || !selection) continue;
        BOOL is_selected = FALSE;
        if (FAILED(selection->get_CurrentIsSelected(&is_selected)) || !is_selected) continue;
        BSTR name = nullptr;
        if (FAILED(item->get_CurrentName(&name)) || !name) continue;
        const std::wstring text(name);
        SysFreeString(name);
        if (const std::wstring path = ResolveDesktopName(folder, text); !path.empty()) selected = path;
    }
    return selected;
}

// Desktop item path through the shell's own folder view. The running shell
// still registers that view on most systems, which is the cheapest answer; a
// substitute desktop may not register it at all.
std::wstring DesktopShellViewPath() {
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows))) || !windows)
        return {};
    VARIANT location;
    VariantInit(&location);
    VARIANT root;
    VariantInit(&root);
    long hwnd = 0;
    ComPtr<IDispatch> dispatch;
    if (FAILED(windows->FindWindowSW(&location, &root, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &dispatch)) || !dispatch)
        return {};
    ComPtr<IServiceProvider> provider;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> shell_view;
    ComPtr<IFolderView2> view;
    if (FAILED(dispatch.As(&provider)) || !provider) return {};
    if (FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) || !browser) return {};
    if (FAILED(browser->QueryActiveShellView(&shell_view)) || !shell_view) return {};
    if (FAILED(shell_view.As(&view)) || !view) return {};
    std::wstring path = FocusedPath(view);
    if (!path.empty()) return path;
    return SelectedPathOf(view);
}

// Item of a shell view that a preview should show, as a filesystem path.
// LVM_HITTEST cannot be used to answer "which item is under the cursor": the
// list-view messages live in the WM_USER range, which the system does not marshal
// across processes, so sending one from here writes into the shell's address
// space and answers garbage. Some shells replace the item list altogether and
// publish no geometry, which leaves no truthful point-to-item mapping at all. The
// item the view has focused - the one a click leaves marked - is therefore what a
// hover preview shows.
std::wstring PathAtPoint(HWND view_window, const ComPtr<IFolderView2>& view, POINT cursor) {
    (void)view_window;
    (void)cursor;
    if (!view) return {};
    if (std::wstring focused = FocusedPath(view); !focused.empty()) return focused;
    return SelectedPathOf(view);
}

// Read the desktop's selected item inside the shell's own folder view. A shell
// that replaces the item list can still answer here, and the focus it keeps is
// the one the user sees.
std::wstring DesktopFolderViewPath() {
    ComPtr<IShellFolder> folder;
    if (FAILED(SHGetDesktopFolder(folder.GetAddressOf())) || !folder) return {};
    ComPtr<IShellView> shell_view;
    if (FAILED(folder->CreateViewObject(GetDesktopWindow(), IID_PPV_ARGS(&shell_view))) || !shell_view) return {};
    ComPtr<IFolderView2> view;
    if (FAILED(shell_view.As(&view)) || !view) return {};
    return PathAtPoint(nullptr, view, POINT{});
}

// Desktop item for a preview: the accessibility tree first, because the desktop's
// item list is exactly what an icon organiser replaces. Every tier is a different
// source of the same selection, so the answer never depends on a coordinate.
std::wstring DesktopSpacePath() {
    if (std::wstring path = DesktopAccessibilityPath(); !path.empty()) {
        diagnostics::runtime::Event("quick_look_desktop", {{"tier", 1}});
        return path;
    }
    if (std::wstring path = DesktopShellViewPath(); !path.empty()) {
        diagnostics::runtime::Event("quick_look_desktop", {{"tier", 2}});
        return path;
    }
    if (std::wstring path = DesktopFolderViewPath(); !path.empty()) {
        diagnostics::runtime::Event("quick_look_desktop", {{"tier", 3}});
        return path;
    }
    diagnostics::runtime::Event("quick_look_desktop", {{"tier", 0}});
    return {};
}

int ReadHoverDelayMs() {
    return NormalizeHoverPreviewDelay(ReadUserPickerAppearanceReadOnly().snapshot.hover_preview_delay_ms);
}

// First selected item of the shell view, as a filesystem path. Empty when
// nothing is selected or the selection has no filesystem location (a virtual
// folder such as This PC).
std::wstring SelectedPath(HWND foreground, Surface surface) {
    // The desktop is answered by its selection, never by a shell view: an icon
    // organiser leaves that view without items while the selection stays readable.
    if (surface == Surface::Desktop) return DesktopSpacePath();
    ShellView shell = FindView(foreground, surface);
    if (!shell.folder) return {};
    if (std::wstring focused = FocusedPath(shell.folder); !focused.empty()) return focused;
    return SelectedPathOf(shell.folder);
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
}

std::shared_ptr<QuickLookHotkey> QuickLookHotkey::Start() {
    std::shared_ptr<QuickLookHotkey> hotkey(new QuickLookHotkey());
    // The hook thread holds its own reference, so the object outlives Stop() and
    // is destroyed by the thread once that thread has finished unwinding.
    HANDLE thread = CreateThread(nullptr, 0, Thread, hotkey.get(), 0, nullptr);
    if (!thread) {
        diagnostics::runtime::Event("quick_look_space", {{"enabled", 0}, {"error", GetLastError()}});
        return nullptr;
    }
    hotkey->thread_ = thread;
    return hotkey;
}

QuickLookHotkey::~QuickLookHotkey() {
    // Start() always leaves one reference with this thread while it runs; reaching
    // the destructor means the thread already dropped it, so only the handle and a
    // possibly lingering preview process are left to clean up.
    if (thread_) CloseHandle(thread_);
    if (hover_process_) {
        TerminateProcess(hover_process_, 0);
        CloseHandle(hover_process_);
    }
}

void QuickLookHotkey::Stop() {
    stopping_ = true;
    // Unhook here rather than on the hook thread: a low level keyboard hook stops
    // producing callbacks the moment it is removed, which is what has to be true
    // before this object can be left to die. The thread then only has to release
    // its COM apartment and its reference.
    if (HHOOK hook = hook_.exchange(nullptr)) UnhookWindowsHookEx(hook);
    active_ = false;
    const auto id = thread_id_.load();
    if (id) PostThreadMessageW(id, WM_QUIT, 0, 0);
    // No join: the caller is a window procedure replying to the installer's
    // shutdown handshake, and the hook thread may be blocked in a shell COM call.
}

bool QuickLookHotkey::Preview(HWND foreground) {
    // ClassifyAt also recognises the desktop's icon layer when an icon organiser
    // draws it in a window of its own.
    const Surface surface = ClassifyAt(foreground);
    if (surface == Surface::None) return false;
    const std::wstring path = SelectedPath(foreground, surface);
    if (path.empty()) {
        // A swallowed Space with nothing to preview is the one case a bug report
        // cannot describe from the outside, so record which surface failed.
        diagnostics::runtime::Event("quick_look_miss",
            {{"surface", static_cast<uint64_t>(static_cast<int>(surface))},
                {"desktop", surface == Surface::Desktop ? 1u : 0u}});
        return false;
    }
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

    const Surface surface = ClassifyAt(under);
    if (surface == Surface::None) {
        CloseHoverPreview();
        return;
    }
    // Hover previews only where a point can be mapped onto an item truthfully. An
    // icon organiser replaces the desktop's item list and publishes no geometry, so
    // the desktop offers Space only; its selection is read there instead.
    if (surface == Surface::Desktop) {
        CloseHoverPreview();
        return;
    }
    const HWND root = GetAncestor(under, GA_ROOT);
    const ShellView shell = FindView(root, surface);
    const std::wstring path = PathAtPoint(shell.window, shell.folder, POINT{cursor.x, cursor.y});
    if (path.empty()) {
        CloseHoverPreview();
        // Nothing under the cursor is expected most of the time (empty space, gap
        // between rows), so this only records that the surface was searched.
        diagnostics::runtime::Event("quick_look_hover_miss",
            {{"surface", static_cast<uint64_t>(static_cast<int>(surface))}});
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
    // Hold a reference for the whole thread: Stop() never joins, so the object
    // must stay alive until this function has finished its own teardown.
    std::shared_ptr<QuickLookHotkey> keep = static_cast<QuickLookHotkey*>(value)->shared_from_this();
    auto& self = *keep;
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
    // Stop() may have detached before this point; only publish the hook when it
    // has not, so the two never disagree about who unhooks.
    HHOOK created = SetWindowsHookExW(WH_KEYBOARD_LL, Keyboard, GetModuleHandleW(nullptr), 0);
    if (self.stopping_) {
        if (created) UnhookWindowsHookEx(created);
        CoUninitialize();
        current_ = nullptr;
        return 0;
    }
    self.hook_ = created;
    self.active_ = created != nullptr;
    diagnostics::runtime::Event("quick_look_space",
        {{"enabled", self.active_.load()}, {"error", created ? ERROR_SUCCESS : GetLastError()}});
    // Hover polling uses a timer on this thread's (hidden) message queue.
    SetTimer(nullptr, kHoverTimer, kHoverPollMs, nullptr);
    while (self.hook_.load() != nullptr && !self.stopping_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
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
    if (HHOOK hook = self.hook_.exchange(nullptr)) UnhookWindowsHookEx(hook);
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
    if (ClassifyAt(foreground) == Surface::None) return CallNextHookEx(nullptr, code, message, data);
    if (FocusIsTextInput(foreground)) return CallNextHookEx(nullptr, code, message, data);
    const auto id = self->thread_id_.load();
    if (!id) return CallNextHookEx(nullptr, code, message, data);
    self->swallowed_ = true;
    PostThreadMessageW(id, kPreviewMessage, reinterpret_cast<WPARAM>(foreground), 0);
    return 1;
}
} // namespace pulse::app
