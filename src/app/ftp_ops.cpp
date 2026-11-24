#include "ftp_ops.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "app_messages.h"
#include "ftp_registry.h"

#include "../common/runtime_log.h"
#include "../fs/ftp_url.h"

namespace pulse::app {
namespace {

std::atomic<bool> stopping{false};
std::atomic<int> active{0};  // requests still running
// The duplicate-scan mirror that is allowed to keep going. A newer scan, or
// the tab's 取消, moves it away from the running one, which then stops between
// listings, files and transfer chunks instead of holding the server.
std::atomic<uint64_t> scan_owner{0};

// The app is going away: long uploads give up between files.
bool Cancelled() { return stopping.load(); }

// True when `generation` is no longer the scan the app wants: it was cancelled
// or replaced by a newer one.
bool ScanStopped(uint64_t generation) {
    return stopping.load() || scan_owner.load() != generation;
}

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

// A folder that the account may not read, or a row that vanished between the
// listing and the transfer, must not sink the whole mirror: a phone's storage
// is full of protected folders, and one of them would otherwise abort the scan.
bool SkippableScanError(FtpError error) {
    // Timeout belongs here too: a phone on Wi-Fi answers most rows quickly and
    // then stalls on one large file. Aborting the whole mirror for that single
    // row loses everything already copied.
    return error == FtpError::NotFound || error == FtpError::Denied ||
           error == FtpError::Timeout;
}

// A phone may refuse a listing halfway through a long mirror (too many
// sessions, or the app dropped the control connection). One more attempt
// reconnects, which is worth it before the scan gives up. `NotFound` is in here
// too: some servers answer a refused folder with 550, which reads as missing.
bool RetryableListError(FtpError error) {
    return error == FtpError::Denied || error == FtpError::Network ||
           error == FtpError::Timeout || error == FtpError::NotFound;
}

// A dropped connection in the middle of a file is worth one more attempt; a
// missing file stays missing, so it is not repeated.
bool RetryableTransferError(FtpError error) {
    return error == FtpError::Network || error == FtpError::Timeout;
}

// ---- the duplicate scanner's mirror ----

// A file the listings named. The download phase needs every size before it can
// tell which rows can be duplicates.
struct ScanFile {
    std::wstring remote;
    std::wstring local;
    uint64_t size = 0;  // 0 when the listing did not say
    FILETIME mtime{};   // the server's own time, when the listing carried one
    bool has_mtime = false;
};

// What the mirror did, for the log and for the caller's report.
struct ScanStats {
    uint64_t listed = 0;      // files the listings named
    uint64_t skipped = 0;     // rows the account may not read, or that failed
    uint64_t candidates = 0;  // rows that share a size: the only possible duplicates
    uint64_t unique = 0;      // sizes no other row shares: never duplicates
    uint64_t reused = 0;      // already on disk from an earlier scan
    uint64_t fetched = 0;     // transferred this time
    uint64_t pruned = 0;      // left over from an earlier scan, now gone
};

// Sends the mirror's progress to the duplicate-scan window, throttled: a phone
// can name thousands of rows, and every one of them would otherwise repaint.
struct ScanReporter {
    HWND window = nullptr;
    uint64_t generation = 0;
    uint64_t last_post = 0;

