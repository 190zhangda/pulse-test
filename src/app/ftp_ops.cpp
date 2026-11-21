#include "ftp_ops.h"

#include <atomic>
#include <mutex>
#include <thread>

#include "app_messages.h"
#include "ftp_registry.h"

#include "../common/runtime_log.h"
#include "../fs/ftp_url.h"

namespace pulse::app {
namespace {

std::atomic<bool> stopping{false};
std::atomic<int> active{0};  // requests still running

// The app is going away: long uploads give up between files.
bool Cancelled() { return stopping.load(); }

// The URL of `name` inside `folder` (canonical text: no password).
std::wstring ChildUrl(const std::wstring& folder, const std::wstring& name) {
    fs::FtpUrl base;
    if (name.empty() || !fs::ParseFtpUrl(folder, base)) return {};
    base.path = fs::FtpJoinPath(base.path, name);
    return fs::FtpCanonicalText(base);
}

bool IsDirectory(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring JoinLocal(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\') return dir + name;
    return dir + L'\\' + name;
}

void NoteFailure(FtpOpResult& result, FtpError error) {
    ++result.failed;
    if (result.error == 0) result.error = static_cast<uint8_t>(error);
}

void UploadFile(FtpOpResult& result, const std::wstring& local, const std::wstring& remote) {
    const FtpError error = FtpRegistry::Instance().Upload(local, remote, [] { return Cancelled(); });
    if (error == FtpError::None) ++result.done;
    else NoteFailure(result, error);
}

// A file goes up as itself; a folder is created on the server first and its
// contents follow, so the tree arrives the way it is on disk.
void UploadTree(FtpOpResult& result, const std::wstring& local, const std::wstring& remote) {
    if (Cancelled()) return;
    if (!IsDirectory(local)) {
        UploadFile(result, local, remote);
        return;
    }
    const FtpError made = FtpRegistry::Instance().MakeDirectory(remote, [] { return Cancelled(); });
    if (made != FtpError::None) {
        NoteFailure(result, made);
        return;
    }
    std::wstring base = local;
    if (!base.ends_with(L'\\')) base.push_back(L'\\');
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((base + L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring child_remote = ChildUrl(remote, name);
        if (child_remote.empty()) continue;
        const std::wstring child_local = base + name;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            UploadTree(result, child_local, child_remote);
        else
            UploadFile(result, child_local, child_remote);
        if (Cancelled()) break;
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

void DownloadFile(FtpOpResult& result, const std::wstring& remote, const std::wstring& local) {
    const FtpError error = FtpRegistry::Instance().Download(remote, local, [] { return Cancelled(); });
    if (error == FtpError::None) ++result.done;
    else NoteFailure(result, error);
}

// A remote file lands as itself; a remote folder is created locally first and
// its contents follow. A path the server will not list is tried as a file.
void DownloadTree(FtpOpResult& result, const std::wstring& remote, const std::wstring& local) {
    if (Cancelled()) return;
    std::vector<fs::DirEntry> rows;
    const FtpError listed =
        FtpRegistry::Instance().ListFolder(remote, rows, [] { return Cancelled(); });
    if (listed != FtpError::None) {
        DownloadFile(result, remote, local);
        return;
    }
    CreateDirectoryW(local.c_str(), nullptr);
    for (const fs::DirEntry& row : rows) {
        if (Cancelled()) break;
        const std::wstring child = JoinLocal(local, row.name);
        if (row.is_dir) DownloadTree(result, row.full_path, child);
        else DownloadFile(result, row.full_path, child);
    }
}

void DeleteOne(FtpOpResult& result, const std::wstring& remote, bool directory) {
    FtpRegistry& registry = FtpRegistry::Instance();
    const FtpError error = directory
        ? registry.RemoveTree(remote, [] { return Cancelled(); })
        : registry.RemoveFile(remote, [] { return Cancelled(); });
    // Gone already counts as done: the view only has to drop the row.
    if (error == FtpError::None || error == FtpError::NotFound) ++result.done;
    else NoteFailure(result, error);
}

FtpOpResult RunRequest(const FtpOpRequest& request) {
    FtpOpResult result;
    result.folder = request.folder;
    switch (request.kind) {
    case FtpOpKind::Upload:
        for (const std::wstring& local : request.sources) {
            if (Cancelled()) break;
            const std::wstring remote = ChildUrl(request.folder, fs::FtpLeafName(local));
            if (remote.empty()) {
                ++result.failed;
                continue;
            }
            UploadTree(result, local, remote);
        }
        break;
    case FtpOpKind::Download:
        for (const std::wstring& remote : request.sources) {
            if (Cancelled()) break;
            const std::wstring leaf = fs::FtpLeafName(remote);
            if (leaf.empty()) {
                ++result.failed;
                continue;
            }
            DownloadTree(result, remote, JoinLocal(request.folder, leaf));
        }
        break;
    case FtpOpKind::Delete:
        for (size_t index = 0; index < request.sources.size(); ++index) {
            if (Cancelled()) break;
            const bool directory = index < request.dirs.size() && request.dirs[index] != 0;
            DeleteOne(result, request.sources[index], directory);
        }
        break;
    case FtpOpKind::Rename: {
        if (request.sources.empty() || request.new_name.empty()) {
            ++result.failed;
            break;
        }
        const FtpError error =
            FtpRegistry::Instance().Rename(request.sources[0], request.new_name, [] { return Cancelled(); });
        if (error == FtpError::None) {
            ++result.done;
            result.created_name = request.new_name;
        } else {
            NoteFailure(result, error);
        }
        break;
    }
    case FtpOpKind::MakeDirectory: {
        const std::wstring remote = ChildUrl(request.folder, request.new_name);
        if (remote.empty()) {
            ++result.failed;
            break;
        }
        const FtpError error = FtpRegistry::Instance().MakeDirectory(remote, [] { return Cancelled(); });
        if (error == FtpError::None) {
            ++result.done;
            result.created_name = request.new_name;
        } else {
            NoteFailure(result, error);
        }
        break;
    }
    }
    return result;
}

}  // namespace

FtpOps& FtpOps::Instance() {
    static FtpOps ops;
    return ops;
}

bool FtpOps::Submit(FtpOpRequest request, HWND window) {
    if (request.folder.empty() || !window) return false;
    if (request.kind != FtpOpKind::MakeDirectory && request.sources.empty()) return false;
    if (request.kind == FtpOpKind::MakeDirectory && request.new_name.empty()) return false;

    // The caller is the window procedure, so it must not wait for a transfer
    // that is still running: every request gets its own thread. They meet on
    // the registry's mutex, which keeps one conversation per server.
    stopping.store(false);
    active.fetch_add(1);
    std::thread([request = std::move(request), window]() mutable {
        FtpOpResult done = RunRequest(request);
        diagnostics::runtime::Event("ftp_op_done", {{"done", static_cast<uint64_t>(done.done)},
                                                    {"failed", static_cast<uint64_t>(done.failed)},
                                                    {"error", static_cast<uint64_t>(done.error)}});
        if (!stopping.load()) {
            PostMessageW(window, WM_FTP_OPS_DONE, 0,
                         reinterpret_cast<LPARAM>(new FtpOpResult(std::move(done))));
        }
        active.fetch_sub(1);
    }).detach();
    return true;
}

void FtpOps::Shutdown() {
    // The app is going away: give a short transfer a moment, then leave. The
    // threads give up between files once `stopping` is set.
    stopping.store(true);
    for (int waited = 0; waited < 100 && active.load() > 0; ++waited) Sleep(50);
}

void FtpOps::SetCopied(std::vector<std::wstring> urls) {
    std::lock_guard lock(copied_mutex_);
    copied_ = std::move(urls);
}

std::vector<std::wstring> FtpOps::Copied() const {
    std::lock_guard lock(copied_mutex_);
    return copied_;
}

void FtpOps::ClearCopied() {
    std::lock_guard lock(copied_mutex_);
    copied_.clear();
}

}  // namespace pulse::app
