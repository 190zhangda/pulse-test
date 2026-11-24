#include "duplicate_scan.h"
#include "app_internal.h"
#include "../fs/fs_enum.h"
#include "../fs/ftp_url.h"
#include "../common/localization.h"
#include "../common/runtime_log.h"
#include "app_messages.h"
#include "content_pack_ui.h"
#include "ftp_ops.h"
#include "ftp_registry.h"
#include "session.h"

#include <cstdio>
#include <thread>

namespace pulse {
namespace {

// FNV-1a over the URL, sixteen hex digits: one cache folder per remote tree.
std::wstring FtpScanCacheKey(const std::wstring& url) {
    uint64_t hash = 1469598103934665603ull;
    for (wchar_t c : url) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ull;
    }
    wchar_t text[17]{};
    _snwprintf_s(text, _TRUNCATE, L"%016llx", static_cast<unsigned long long>(hash));
    return text;
}

void EnsureFolders(const std::wstring& path) {
    std::wstring sofar;
    for (wchar_t c : path) {
        sofar.push_back(c);
        if (c == L'\\' || c == L'/') CreateDirectoryW(sofar.c_str(), nullptr);
    }
    CreateDirectoryW(sofar.c_str(), nullptr);
}

// The local folder that mirrors `url` for the duplicate scanner.
std::wstring FtpScanLocalRoot(const std::wstring& url) {
    std::wstring root = app::GetPulseDataDir() + L"\\Cache\\Ftp\\Scan\\" + FtpScanCacheKey(url);
    EnsureFolders(root);
    return root;
}

// The Win32 code the duplicate-scan view shows for a failed mirror download.
DWORD ScanErrorFor(uint8_t error) {
    switch (static_cast<app::FtpError>(error)) {
    case app::FtpError::None: return ERROR_SUCCESS;
    case app::FtpError::BadUrl: return ERROR_INVALID_NAME;
    case app::FtpError::Auth: return ERROR_ACCESS_DENIED;
    case app::FtpError::NotFound: return ERROR_PATH_NOT_FOUND;
    case app::FtpError::Timeout: return ERROR_TIMEOUT;
    case app::FtpError::Network: return ERROR_NETWORK_UNREACHABLE;
    case app::FtpError::Denied: return ERROR_ACCESS_DENIED;
    case app::FtpError::Cancelled: return ERROR_CANCELLED;
    }
    return ERROR_NOT_SUPPORTED;
}

// Scan requests normally wait for the content pack; an isolated test run owns
// the pack check itself (content_pack_ui declines there), so the probe env var
// lets the scan proceed against the bundled pack.
bool ContentPackReady(AppState& s) {
    if (s.isolatedTest) {
        wchar_t probe[8]{};
        if (GetEnvironmentVariableW(L"PULSE_TEST_DUP_SCAN", probe, ARRAYSIZE(probe))) return true;
    }
    return OfferContentPack(s);
}

// Hands `roots` to the duplicate finder in Pulse.Content.exe.
void RunDuplicateSearch(AppState& s, const std::vector<std::wstring>& roots) {
    auto& session = s.duplicateScan;
    session.ResetResults();
    session.generation += 1;
    session.scanning = true;
    index::ContentSearchRequest request;
    request.generation = session.generation;
    request.mode = index::ContentSearchMode::Duplicates;
    request.maximum_hits = 10000;
    request.roots = roots;
    request.root = roots.front();
    request.recursive = true;
    request.skip_system_locations = session.scope != app::DuplicateScanScope::Folder;
    request.minimum_file_bytes = session.minimum_file_bytes;
    s.duplicateSearch.SearchAsync(std::move(request));
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Deletes the extra copies of one group (or all of them) on the server.
void DeleteFtpExtras(AppState& s, std::vector<std::wstring> targets) {
    if (targets.empty()) return;
    app::FtpOpRequest request;
    request.kind = app::FtpOpKind::Delete;
    request.folder = s.duplicateScan.ftp_source_root;
    request.sources = std::move(targets);
    request.dirs.assign(request.sources.size(), 0);  // duplicates are files
    app::FtpOps::Instance().Submit(std::move(request), s.hwnd);
}

}  // namespace

void PersistDuplicateScanPrefs(AppState& s) {
    s.appPrefs.duplicate_scan_scope = static_cast<int>(s.duplicateScan.scope);
    s.appPrefs.duplicate_scan_folder = s.duplicateScan.folder_path;
    s.appPrefs.duplicate_scan_drive = s.duplicateScan.drive_root;
    s.appPrefs.Save();
}

void RequestDuplicateVolumeCache(AppState& s, bool force) {
    if (!s.hwnd || s.dup_volume_cache_pending) return;
    if (!force && s.dup_volume_cache_tick != 0 &&
        GetTickCount64() - s.dup_volume_cache_tick <= 2000)
        return;
    s.dup_volume_cache_pending = true;
    HWND hwnd = s.hwnd;
    std::thread([hwnd] {
        index::IndexConfig config;
        auto* payload = new std::vector<index::VolumeInfo>(index::EnumerateLocalVolumes(config));
        if (!PostMessageW(hwnd, WM_DUP_VOLUMES, 0, reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

void ApplyDuplicateVolumeCache(AppState& s, std::vector<index::VolumeInfo> volumes) {
    s.dup_volume_cache = std::move(volumes);
    s.dup_volume_cache_tick = GetTickCount64();
    s.dup_volume_cache_pending = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void StartDuplicateScan(AppState& s) {
    auto& session = s.duplicateScan;
    if (session.scope == app::DuplicateScanScope::AllFixed && s.dup_volume_cache.empty()) {
        RequestDuplicateVolumeCache(s, true);
        return;
    }
    const auto& volumes = s.dup_volume_cache;
    if (session.scope == app::DuplicateScanScope::Folder && session.folder_path.empty()) {
        std::wstring path;
        if (!PickFolder(s, path, l10n::Get(l10n::StringId::DupFolderPlaceholder).c_str(), L"duplicates", {}, true))
            return;
        session.folder_path = std::move(path);
    }
    if (session.scope == app::DuplicateScanScope::Drive && session.drive_root.empty()) {
        for (const auto& volume : volumes) {
            if (volume.mount_point.empty()) continue;
            session.drive_root = app::DuplicateScanSession::NormalizeDriveRoot(volume.mount_point);
            break;
        }
    }
    // An ftp:// folder has no local files to walk: mirror it first, then scan
    // the copy. The password stays in this session (the picked text), while
    // the folder path shown and saved carries only the user name.
    if (fs::IsFtpUrl(session.folder_path)) {
        fs::FtpUrl remote;
        if (!fs::ParseFtpUrl(session.folder_path, remote)) return;
        if (!ContentPackReady(s)) return;
        // Keep the signed-in text in the session only; the folder path that is
        // shown and saved drops the password.
        const std::wstring canonical = fs::FtpCanonicalText(remote);
        std::wstring credentials = session.folder_path;
        // Scanning again starts from the saved path, which has no password:
        // fall back to the text this session signed in with for that folder.
        if (remote.password.empty() && !session.ftp_credentials.empty()) {
            fs::FtpUrl previous;
            if (fs::ParseFtpUrl(session.ftp_credentials, previous) &&
                !previous.password.empty() &&
                _wcsicmp(fs::FtpCanonicalText(previous).c_str(), canonical.c_str()) == 0)
                credentials = session.ftp_credentials;
        }
        const std::wstring local_root = FtpScanLocalRoot(credentials);
        session.ResetResults();
        session.generation += 1;
        session.scope = app::DuplicateScanScope::Folder;
        session.folder_path = canonical;
        session.SetFtpMapping(canonical, credentials, local_root);
        PersistDuplicateScanPrefs(s);
        session.scanning = true;
        session.phase = index::ContentSearchPhase::Enumerating;
        session.current_root = canonical;
        if (!app::FtpOps::Instance().DownloadForScan(credentials, local_root, s.hwnd,
                                                    session.generation,
                                                    session.minimum_file_bytes)) {
            session.scanning = false;
            session.completed = true;
            session.error = ERROR_NOT_SUPPORTED;
        }
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    auto roots = app::DuplicateScanSession::ResolveRoots(
        session.scope, session.folder_path, session.drive_root, volumes);
    if (roots.empty()) return;
    // Duplicate finding runs in Pulse.Content.exe (content enhancement pack).
    if (!ContentPackReady(s)) return;
    PersistDuplicateScanPrefs(s);
    session.ClearFtpMapping();
    RunDuplicateSearch(s, roots);
}

void ApplyFtpScanDownload(AppState& s, const app::FtpScanDownloadResult& result) {
    auto& session = s.duplicateScan;
    // A newer scan (or a cancelled one) replaced this download.
    if (result.generation != session.generation || !session.IsFtpScan()) return;
    if (result.error != 0) {
        session.scanning = false;
        session.completed = true;
        session.error = ScanErrorFor(result.error);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    // Protected folders on the server were left out of the mirror; the scan
    // itself is fine, so say so plainly instead of reporting no duplicates.
    if (result.skipped != 0)
        diagnostics::runtime::Event("ftp_scan_skipped", {{"rows", result.skipped}});
    RunDuplicateSearch(s, {result.local_root});
}

void ApplyFtpScanProgress(AppState& s, const app::FtpScanProgress& progress) {
    auto& session = s.duplicateScan;
    if (progress.generation != session.generation || !session.IsFtpScan() ||
        !session.scanning)
        return;
    // The listing phase cannot know how many rows exist yet, so its count is
    // the files named so far; the download phase knows its (much smaller)
    // candidate set and reports against it.
    session.scanned_files = progress.listing_done ? progress.files : progress.listed;
    session.total_files = progress.listing_done ? progress.total : 0;
    session.scanned_bytes = progress.bytes;
    session.current_root = session.ftp_source_root;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void CancelDuplicateScan(AppState& s) {
    s.duplicateSearch.Cancel();
    // A mirror download may still be running: tell it to stop, then bumping the
    // generation makes any result it still posts stale, so a cancelled scan
    // cannot start one moment later.
    if (s.duplicateScan.scanning && s.duplicateScan.IsFtpScan())
        app::FtpOps::Instance().CancelScan(s.duplicateScan.generation);
    if (s.duplicateScan.scanning) s.duplicateScan.generation += 1;
    s.duplicateScan.scanning = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void RecycleDuplicateGroup(AppState& s, size_t group) {
    if (s.duplicateScan.IsFtpScan()) {
        // Nothing on the server can go to the recycle bin: FtpOps deletes it.
        DeleteFtpExtras(s, s.duplicateScan.FtpFilesToDelete(group));
        return;
    }
    ops::OpRequest req;
    if (!s.duplicateScan.BuildCleanupRequest(req, group)) return;
    s.ops.Submit(std::move(req));
}

void RecycleAllDuplicateExtras(AppState& s) {
    if (s.duplicateScan.IsFtpScan()) {
        DeleteFtpExtras(s, s.duplicateScan.AllFtpFilesToDelete());
        return;
    }
    ops::OpRequest req;
    if (!s.duplicateScan.BuildCleanupRequest(req)) return;
    s.ops.Submit(std::move(req));
}

void OpenDuplicateLocation(AppState& s, size_t group, size_t file) {
    if (group >= s.duplicateScan.groups.size()) return;
    const auto& files = s.duplicateScan.groups[group].files;
    if (file >= files.size()) return;
    const std::wstring& path = files[file].path;
    // A server path has no Windows separators: ask the ftp helpers for its
    // folder, then browse there like any other remote folder.
    fs::FtpUrl remote;
    if (fs::ParseFtpUrl(path, remote)) {
        remote.path = fs::FtpParentPath(remote.path);
        NavigateTo(s, fs::FtpCanonicalText(remote));
        return;
    }
    const std::wstring parent = fs::ParentPath(path);
    if (!parent.empty()) NavigateTo(s, parent);
}

} // namespace pulse