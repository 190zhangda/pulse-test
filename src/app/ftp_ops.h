// ftp_ops.h - Writing to an ftp:// folder: upload, delete, rename, new folder.
//
// The remote side has no file system Pulse can hand to the operations engine,
// so these run here instead: one background thread, one request at a time, and
// a message back to the window that asked (WM_FTP_OPS_DONE) so the view can
// refresh and report what went wrong.
#pragma once
#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace pulse::app {

enum class FtpOpKind : uint8_t {
    Upload,        // sources = local paths (files or folders), folder = the ftp folder
    Download,      // sources = remote URLs, folder = the local folder to fill
    Delete,        // sources = remote URLs
    Rename,        // sources[0] = remote URL, new_name = the new leaf name
    MakeDirectory  // folder + new_name: the folder to create
};

struct FtpOpRequest {
    FtpOpKind kind = FtpOpKind::Upload;
    std::wstring folder;              // the ftp folder the view shows (refresh target)
    std::vector<std::wstring> sources;
    std::vector<uint8_t> dirs;        // Delete: aligned with sources, 1 = folder
    std::wstring new_name;
};

// What the UI needs to react: which folder was written, how it went, and the
// row to select afterwards.
struct FtpOpResult {
    std::wstring folder;
    uint8_t error = 0;      // app::FtpError
    size_t done = 0;
    size_t failed = 0;
    std::wstring created_name;  // a new folder: the row to select
    // What was acted on, so a listener can drop those rows (the duplicate
    // finder removes the files it just deleted on the server).
    std::vector<std::wstring> sources;
};

// Result of the recursive download that precedes a duplicate scan of an ftp://
// folder. `generation` lets the window ignore a download that finished after
// the user cancelled or started a different scan.
struct FtpScanDownloadResult {
    std::wstring local_root;
    uint64_t generation = 0;
    uint8_t error = 0;      // app::FtpError
};

class FtpOps {
public:
    static FtpOps& Instance();

    // Runs `request` on a background thread and posts WM_FTP_OPS_DONE to
    // `window` with a heap-allocated FtpOpResult. False when the request makes
    // no sense (nothing to do), so the caller can skip arming a selection.
    bool Submit(FtpOpRequest request, HWND window);
    // Waits for the running request (the app is going away).
    void Shutdown();

    // Recursively downloads a whole ftp:// folder into `local_root` for the
    // duplicate scanner (which only reads Win32 paths). Posts
    // WM_FTP_SCAN_DOWNLOAD to `window` with a heap FtpScanDownloadResult when
    // it finishes; `generation` is echoed back so a stale result is ignored.
    // `credentials_url` must carry the password for the first connection.
    bool DownloadForScan(const std::wstring& credentials_url,
                         const std::wstring& local_root, HWND window,
                         uint64_t generation);

    // Ctrl+C on a server's rows: the URLs a later paste into a local folder
    // fetches. Pulse keeps them here because the Windows clipboard is about
    // paths that exist, and these do not.
    void SetCopied(std::vector<std::wstring> urls);
    std::vector<std::wstring> Copied() const;
    void ClearCopied();

private:
    FtpOps() = default;

    mutable std::mutex copied_mutex_;
    std::vector<std::wstring> copied_;
};

}  // namespace pulse::app
