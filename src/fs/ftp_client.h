// ftp_client.h - The FTP protocol over Winsock: one control connection, a
// passive data connection per transfer, no TLS (FTPS is not implemented).
//
// Everything blocks, so the callers run it on a worker thread
// (ftp_source.h keeps one client per server and hands out listings and
// transfers). Timeouts and cancellation are the caller's only protection from
// a phone that left the network mid-listing.
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::fs {

enum class FtpStatus : uint8_t {
    Ok,
    Network,    // connect/send/receive failed
    Timeout,    // the server did not answer within the budget
    Auth,       // the server refused the user name or the password
    NotFound,   // the path is not there (550 "no such file")
    Denied,     // the server refused the operation
    Protocol,   // a reply Pulse cannot use
    LocalIo,    // the local file could not be read or written
    Cancelled,
};

// "ftp://host/dir/file" -> host, port and remote path.
struct FtpTarget {
    std::wstring host;
    uint16_t port = 21;
    std::wstring path = L"/";  // decoded, starts with '/'
};

struct FtpItem {
    std::wstring name;
    bool is_dir = false;
    uint64_t size = 0;
    FILETIME mtime{};
    bool has_mtime = false;
};

struct FtpSettings {
    uint32_t connect_timeout_ms = 8000;
    uint32_t io_timeout_ms = 20000;
};

// Progress callbacks return false to stop the transfer (the caller's Cancel).
using FtpProgress = std::function<bool(uint64_t transferred, uint64_t total)>;
// Cancel callbacks return true to stop: polled between replies and during a
// transfer. An empty one means the caller cannot cancel.
using FtpCancel = std::function<bool()>;

class FtpClient {
public:
    explicit FtpClient(FtpSettings settings = {});
    ~FtpClient();
    FtpClient(const FtpClient&) = delete;
    FtpClient& operator=(const FtpClient&) = delete;

    // Winsock for this process; safe to call from any thread, idempotent.
    static void Startup();
    static void Shutdown();

    // Connects and logs in. An empty user logs in as anonymous. `detail` gets
    // the server's reply text when the status is not Ok.
    FtpStatus Connect(const std::wstring& host, uint16_t port, const std::wstring& user,
                      const std::wstring& password, std::wstring* detail);
    void Close();
    bool connected() const { return control_ != INVALID_SOCKET; }

    // Rows of one folder. The path is tried as "LIST <path>", then as
    // "CWD <path>" + "LIST"; names come back decoded from UTF-8.
    FtpStatus List(const std::wstring& path, std::vector<FtpItem>& out,
                   const FtpCancel& cancelled, std::wstring* detail);
    // SIZE + MDTM for one file (directory sizes are not reported by FTP).
    FtpStatus Stat(const std::wstring& path, uint64_t* size, FILETIME* mtime, bool* has_mtime,
                   std::wstring* detail);

    FtpStatus Download(const std::wstring& remote, const std::wstring& local,
                       const FtpCancel& cancelled, const FtpProgress& progress,
                       std::wstring* detail);
    FtpStatus Upload(const std::wstring& local, const std::wstring& remote,
                     const FtpCancel& cancelled, const FtpProgress& progress,
                     std::wstring* detail);
    // A file, or a directory (which the server must have empty or handle
    // recursively; basic FTP servers do not, so Pulse deletes contents first).
    FtpStatus Remove(const std::wstring& path, std::wstring* detail);
    FtpStatus RemoveDirectory(const std::wstring& path, std::wstring* detail);
    FtpStatus MakeDirectory(const std::wstring& path, std::wstring* detail);
    FtpStatus Rename(const std::wstring& from, const std::wstring& to, std::wstring* detail);
    // True when the last reply said the server speaks UTF-8 names.
    bool utf8_names() const { return utf8_names_; }

private:
    struct Reply {
        int code = 0;
        std::string text;
    };

    FtpStatus Command(const std::string& line, Reply* reply, std::wstring* detail);
    FtpStatus ReadReply(Reply* reply, std::wstring* detail);
    FtpStatus Expect(int wanted, const std::string& line, Reply* reply, std::wstring* detail);
    FtpStatus OpenData(SOCKET* data, std::wstring* detail);
    FtpStatus FinishTransfer(Reply* reply, std::wstring* detail);
    FtpStatus Abort();

    bool FillBuffer(std::wstring* detail);
    bool TakeLine(std::string* line);
    void Reset();

    FtpSettings settings_;
    SOCKET control_ = INVALID_SOCKET;
    std::string buffer_;
    bool utf8_names_ = false;
    bool epsv_ = true;
    bool mlsd_ = false;
};

// Decodes one "LIST" row (unix or DOS style). False for rows that name
// nothing (the "total 12" header).
bool ParseFtpListRow(std::string_view line, FtpItem& out);
// Decodes one MLSD row: "type=file;size=12;modify=20240101120000; name".
bool ParseFtpMlsdRow(std::string_view line, FtpItem& out);

}  // namespace pulse::fs