    void Send(uint64_t listed, uint64_t files, uint64_t bytes, uint64_t total,
              bool listing_done, bool force) {
        if (!window || Cancelled()) return;
        const uint64_t now = GetTickCount64();
        if (!force && now - last_post < 200) return;
        last_post = now;
        auto* progress = new FtpScanProgress;
        progress->generation = generation;
        progress->listed = listed;
        progress->files = files;
        progress->bytes = bytes;
        progress->total = total;
        progress->listing_done = listing_done;
        if (!PostMessageW(window, WM_FTP_SCAN_PROGRESS, 0, reinterpret_cast<LPARAM>(progress)))
            delete progress;
    }
};

// Walks the ftp:// tree with listings only. A listing costs a round trip; a
// transfer costs its bytes, so knowing the whole tree first is what lets the
// download phase touch only the rows that can possibly be duplicates. `remote`
// on the first call carries the password; child URLs from ListFolder are
// canonical, which reuse the logged-in connection held by the registry.
FtpError CollectScanTree(const std::wstring& remote, const std::wstring& local,
                         ScanStats& stats, uint64_t minimum_bytes,
                         std::vector<ScanFile>& files, bool named_by_row,
                         bool named_by_caller, ScanReporter& reporter,
                         uint64_t& total_bytes) {
    const uint64_t generation = reporter.generation;
    if (ScanStopped(generation)) return FtpError::Cancelled;
    std::vector<fs::DirEntry> rows;
    FtpError listed = FtpRegistry::Instance().ListFolder(
        remote, rows, [generation] { return ScanStopped(generation); });
    if (listed != FtpError::None && RetryableListError(listed)) {
        rows.clear();
        listed = FtpRegistry::Instance().ListFolder(
            remote, rows, [generation] { return ScanStopped(generation); });
    }
    if (listed != FtpError::None) {
        // The scan root and every row a listing called a folder are folders:
        // asking for their bytes would only turn "denied" into "not found".
        if (named_by_row || !named_by_caller) return listed;
        // The caller's own argument may still name a single file, which list
        // refuses; a "not a folder" answer means it is a file to fetch.
        if (listed != FtpError::NotFound) return listed;
        ScanFile file;
        file.remote = remote;
        file.local = local;
        files.push_back(std::move(file));
        ++stats.listed;
        reporter.Send(stats.listed, 0, total_bytes, 0, false, false);
        return FtpError::None;
    }
    CreateDirectoryW(local.c_str(), nullptr);
    for (const fs::DirEntry& row : rows) {
        if (ScanStopped(generation)) return FtpError::Cancelled;
        const std::wstring child = JoinLocal(local, row.name);
        // The scanner drops anything under its minimum size, so copying such a
        // row would only spend the link's time on a file nobody looks at.
        if (!row.is_dir && minimum_bytes != 0 && row.size != 0 && row.size < minimum_bytes)
            continue;
        if (row.is_dir) {
            const FtpError error = CollectScanTree(row.full_path, child, stats, minimum_bytes,
                                                   files, true, false, reporter, total_bytes);
            if (error == FtpError::None) continue;
            if (SkippableScanError(error)) {
                ++stats.skipped;
                // A folder that could not be read leaves nothing to compare.
                RemoveDirectoryW(child.c_str());
                continue;
            }
            return error;
        }
        ScanFile file;
        file.remote = row.full_path;
        file.local = child;
        file.size = row.size;
        file.mtime = row.mtime;
        file.has_mtime = row.mtime.dwLowDateTime != 0 || row.mtime.dwHighDateTime != 0;
        total_bytes += row.size;
        files.push_back(std::move(file));
        ++stats.listed;
        reporter.Send(stats.listed, 0, total_bytes, 0, false, false);
    }
    return FtpError::None;
}

FILETIME ScanStamp() {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    return now;
}

// Marks a mirrored row as kept by this scan (its creation time becomes the
// scan's start) and, when the listing named the server's time, makes the local
// copy carry it. The next scan can then tell an unchanged row from one the
// server replaced with different bytes of the same length.
void TouchMirror(const ScanFile& file, const FILETIME& stamp) {
    HANDLE handle = CreateFileW(file.local.c_str(), FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return;
    SetFileTime(handle, &stamp, nullptr, file.has_mtime ? &file.mtime : nullptr);
    CloseHandle(handle);
}

// True when an earlier scan already left this row on disk unchanged: a second
// scan then only has to list the tree, not fetch it again.
bool LocalCopyMatches(const ScanFile& file) {
    if (file.size == 0) return false;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(file.local.c_str(), GetFileExInfoStandard, &data)) return false;
    const uint64_t on_disk = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if (on_disk != file.size) return false;
    // A size alone cannot tell an unchanged row from one the server rewrote in
    // place; compare the server's time when the listing carried it.
    if (!file.has_mtime) return true;
    return CompareFileTime(&data.ftLastWriteTime, &file.mtime) == 0;
}

// True for a file this scan kept. Creation time is the marker, and the margin
// covers file systems that store it with a coarser clock than NTFS (FAT rounds
// to 10 ms, some network shares to 2 s).
bool SeenThisScan(const FILETIME& created, const FILETIME& stamp) {
    ULARGE_INTEGER start;
    start.LowPart = stamp.dwLowDateTime;
    start.HighPart = stamp.dwHighDateTime;
    // 100 ns units: two seconds.
    start.QuadPart = start.QuadPart > 20000000ull ? start.QuadPart - 20000000ull : 0;
    FILETIME threshold{};
    threshold.dwLowDateTime = start.LowPart;
    threshold.dwHighDateTime = start.HighPart;
    return CompareFileTime(&created, &threshold) >= 0;
}

// Drops every mirrored file this scan did not keep: an extra copy the user has
// since deleted, or a row that changed size, must not come back as a duplicate
// that cannot be removed.
void PruneMirror(const std::wstring& directory, const FILETIME& stamp, uint64_t& removed) {
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW(JoinLocal(directory, L"*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> children;
    do {
        if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
        children.emplace_back(found.cFileName);
    } while (FindNextFileW(search, &found));
    FindClose(search);
    for (const std::wstring& name : children) {
        const std::wstring child = JoinLocal(directory, name);
        if (IsDirectory(child)) {
            PruneMirror(child, stamp, removed);
            RemoveDirectoryW(child.c_str());  // only succeeds once it is empty
            continue;
        }
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(child.c_str(), GetFileExInfoStandard, &data)) continue;
        if (SeenThisScan(data.ftCreationTime, stamp)) continue;
        DeleteFileW(child.c_str());
        ++removed;
    }
}

// Mirrors an ftp:// folder into `local_root` so the duplicate scanner, which
// only reads Win32 paths, can compare it. Two passes: list the whole tree, then
// fetch only the rows whose size another row shares. Sizes on a phone's card
// are mostly unique, so the second pass touches a small part of the first, and
// a scan the user repeats only lists what an earlier one already fetched.
FtpError MirrorForScan(const std::wstring& remote, const std::wstring& local_root,
                       ScanStats& stats, uint64_t minimum_bytes, HWND window,
                       uint64_t generation) {
    const FILETIME stamp = ScanStamp();
    ScanReporter reporter{window, generation};
    uint64_t total_bytes = 0;
    std::vector<ScanFile> files;
    const FtpError collected = CollectScanTree(remote, local_root, stats, minimum_bytes, files,
                                               false, true, reporter, total_bytes);
    if (collected != FtpError::None) return collected;
    stats.listed = files.size();
    // Rows the listing gave a size for, bucketed by it: a bucket that holds a
    // single row cannot be a duplicate of anything in this tree.
    std::unordered_map<uint64_t, uint32_t> shared;
    for (const ScanFile& file : files) {
        if (file.size != 0) ++shared[file.size];
    }
    uint64_t candidates = 0;
    for (const ScanFile& file : files) {
        if (file.size == 0) {
            ++candidates;  // no size: the listing cannot rule it out
            continue;
        }
        const auto bucket = shared.find(file.size);
        if (bucket != shared.end() && bucket->second >= 2) ++candidates;
    }
    stats.candidates = candidates;
    reporter.Send(stats.listed, 0, total_bytes, candidates, true, true);

    uint64_t handled = 0;
    uint64_t handled_bytes = 0;
    for (const ScanFile& file : files) {
        if (ScanStopped(generation)) return FtpError::Cancelled;
        if (file.size != 0) {
            const auto bucket = shared.find(file.size);
            if (bucket != shared.end() && bucket->second < 2) {
                ++stats.unique;
                continue;
            }
        }
        if (LocalCopyMatches(file)) {
            TouchMirror(file, stamp);
            ++stats.reused;
        } else {
            FtpError error = FtpRegistry::Instance().Download(
                file.remote, file.local, [generation] { return ScanStopped(generation); });
            if (error != FtpError::None && RetryableTransferError(error)) {
                error = FtpRegistry::Instance().Download(
                    file.remote, file.local, [generation] { return ScanStopped(generation); });
            }
            if (error == FtpError::None) {
                TouchMirror(file, stamp);
                ++stats.fetched;
            } else if (SkippableScanError(error)) {
                ++stats.skipped;
            } else {
                return error;
            }
        }
        ++handled;
        handled_bytes += file.size;
        reporter.Send(stats.listed, handled, handled_bytes, candidates, true, false);
    }
    PruneMirror(local_root, stamp, stats.pruned);
    reporter.Send(stats.listed, handled, handled_bytes, candidates, true, true);
    return FtpError::None;
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
    result.sources = request.sources;
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

bool FtpOps::DownloadForScan(const std::wstring& credentials_url,
                             const std::wstring& local_root, HWND window,
                             uint64_t generation, uint64_t minimum_bytes) {
    if (credentials_url.empty() || local_root.empty() || !window) return false;
    // A newer scan replaces the one before it: the older one stops as soon as
    // it notices that it no longer owns the server.
    scan_owner.store(generation);
    stopping.store(false);
    active.fetch_add(1);
    std::thread([credentials_url, local_root, window, generation, minimum_bytes]() {
        const uint64_t started = GetTickCount64();
        ScanStats stats;
        const FtpError error =
            MirrorForScan(credentials_url, local_root, stats, minimum_bytes, window, generation);
        diagnostics::runtime::Event("ftp_scan_download",
                                    {{"error", static_cast<uint64_t>(error)},
                                     {"generation", generation},
                                     {"listed", stats.listed},
                                     {"candidates", stats.candidates},
                                     {"unique", stats.unique},
                                     {"reused", stats.reused},
                                     {"fetched", stats.fetched},
                                     {"skipped", stats.skipped},
                                     {"pruned", stats.pruned},
                                     {"ms", GetTickCount64() - started}});
        if (!stopping.load()) {
            auto* done = new FtpScanDownloadResult;
            done->local_root = local_root;
            done->generation = generation;
            done->error = static_cast<uint8_t>(error);
            done->skipped = stats.skipped;
            if (!PostMessageW(window, WM_FTP_SCAN_DOWNLOAD, 0,
                              reinterpret_cast<LPARAM>(done)))
                delete done;
        }
        active.fetch_sub(1);
    }).detach();
    return true;
}

void FtpOps::CancelScan(uint64_t generation) {
    // Only the scan that still owns the mirror can be stopped this way; a
    // newer scan must not be cancelled by an old tab's button.
    uint64_t expected = generation;
    scan_owner.compare_exchange_strong(expected, 0);
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
