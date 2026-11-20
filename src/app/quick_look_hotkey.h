#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>

namespace pulse::app {
// Space bar preview for File Explorer and the desktop: with a selection in an
// Explorer window (or on the desktop) a plain Space press opens the Quick Look
// window instead of requiring the right-click verb. The hook lives on its own
// message thread, exactly like WinEHotkey, and stays out of every other app.
class QuickLookHotkey {
public:
    QuickLookHotkey();
    ~QuickLookHotkey();
    QuickLookHotkey(const QuickLookHotkey&) = delete;
    QuickLookHotkey& operator=(const QuickLookHotkey&) = delete;
    void Stop();
    bool Active() const noexcept { return active_.load(); }
private:
    static DWORD WINAPI Thread(void* value);
    static LRESULT CALLBACK Keyboard(int code, WPARAM message, LPARAM data);
    bool Preview(HWND foreground);
    void CheckHover();
    void CloseHoverPreview();
    void ReinjectSpace();
    HANDLE thread_ = nullptr;
    std::atomic<DWORD> thread_id_{0};
    std::atomic<bool> stopping_{false}, active_{false};
    HHOOK hook_ = nullptr;
    bool space_down_ = false, swallowed_ = false;
    // Diagnostics only: PULSE_TEST_QUICK_LOOK_KEYS=1 lets injected keys through
    // so an automated run can drive the Space path. Our own reinjection is
    // excluded either way, and normal runs never set the variable.
    bool accept_injected_ = false;
    bool reinjecting_ = false;
    // Hover preview state, owned by the hook thread.
    HANDLE hover_process_ = nullptr;
    int hover_delay_ms_ = 500;
    std::wstring hover_path_;
    POINT hover_anchor_{0, 0};
    ULONGLONG hover_moved_at_ = 0; // tick when the cursor entered the current item
    ULONGLONG hover_shown_at_ = 0; // tick when the preview process was started
    bool hover_pending_ = false;   // cursor settled on an item, delay running
    bool hover_shown_ = false;     // a preview process is alive for that item
    // The desktop answers through the accessibility tree, which is far too slow
    // to query on every poll; the answer is reused while the cursor stays put.
    POINT hover_query_point_{0, 0};
    std::wstring hover_query_path_;
    inline static thread_local QuickLookHotkey* current_ = nullptr;
};
} // namespace pulse::app
