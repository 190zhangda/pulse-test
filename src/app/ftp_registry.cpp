#include "ftp_registry.h"

#include <algorithm>
#include <cwctype>

#include "../common/runtime_log.h"
#include "../fs/ftp_url.h"

namespace pulse::app {
namespace {

// An FTP row's path is the canonical URL the browser navigates by: no
// password, the port only when it is not 21.
std::wstring EntryPathFor(const fs::FtpUrl& base, const std::wstring& name) {
    fs::FtpUrl child = base;
    child.path = fs::FtpJoinPath(base.path, name);
    return fs::FtpCanonicalText(child);
}

FILETIME FileTimeOf(uint64_t value) {
    FILETIME time{};
    time.dwLowDateTime = static_cast<DWORD>(value & 0xFFFFFFFFu);
    time.dwHighDateTime = static_cast<DWORD>(value >> 32);
    return time;
}

}  // namespace

FtpRegistry& FtpRegistry::Instance() {
    static FtpRegistry registry;
    return registry;
}

std::wstring FtpRegistry::KeyFor(const fs::FtpUrl& url) {
    std::wstring key = url.host;
    for (wchar_t& c : key) c = static_cast<wchar_t>(towlower(c));
    key.push_back(L':');
    key += std::to_wstring(url.port);
    return key;
}

FtpError FtpRegistry::MapStatus(fs::FtpStatus status) {
    switch (status) {
    case fs::FtpStatus::Ok: return FtpError::None;
    case fs::FtpStatus::Auth: return FtpError::Auth;
    case fs::FtpStatus::NotFound: return FtpError::NotFound;
    case fs::FtpStatus::Timeout: return FtpError::Timeout;
    case fs::FtpStatus::Cancelled: return FtpError::Cancelled;
    case fs::FtpStatus::Denied: return FtpError::Denied;
    case fs::FtpStatus::LocalIo: return FtpError::Network;
    case fs::FtpStatus::Network: return FtpError::Network;
    case fs::FtpStatus::Protocol: return FtpError::Network;
    }
    return FtpError::Network;
}

void FtpRegistry::Remember(const fs::FtpUrl& url) {
    if (!url.valid) return;
    std::lock_guard lock(mutex_);
    Server& server = *servers_[KeyFor(url)];
    server.host = url.host;
    server.port = url.port;
    if (!url.user.empty()) server.user = url.user;
    if (!url.password.empty()) server.password = url.password;
}

std::shared_ptr<FtpRegistry::Server> FtpRegistry::ServerFor(const fs::FtpUrl& url) {
    const std::wstring key = KeyFor(url);
    std::shared_ptr<Server>& slot = servers_[key];
    if (!slot) slot = std::make_shared<Server>();
    slot->host = url.host;
    slot->port = url.port;
    return slot;
}

FtpError FtpRegistry::Download(const std::wstring& url, const std::wstring& local,
                               const std::function<bool()>& cancelled) {
    fs::FtpUrl parsed;
    if (!fs::ParseFtpUrl(url, parsed)) return FtpError::BadUrl;

    std::lock_guard lock(mutex_);
    const std::shared_ptr<Server> server = ServerFor(parsed);
    std::wstring detail;
    if (!server->logged_in) {
        const fs::FtpStatus connected =
            server->client.Connect(server->host, server->port, server->user, server->password, &detail);
        if (connected != fs::FtpStatus::Ok) {
            server->client.Close();
            return MapStatus(connected);
        }
        server->logged_in = true;
    }
    const fs::FtpStatus fetched = server->client.Download(parsed.path, local, cancelled, {}, &detail);
    if (fetched != fs::FtpStatus::Ok) {
        if (fetched != fs::FtpStatus::Cancelled) {
            server->client.Close();
            server->logged_in = false;
        }
        diagnostics::runtime::TextEvent("ftp_download_failed", {{"path", parsed.path}},
                                        {{"status", static_cast<uint64_t>(fetched)}});
        return MapStatus(fetched);
    }
    diagnostics::runtime::TextEvent("ftp_downloaded", {{"path", parsed.path}}, {{"ok", 1}});
    return FtpError::None;
}
bool FtpRegistry::FolderWritable(const std::wstring& url) const {
    fs::FtpUrl parsed;
    return fs::ParseFtpUrl(url, parsed);
}

void FtpRegistry::DropAll() {
    std::lock_guard lock(mutex_);
    for (auto& [key, server] : servers_) {
        if (server) server->client.Close();
    }
    servers_.clear();
}

FtpError FtpRegistry::ListFolder(const std::wstring& url, std::vector<fs::DirEntry>& out,
                                 const std::function<bool()>& cancelled) {
    fs::FtpUrl parsed;
    if (!fs::ParseFtpUrl(url, parsed)) return FtpError::BadUrl;
    if (cancelled && cancelled()) return FtpError::Cancelled;

    std::lock_guard lock(mutex_);
    const std::shared_ptr<Server> server = ServerFor(parsed);
    if (cancelled && cancelled()) return FtpError::Cancelled;
    std::wstring detail;
    if (!server->logged_in) {
        const fs::FtpStatus connected =
            server->client.Connect(server->host, server->port, server->user, server->password, &detail);
        if (connected != fs::FtpStatus::Ok) {
            diagnostics::runtime::TextEvent("ftp_connect_failed", {{"host", server->host}},
                                            {{"status", static_cast<uint64_t>(connected)}});
            server->client.Close();
            return MapStatus(connected);
        }
        server->logged_in = true;
        diagnostics::runtime::Event("ftp_connected", {{"port", static_cast<uint64_t>(server->port)}});
    }

    std::vector<fs::FtpItem> items;
    const fs::FtpStatus listed = server->client.List(parsed.path, items, cancelled, &detail);
    if (listed != fs::FtpStatus::Ok) {
        // A failed listing may leave the control connection out of step.
        if (listed != fs::FtpStatus::NotFound) {
            server->client.Close();
            server->logged_in = false;
        }
        diagnostics::runtime::TextEvent("ftp_list_failed", {{"host", server->host}},
                                        {{"status", static_cast<uint64_t>(listed)}});
        return MapStatus(listed);
    }

    out.reserve(items.size());
    for (const fs::FtpItem& item : items) {
        fs::DirEntry entry;
        entry.full_path = EntryPathFor(parsed, item.name);
        entry.name = item.name;
        entry.is_dir = item.is_dir;
        entry.size = item.is_dir ? 0 : item.size;
        entry.attrs = item.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        entry.mtime = item.has_mtime ? item.mtime : FileTimeOf(0);
        entry.ctime = entry.mtime;
        entry.atime = entry.mtime;
        out.push_back(std::move(entry));
    }
    diagnostics::runtime::Event("ftp_listed",
                                {{"entries", static_cast<uint64_t>(out.size())},
                                 {"port", static_cast<uint64_t>(server->port)}});
    return FtpError::None;
}

}  // namespace pulse::app
