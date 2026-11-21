// ftp_cache.cpp
#include "ftp_cache.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "app_messages.h"
#include "ftp_registry.h"
#include "session.h"
#include "../common/runtime_log.h"
#include "../fs/ftp_url.h"

namespace pulse::app {
namespace {

// The URL as a directory name: FNV-1a over the UTF-16 code units, so the same
// URL always lands in the same directory and two hosts never share one.
std::wstring HashOf(const std::wstring& text) {
    uint64_t hash = 1469598103934665603ull;
    for (const wchar_t c : text) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ull;
    }
    wchar_t buffer[17] = {};
    for (int i = 15; i >= 0; --i) {
        buffer[i] = L"0123456789abcdef"[hash & 0xF];
        hash >>= 4;
    }
    return std::wstring(buffer);
}

// Creates each missing piece of `path` (no trailing separator needed).
void EnsureDirectory(const std::wstring& path) {
    std::wstring partial;
    partial.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        partial.push_back(path[i]);
        const bool boundary = path[i] == L'\\' || path[i] == L'/';
        if (!boundary) continue;
        if (partial.size() <= 3) continue;  // "C:\" and the like
        CreateDirectoryW(partial.c_str(), nullptr);
    }
    if (!path.empty()) CreateDirectoryW(path.c_str(), nullptr);
}

}  // namespace

// Opening a file hands it to another program, which may keep it open: give each
// open its own name so a second one does not fail on a share violation.
std::wstring FreeLocalPath(const std::wstring& directory, const std::wstring& leaf) {
    std::wstring candidate = directory + L"\\" + leaf;
    if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;
    const size_t dot = leaf.find_last_of(L'.');
    const std::wstring stem = dot == std::wstring::npos ? leaf : leaf.substr(0, dot);
    const std::wstring extension = dot == std::wstring::npos ? std::wstring() : leaf.substr(dot);
    for (int attempt = 2; attempt < 100; ++attempt) {
        candidate = directory + L"\\" + stem + L" (" + std::to_wstring(attempt) + L")" + extension;
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;
    }
    return candidate;
}

FtpCache& FtpCache::Instance() {
    static FtpCache cache;
    return cache;
}

std::wstring FtpCache::LeafFor(const std::wstring& url) {
    fs::FtpUrl parsed;
    if (!fs::ParseFtpUrl(url, parsed)) return L"file";
    std::wstring leaf = fs::FtpLeafName(parsed.path);
    if (leaf.empty()) leaf = L"file";
    // A name Win32 accepts: no separators, no trailing dot or space.
    for (wchar_t& c : leaf) {
        if (c < 32 || c == L'\\' || c == L'/' || c == L':' || c == L'*' ||
            c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|')
            c = L'_';
    }
    while (!leaf.empty() && (leaf.back() == L'.' || leaf.back() == L' ')) leaf.pop_back();
    if (leaf.empty()) leaf = L"file";
    return leaf;
}

std::wstring FtpCache::DirectoryFor(const std::wstring& url) {
    return GetPulseDataDir() + L"\\Cache\\Ftp\\" + HashOf(url);
}

std::wstring FtpCache::LocalCopy(const std::wstring& url) {
    if (url.empty()) return {};
    const std::wstring path = DirectoryFor(url) + L"\\" + LeafFor(url);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return {};
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return {};
    return path;
}

bool FtpCache::Request(const std::wstring& url, HWND window, bool for_preview) {
    fs::FtpUrl parsed;
    if (!fs::ParseFtpUrl(url, parsed)) return false;
    if (!window) return false;

    // A copy already on disk: answer at once, still through the message, so the
    // caller has one path to maintain. Opening a file always fetches it again:
    // the copy from an earlier visit may be older than what the server holds now.
    if (const std::wstring local = for_preview ? LocalCopy(url) : std::wstring(); !local.empty()) {
        auto* result = new FtpCacheResult{url, local, 0, for_preview};
        if (!PostMessageW(window, WM_FTP_CACHE_READY, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
        return true;
    }

    {
        std::lock_guard lock(mutex_);
        if (stopping_) return true;
        if (!inflight_.insert(url).second) return true;  // already on its way
        ++running_;
    }
    const std::wstring directory = DirectoryFor(url);
    const std::wstring local = for_preview ? directory + L"\\" + LeafFor(url) : FreeLocalPath(directory, LeafFor(url));
    diagnostics::runtime::TextEvent("ftp_cache_request", {{"path", parsed.path}}, {{"preview", for_preview ? 1u : 0u}});

    std::thread worker([this, url, directory, local, window, for_preview] {
        EnsureDirectory(directory);
        const FtpError error = FtpRegistry::Instance().Download(url, local, [this] {
            std::lock_guard lock(mutex_);
            return stopping_;
        });
        auto* result = new FtpCacheResult{url, error == FtpError::None ? local : std::wstring(),
                                          static_cast<uint8_t>(error), for_preview};
        {
            std::lock_guard lock(mutex_);
            inflight_.erase(url);
            --running_;
        }
        diagnostics::runtime::TextEvent("ftp_cache_ready", {{"path", url}},
                                        {{"error", static_cast<uint64_t>(error)}});
        if (!PostMessageW(window, WM_FTP_CACHE_READY, 0, reinterpret_cast<LPARAM>(result))) delete result;
    });
    worker.detach();
    return true;
}

void FtpCache::Shutdown() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    // The fetches poll `stopping_` between replies and during a transfer; the
    // processes ends soon after, so no join here.
}

}  // namespace pulse::app
