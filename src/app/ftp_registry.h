// ftp_registry.h - The FTP servers this session is talking to.
//
// One FtpClient per server (host + port), connected on first use and kept
// while the app runs; a listing that fails on the wire drops the connection so
// the next attempt reconnects. The registry runs on the worker thread
// (WorkerPool), like DeviceRegistry does for phones, so its calls block.
//
// Credentials come from the URL the user typed (ftp://user:password@host/dir)
// and stay in memory for the session; saved servers are a later stage.
#pragma once
#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../fs/fs_enum.h"
#include "../fs/ftp_client.h"
#include "../fs/ftp_url.h"

namespace pulse::app {

enum class FtpError : uint8_t {
    None,
    BadUrl,     // the path is not an ftp:// URL Pulse can use
    Auth,       // the server refused the user name or the password
    NotFound,   // the folder is not there
    Timeout,    // no answer within the budget
    Network,    // the connection failed or dropped
    Denied,     // the server refused the command
    Cancelled,
};

// The rows of an ftp:// folder plus the listing status, in one call.
class FtpRegistry {
public:
    static FtpRegistry& Instance();

    // Remembers the credentials of the server in `url` (the address bar passes
    // them once; the paths the browser navigates by carry no password).
    void Remember(const fs::FtpUrl& url);

    // Any thread but the worker: the rows of an ftp:// folder. `cancelled` is
    // polled between replies and during a transfer.
    FtpError ListFolder(const std::wstring& url, std::vector<fs::DirEntry>& out,
                        const std::function<bool()>& cancelled);
    // A folder that takes a paste (an FTP folder always does: the server may
    // still refuse, which the operation reports).
    bool FolderWritable(const std::wstring& url) const;
    // Closes every connection (the app is going away).
    void DropAll();

private:
    FtpRegistry() = default;

    struct Server {
        std::wstring host;
        uint16_t port = 21;
        std::wstring user;
        std::wstring password;
        fs::FtpClient client;
        bool logged_in = false;
    };

    // The caller holds mutex_: the slot may be created, never dropped here.
    std::shared_ptr<Server> ServerFor(const fs::FtpUrl& url);
    static std::wstring KeyFor(const fs::FtpUrl& url);
    static FtpError MapStatus(fs::FtpStatus status);

    std::mutex mutex_;
    std::map<std::wstring, std::shared_ptr<Server>> servers_;
};

}  // namespace pulse::app
