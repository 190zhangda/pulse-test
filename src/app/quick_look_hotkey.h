#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace pulse::app {
// Space bar preview for File Explorer and the desktop: with a selection in an
// Explorer window (or on the desktop) a plain Space press opens the Quick Look
// window instead of requiring the right-click verb. The hook lives on its own
// message thread, exactly like WinEHotkey, and stays out of every other app.
//
// Shutdown must never wait for that thread: the update installer blocks inside
// DestroyWindow while it polls the shutdown handshake, and the hook thread may be
// inside a shell COM call at that moment, so joining it there turned every update
// into a timeout. Stop() therefore unhooks the keyboard callback immediately (no
// callback can run afterwards) and asks the thread to leave; the thread keeps its
// own reference to this object and releases it when it has finished its teardown,
// which is what finally destroys the object.
class QuickLookHotkey : public std::enable_shared_from_this<QuickLookHotkey> {
public:
    // The two halves of the feature have their own switch: Space and the pointer
    // resting on a row. Start() takes the initial pair, SetModes() follows later
    // edits without restarting the thread.
    static std::shared_ptr<QuickLookHotkey> Start(bool space, bool hover);
    void SetModes(bool space, bool hover);
    ~QuickLookHotkey();
    QuickLookHotkey(const QuickLookHotkey&) = delete;
    QuickLookHotkey& operator=(const QuickLookHotkey&) = delete;
    void Stop();
    bool Active() const noexcept { return active_.load(); }
private:
    QuickLookHotkey();
    static DWORD WINAPI Thread(void* value);
    static LRESULT CALLBACK Keyboard(int code, WPARAM message, LPARAM data);
    bool Preview(HWND foreground);
    void CheckHover();
    void CloseSpacePreview();
    void CloseHoverPreview();
    void CloseAllPreviews();
    void ReinjectSpace();
    // Desktop icon organisers publish the icon the user acted on through their
    // own channel, not through the shell: Coodesker sends the path to a window of
    // class "SeerWindowClass" as WM_COPYDATA. This window is where that arrives.
    void CreateOrganiserWindow();
    void DestroyOrganiserWindow();
    void PreviewOrganiserPath();
    // Close whatever is showing and open this path as the Space preview.
    bool ShowSpacePreview(const std::wstring& path);
    static LRESULT CALLBACK OrganiserWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    HANDLE thread_ = nullptr;
    std::atomic<DWORD> thread_id_{0};
    std::atomic<bool> stopping_{false}, active_{false};
    // Cleared under exchange so Stop() and the thread's own teardown never both
    // unhook the same handle.
    std::atomic<HHOOK> hook_{nullptr};
    bool space_down_ = false, swallowed_ = false;
    // Diagnostics only: PULSE_TEST_QUICK_LOOK_KEYS=1 lets injected keys through
    // so an automated run can drive the Space path. Our own reinjection is
    // excluded either way, and normal runs never set the variable.
    bool accept_injected_ = false;
    bool reinjecting_ = false;
    // Hover preview state, owned by the hook thread. space_process_ belongs to
    // Space, hover_process_ to the pointer: closing one must never touch the
    // other, because a Space press used to be filed in the hover slot and was
    // then dismissed by the very next hover poll.
    HANDLE space_process_ = nullptr;
    HANDLE hover_process_ = nullptr;
    std::wstring space_path_;
    ULONGLONG space_shown_at_ = 0;
    std::atomic<bool> space_mode_{true}, hover_mode_{true};
    // The organiser's WM_COPYDATA payload, filled by the window procedure and read
    // by the hook thread's own loop, so no preview is ever started inside a
    // message sent by another process.
    HWND organiser_window_ = nullptr;
    std::wstring organiser_path_;
    // Set while a desktop Space press waits for the organiser's message: the
    // shell's own idea of the desktop selection is stale under an organiser, so it
    // is only used when the message never arrives.
    ULONGLONG desktop_wait_until_ = 0;
    ULONGLONG desktop_wait_press_at_ = 0;
    HWND desktop_wait_foreground_ = nullptr;
    // WM_COPYDATA arrives on the hook thread's own message loop, but the state it
    // fills is read from that same thread's loop afterwards; the lock keeps the
    // window procedure honest if the organiser ever sends from another thread.
    std::mutex organiser_lock_;
    int hover_delay_ms_ = 500;
    std::wstring hover_path_;
    POINT hover_anchor_{0, 0};
    ULONGLONG hover_moved_at_ = 0; // tick when the cursor entered the current item
    ULONGLONG hover_shown_at_ = 0; // tick when the preview process was started
    bool hover_pending_ = false;   // cursor settled on an item, delay running
    bool hover_shown_ = false;     // a preview process is alive for that item
    inline static thread_local QuickLookHotkey* current_ = nullptr;
};
} // namespace pulse::app
