// ftp_cache.h - Local copies of remote files.
//
// Preview, and later the shell verbs, need a path Win32 can open; an ftp://
// URL has none. FtpCache fetches a remote file once into
// %LOCALAPPDATA%\Pulse\Cache\Ftp\<url hash>\<name> and hands back the copy.
// The fetch runs on its own thread and reports through WM_FTP_CACHE_READY
// (app_messages.h) with a new FtpCacheResult in lParam: the window owns it.
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>

namespace pulse::app {

struct FtpCacheResult {
    std::wstring url;
    std::wstring local_path;   // the copy, when error == 0
    uint8_t error = 0;         // app::FtpError
    bool for_preview = false;  // the request came from a preview command
};

class FtpCache {
public:
    static FtpCache& Instance();

    // The copy already on disk for `url`, or an empty string. Cheap: no I/O
    // beyond one attribute check, so the UI thread may call it.
    std::wstring LocalCopy(const std::wstring& url);

    // Fetches `url` in the background and posts WM_FTP_CACHE_READY to
    // `window`. False when `url` is not an ftp:// URL Pulse can fetch.
    bool Request(const std::wstring& url, HWND window, bool for_preview);

    // Waits for the fetches in flight to notice the app is going away.
    void Shutdown();

private:
    FtpCache() = default;
    ~FtpCache() = default;

    // A directory per URL, so two servers cannot collide on a file name.
    static std::wstring DirectoryFor(const std::wstring& url);
    static std::wstring LeafFor(const std::wstring& url);

    std::mutex mutex_;
    std::set<std::wstring> inflight_;  // URLs a fetch is already working on
    std::atomic<int> running_{0};
    bool stopping_ = false;
};

}  // namespace pulse::app
