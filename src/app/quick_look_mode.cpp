#include "quick_look_mode.h"
#include "app_prefs.h"
#include "picker_appearance.h"
#include "picker_appearance_ui.h"
#include "session.h"
#include "../common/json_utils.h"
#include "../common/localization.h"
#include "../common/utf8_file.h"
#include "../ui/FluentTokens.h"
#include "../ui/quick_preview_command.h"
#include "../ui/quick_preview_window.h"
#include "../ui/typography.h"
#include "../ui/window_effect_support.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <string>

namespace pulse::app {
namespace {
// The preview window posts these to its owner. They stay far above the
// WM_APP values app_messages.h already assigns.
constexpr UINT kQuickLookOpenMessage = WM_APP + 302;
constexpr UINT_PTR kQuickLookWatchTimer = 1;
constexpr UINT kQuickLookWatchIntervalMs = 120;

struct QuickLookHost {
    ui::QuickPreviewWindow* preview = nullptr;
    ULONGLONG shown_at = 0;
    bool done = false;
};

LRESULT CALLBACK HostProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* host = reinterpret_cast<QuickLookHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    case kQuickLookOpenMessage:
        if (wparam == 0) {
            // Open/close: the window only hides itself, so end the process here.
            if (host) host->done = true;
        } else if (host && host->preview) {
            // A folder preview picked an entry: hand it to the shell.
            const std::wstring target = host->preview->TakeOpenPath();
            if (!target.empty()) ShellExecuteW(hwnd, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    case WM_TIMER:
        // Esc and the close button hide the preview without telling the owner.
        // The grace period covers the first show, which happens asynchronously.
        if (host && host->preview && !host->preview->visible() &&
            GetTickCount64() - host->shown_at > 1000) {
            host->done = true;
        }
        return 0;
    case WM_CLOSE:
        if (host) host->done = true;
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

bool RegisterHostClass(HINSTANCE instance) {
    WNDCLASSEXW host_class{sizeof(host_class)};
    host_class.hInstance = instance;
    host_class.lpfnWndProc = HostProc;
    host_class.lpszClassName = L"PulseQuickLookHost";
    return RegisterClassExW(&host_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

// The preview centres itself on the owner rectangle, so the owner covers the
// monitor the user is working on.
RECT HostBounds() {
    POINT cursor{0, 0};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(monitor, &info)) return info.rcMonitor;
    RECT bounds{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    return bounds;
}

ui::QuickPreviewItem MakeItem(const std::wstring& path, const WIN32_FILE_ATTRIBUTE_DATA& data) {
    ui::QuickPreviewItem item;
    item.path = path;
    const size_t slash = path.find_last_of(L"\\/");
    item.name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (item.name.empty()) item.name = path;
    item.attrs = data.dwFileAttributes;
    item.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    item.modified = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                    data.ftLastWriteTime.dwLowDateTime;
    item.read_only = (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    item.position = 0;
    item.count = 1;
    return item;
}

// Quick Look options live in app.json; AppPrefs::Load is avoided here because it
// repairs integration state that belongs to the main application.
ui::QuickPreviewOptions ReadOptions(const std::wstring& json) {
    ui::QuickPreviewOptions options;
    options.size_mode = NormalizePreviewSize(pulse::json::ExtractInt(json, L"preview_size", kPreviewSizeFit));
    options.remember_size = pulse::json::ExtractBool(json, L"preview_remember_size", true);
    const int x = pulse::json::ExtractInt(json, L"preview_rect_x", 0);
    const int y = pulse::json::ExtractInt(json, L"preview_rect_y", 0);
    const int w = (std::max)(0, pulse::json::ExtractInt(json, L"preview_rect_w", 0));
    const int h = (std::max)(0, pulse::json::ExtractInt(json, L"preview_rect_h", 0));
    if (w > 0 && h > 0) options.remembered = RECT{x, y, x + w, y + h};
    options.space_closes = pulse::json::ExtractBool(json, L"preview_space_closes", true);
    options.loop = NormalizePreviewLoop(pulse::json::ExtractInt(json, L"preview_loop", kPreviewLoopAnimations));
    options.key_hint_left = (std::clamp)(pulse::json::ExtractInt(json, L"preview_key_hint_left", 3), 0, 3);
    return options;
}
} // namespace

int RunQuickLookMode(const wchar_t* path) {
    if (!path || !*path) return 2;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    const std::wstring target(path);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &data)) return 2;

    // Presentation settings, read-only: language, theme, accent, fonts.
    const auto appearance = ReadUserPickerAppearanceReadOnly().snapshot;
    l10n::Initialize(GetModuleHandleW(nullptr), appearance.language);
    ui::typography::SetUiFontScale(appearance.ui_font_scale);
    ui::typography::SetTextRenderMode(static_cast<ui::typography::TextRenderMode>(appearance.text_render));
    const auto colors = ResolveSystemPickerAppearanceColors(appearance);

    std::wstring json;
    ReadUtf8File(GetPulseDataDir() + L"\\app.json", json);
    const ui::QuickPreviewOptions options = ReadOptions(json);
    const ui::WindowEffect effect = ui::SupportedWindowEffect(
        ui::WindowEffectFromId(pulse::json::ExtractString(json, L"window_effect", L"mica-alt")));

    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!RegisterHostClass(instance)) return 3;

    QuickLookHost host;
    const RECT bounds = HostBounds();
    // The host must be visible for the preview it owns to be visible, so it is a
    // fully transparent, click-through layered window rather than a hidden one.
    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                                 L"PulseQuickLookHost", L"", WS_POPUP, bounds.left, bounds.top,
                                 bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr,
                                 instance, &host);
    if (!owner) return 3;
    SetLayeredWindowAttributes(owner, 0, 0, LWA_ALPHA);
    ShowWindow(owner, SW_SHOWNOACTIVATE);

    int result = 0;
    {
        ui::QuickPreviewWindow preview;
        host.preview = &preview;
        if (!preview.Initialize(owner, WM_APP + 301, kQuickLookOpenMessage)) {
            result = 3;
        } else {
            preview.SetAccent(ui::HexColor(colors.accent_rgb));
            preview.SetOptions(options);
            // safe_mode stays off so Office and PDF previews use their hosted
            // handlers; without a command message the window shows no file verbs.
            preview.Show(MakeItem(target, data), colors.dark, effect, /*safe_mode=*/false);
            host.shown_at = GetTickCount64();
            SetTimer(owner, kQuickLookWatchTimer, kQuickLookWatchIntervalMs, nullptr);
            MSG msg{};
            while (!host.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            KillTimer(owner, kQuickLookWatchTimer);
            preview.Close();
        }
        host.preview = nullptr;
    }
    DestroyWindow(owner);
    return result;
}
} // namespace pulse::app
