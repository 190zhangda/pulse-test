#pragma once

#include "../index/content_search.h"
#include "../index/index_config.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse {

struct AppState;
namespace ops { struct OpRequest; }

namespace app {

// ftp_ops.h: the result of the mirror download that precedes a scan of an
// ftp:// folder, and its progress reports.
struct FtpScanDownloadResult;
struct FtpScanProgress;

enum class DuplicateScanScope : int {
    Folder = 0,
    Drive = 1,
    AllFixed = 2,
};

struct DuplicateFile {
    std::wstring path;
    std::wstring name;
    uint64_t size = 0;
    uint64_t modified = 0;
};

struct DuplicateGroup {
    uint32_t id = 0;
    uint64_t size = 0;
    std::vector<DuplicateFile> files;
    size_t keep_index = 0;
};

struct DuplicateScanSession {
    DuplicateScanScope scope = DuplicateScanScope::Folder;
    std::wstring folder_path;
    std::wstring drive_root;
    uint64_t minimum_file_bytes = 1024;
    uint64_t generation = 0;
    bool scanning = false;
    bool completed = false;
    bool truncated = false;
    DWORD error = ERROR_SUCCESS;
    index::ContentSearchPhase phase = index::ContentSearchPhase::Enumerating;
    uint64_t scanned_files = 0;
    uint64_t total_files = 0;
    uint64_t scanned_bytes = 0;
    std::wstring current_root;
    double files_per_second = 0;
    double megabytes_per_second = 0;
    std::vector<DuplicateGroup> groups;
    uint64_t result_epoch = 0;

    // A scan of an ftp:// folder is mirrored to a local cache first (the
    // Pulse.Content scanner only understands Win32 paths). These remember the
    // mapping so hits can be shown as ftp:// paths and cleanup deletes on the
    // server instead of the local mirror. `ftp_source_root` is canonical text
    // without a password; `ftp_credentials` carries the original signed-in URL
    // (password included) that transfers must use.
    std::wstring ftp_source_root;
    std::wstring ftp_credentials;
    std::wstring ftp_local_root;

    static uint64_t DefaultMinimumBytes(DuplicateScanScope scope) noexcept;
    static std::wstring NormalizeDriveRoot(std::wstring root);
    static std::vector<std::wstring> ResolveRoots(
        DuplicateScanScope scope, const std::wstring& folder, const std::wstring& drive,
        const std::vector<index::VolumeInfo>& volumes);

    void ResetResults();
    void ApplyUpdate(const index::ContentSearchProgress& progress,
                     const std::vector<index::ContentHit>& hits);
    void SetFtpMapping(std::wstring source_root, std::wstring credentials,
                       std::wstring local_root);
    void ClearFtpMapping();
    bool IsFtpScan() const noexcept { return !ftp_local_root.empty(); }
    void SetKeep(size_t group, size_t file);
    std::vector<std::wstring> FilesToDelete(size_t group) const;
    std::vector<std::wstring> AllFilesToDelete() const;
    bool BuildCleanupRequest(ops::OpRequest& request, size_t group = SIZE_MAX) const;
    // FTP cleanup: the extra rows of `selected` (or every group) as ftp://
    // URLs; the caller deletes them on the server via FtpOps.
    std::vector<std::wstring> FtpFilesToDelete(size_t group) const;
    std::vector<std::wstring> AllFtpFilesToDelete() const;
    void RemoveDeleted(const std::vector<std::wstring>& paths);
    size_t ExtraCount() const;

private:
    void AppendHits(const std::vector<index::ContentHit>& hits);
    std::wstring MapLocalToFtp(const std::wstring& path) const;
    void SortGroups();
    void RebuildGroupIndex();
    void UpdateSpeed(const index::ContentSearchProgress& progress);

    std::unordered_map<uint32_t, DuplicateFile> pending_groups_;
    std::unordered_map<uint32_t, size_t> group_indices_;
    std::unordered_set<uint32_t> invalidated_groups_;
    std::chrono::steady_clock::time_point speed_tick_{};
    uint64_t speed_files_ = 0;
    uint64_t speed_bytes_ = 0;
};

} // namespace app

// The recursive download that mirrors an ftp:// folder (ftp_ops.h) finished:
// point the session at the local copy and run the ordinary scan.
void ApplyFtpScanDownload(AppState& s, const app::FtpScanDownloadResult& result);
// WM_FTP_SCAN_PROGRESS: show the mirror's listing or download count.
void ApplyFtpScanProgress(AppState& s, const app::FtpScanProgress& progress);
void StartDuplicateScan(AppState& s);
void CancelDuplicateScan(AppState& s);
void RecycleDuplicateGroup(AppState& s, size_t group);
void RecycleAllDuplicateExtras(AppState& s);
void OpenDuplicateLocation(AppState& s, size_t group, size_t file);
void PersistDuplicateScanPrefs(AppState& s);
void RequestDuplicateVolumeCache(AppState& s, bool force = false);
void ApplyDuplicateVolumeCache(AppState& s, std::vector<index::VolumeInfo> volumes);

} // namespace pulse
