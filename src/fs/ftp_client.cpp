#include <winsock2.h>
#include <ws2tcpip.h>

#include "ftp_client.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

#include "ftp_url.h"

namespace pulse::fs {
namespace {

constexpr int kChunk = 64 * 1024;
constexpr size_t kMaxListingBytes = 64u * 1024u * 1024u;

// Server replies are ASCII, sometimes UTF-8: decode them for the toast.
void SetDetail(std::wstring* detail, const std::string& text) {
    if (detail) *detail = FtpFromUtf8(text);
}

void SetDetail(std::wstring* detail, const std::wstring& text) {
    if (detail) *detail = text;
}

bool SetSocketTimeout(SOCKET handle, uint32_t ms) {
    DWORD value = ms;
    const char* bytes = reinterpret_cast<const char*>(&value);
    const bool recv_ok = setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, bytes, static_cast<int>(sizeof(value))) == 0;
    const bool send_ok = setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, bytes, static_cast<int>(sizeof(value))) == 0;
    return recv_ok && send_ok;
}

std::wstring Unbracket(std::wstring host) {
    if (host.size() >= 2 && host.front() == L'[' && host.back() == L']')
        host = host.substr(1, host.size() - 2);
    return host;
}

// Connects a TCP socket to host:port, honouring the connect budget. SOCKET is
// INVALID_SOCKET on failure and `detail` says why.
SOCKET ConnectTcp(const std::wstring& host, uint16_t port, uint32_t timeout_ms, std::wstring* detail) {
    const std::wstring plain = Unbracket(host);
    addrinfoW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    const std::wstring service = std::to_wstring(port);
    addrinfoW* candidates = nullptr;
    const int looked = GetAddrInfoW(plain.c_str(), service.c_str(), &hints, &candidates);
    if (looked != 0 || !candidates) {
        SetDetail(detail, L"cannot resolve " + plain);
        return INVALID_SOCKET;
    }
    SOCKET result = INVALID_SOCKET;
    for (addrinfoW* entry = candidates; entry && result == INVALID_SOCKET; entry = entry->ai_next) {
        SOCKET client = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (client == INVALID_SOCKET) continue;
        u_long nonblocking = 1;
        ioctlsocket(client, FIONBIO, &nonblocking);
        const int started = connect(client, entry->ai_addr, static_cast<int>(entry->ai_addrlen));
        bool up = started == 0;
        if (!up && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(client, &writable);
            fd_set failed;
            FD_ZERO(&failed);
            FD_SET(client, &failed);
            timeval budget{};
            budget.tv_sec = static_cast<long>(timeout_ms / 1000);
            budget.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
            const int ready = select(0, nullptr, &writable, &failed, &budget);
            if (ready > 0 && FD_ISSET(client, &writable)) {
                int error = 0;
                int length = sizeof(error);
                if (getsockopt(client, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == 0 &&
                    error == 0)
                    up = true;
            }
        }
        if (!up) {
            closesocket(client);
            continue;
        }
        nonblocking = 0;
        ioctlsocket(client, FIONBIO, &nonblocking);
        SetSocketTimeout(client, timeout_ms);
        result = client;
    }
    FreeAddrInfoW(candidates);
    if (result == INVALID_SOCKET && detail && detail->empty())
        *detail = L"cannot connect to " + plain + L":" + service;
    return result;
}

bool SendAll(SOCKET handle, const char* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        const int written = send(handle, data + sent, static_cast<int>(std::min<size_t>(size - sent, 1u << 20)), 0);
        if (written <= 0) return false;
        sent += static_cast<size_t>(written);
    }
    return true;
}

// > 0: bytes read, 0: the peer closed, < 0: error (WSAETIMEDOUT when slow).
int RecvSome(SOCKET handle, char* out, int size) {
    return recv(handle, out, size, 0);
}

int MonthFromName(std::string_view name) {
    static const char* kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                    "jul", "aug", "sep", "oct", "nov", "dec"};
    if (name.size() < 3) return 0;
    std::string head(name.substr(0, 3));
    for (char& c : head) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (int i = 0; i < 12; ++i)
        if (head == kMonths[i]) return i + 1;
    return 0;
}

bool SystemTimeToFiletimeUtc(int year, int month, int day, int hour, int minute, int second, FILETIME* out) {
    SYSTEMTIME parts{};
    parts.wYear = static_cast<WORD>(year);
    parts.wMonth = static_cast<WORD>(month);
    parts.wDay = static_cast<WORD>(day);
    parts.wHour = static_cast<WORD>(hour);
    parts.wMinute = static_cast<WORD>(minute);
    parts.wSecond = static_cast<WORD>(second);
    return SystemTimeToFileTime(&parts, out) != 0;
}

}  // namespace

bool ParseFtpListRow(std::string_view line, FtpItem& out) {
    out = FtpItem{};
    if (line.size() < 4) return false;
    if (line.rfind("total", 0) == 0) return false;

    // Unix style: "drwxr-xr-x 2 owner group 4096 Jan 01 12:00 name". The
    // owner and group fields are optional on some servers, so the date is the
    // anchor: the size is the field before the month name, the name is
    // whatever follows the three date fields.
    const char kind = line.front();
    const bool unix_style = kind == '-' || kind == 'd' || kind == 'l' || kind == 'b' || kind == 'c' ||
                            kind == 'p' || kind == 's';
    if (unix_style) {
        struct Span {
            size_t begin = 0;
            size_t end = 0;
        };
        Span spans[12]{};
        int count = 0;
        size_t cursor = 0;
        while (cursor < line.size() && count < 12) {
            while (cursor < line.size() && line[cursor] == ' ') ++cursor;
            if (cursor >= line.size()) break;
            const size_t start = cursor;
            while (cursor < line.size() && line[cursor] != ' ') ++cursor;
            spans[count].begin = start;
            spans[count].end = cursor;
            ++count;
        }
        int month_at = -1;
        for (int i = 4; i < count; ++i) {
            if (MonthFromName(line.substr(spans[i].begin, spans[i].end - spans[i].begin)) > 0) {
                month_at = i;
                break;
            }
        }
        if (month_at < 2 || month_at + 3 > count) return false;
        const int size_at = month_at - 1;
        out.size = strtoull(std::string(line.substr(spans[size_at].begin,
                                                    spans[size_at].end - spans[size_at].begin)).c_str(),
                            nullptr, 10);
        out.is_dir = kind == 'd';

        const std::string month_text(line.substr(spans[month_at].begin, spans[month_at].end - spans[month_at].begin));
        const std::string day_text(line.substr(spans[month_at + 1].begin,
                                               spans[month_at + 1].end - spans[month_at + 1].begin));
        const std::string stamp(line.substr(spans[month_at + 2].begin,
                                            spans[month_at + 2].end - spans[month_at + 2].begin));
        int hour = 0;
        int minute = 0;
        int year = 0;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        if (stamp.find(':') != std::string::npos) {
            sscanf_s(stamp.c_str(), "%d:%d", &hour, &minute);
            year = now.wYear;
            const int month = MonthFromName(month_text);
            // A month later than this one belongs to last year.
            if (month > static_cast<int>(now.wMonth)) --year;
        } else {
            year = atoi(stamp.c_str());
        }
        const int month = MonthFromName(month_text);
        const int day = atoi(day_text.c_str());
        if (month > 0 && year > 1900 &&
            SystemTimeToFiletimeUtc(year, month, day, hour, minute, 0, &out.mtime))
            out.has_mtime = true;

        size_t name_start = spans[month_at + 3].begin;
        if (name_start >= line.size()) return false;
        std::string name(line.substr(name_start));
        if (name == "." || name == "..") return false;
        // A symlink row reads "name -> target": the row is the link itself.
        if (const size_t arrow = name.find(" -> "); arrow != std::string::npos) name.resize(arrow);
        out.name = FtpFromUtf8(name);
        return !out.name.empty();
    }

    // DOS style: "01-01-24  12:00PM       <DIR>          name".
    if (line.size() > 20 && line[2] == '-' && line[5] == '-') {
        char date[16]{};
        char time_text[16]{};
        if (sscanf_s(std::string(line).c_str(), "%15s %15s", date, static_cast<unsigned>(sizeof(date)),
                     time_text, static_cast<unsigned>(sizeof(time_text))) != 2)
            return false;
        int month = 0;
        int day = 0;
        int year = 0;
        if (sscanf_s(date, "%d-%d-%d", &month, &day, &year) != 3) return false;
        if (year < 100) year += year < 70 ? 2000 : 1900;
        int hour = 0;
        int minute = 0;
        char meridiem[4]{};
        if (sscanf_s(time_text, "%d:%d%3s", &hour, &minute, meridiem,
                     static_cast<unsigned>(sizeof(meridiem))) < 2)
            return false;
        const bool pm = tolower(static_cast<unsigned char>(meridiem[0])) == 'p';
        if (pm && hour < 12) hour += 12;
        if (!pm && hour == 12) hour = 0;

        std::string rest(line.substr(18));
        const size_t first = rest.find_first_not_of(' ');
        if (first == std::string::npos) return false;
        rest.erase(0, first);
        size_t name_at = 0;
        if (rest.rfind("<DIR>", 0) == 0) {
            out.is_dir = true;
            name_at = 5;
        } else {
            const size_t space = rest.find(' ');
            if (space == std::string::npos) return false;
            out.size = strtoull(rest.substr(0, space).c_str(), nullptr, 10);
            name_at = space;
        }
        while (name_at < rest.size() && rest[name_at] == ' ') ++name_at;
        if (name_at >= rest.size()) return false;
        out.name = FtpFromUtf8(rest.substr(name_at));
        if (SystemTimeToFiletimeUtc(year, month, day, hour, minute, 0, &out.mtime)) out.has_mtime = true;
        return !out.name.empty();
    }
    return false;
}

bool ParseFtpMlsdRow(std::string_view line, FtpItem& out) {
    out = FtpItem{};
    const size_t space = line.find(' ');
    if (space == std::string_view::npos) return false;
    const std::string_view facts = line.substr(0, space);
    std::string_view name = line.substr(space + 1);
    if (!name.empty() && name.front() == ' ') name.remove_prefix(1);
    if (name.empty() || name == "." || name == "..") return false;
    out.name = FtpFromUtf8(std::string(name));
    size_t cursor = 0;
    while (cursor < facts.size()) {
        const size_t semi = facts.find(';', cursor);
        const std::string_view fact =
            facts.substr(cursor, semi == std::string_view::npos ? std::string_view::npos : semi - cursor);
        cursor = semi == std::string_view::npos ? facts.size() : semi + 1;
        const size_t equals = fact.find('=');
        if (equals == std::string_view::npos) continue;
        const std::string key(fact.substr(0, equals));
        const std::string value(fact.substr(equals + 1));
        if (key == "type") {
            out.is_dir = value == "dir" || value == "cdir" || value == "pdir";
        } else if (key == "size") {
            out.size = strtoull(value.c_str(), nullptr, 10);
        } else if (key == "modify" && value.size() >= 14) {
            const int year = atoi(value.substr(0, 4).c_str());
            const int month = atoi(value.substr(4, 2).c_str());
            const int day = atoi(value.substr(6, 2).c_str());
            const int hour = atoi(value.substr(8, 2).c_str());
            const int minute = atoi(value.substr(10, 2).c_str());
            const int second = atoi(value.substr(12, 2).c_str());
            if (month > 0 && SystemTimeToFiletimeUtc(year, month, day, hour, minute, second, &out.mtime))
                out.has_mtime = true;
        }
    }
    return !out.name.empty();
}

void FtpClient::Startup() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

void FtpClient::Shutdown() {
    WSACleanup();
}

FtpClient::FtpClient(FtpSettings settings) : settings_(settings) {
    Startup();
}

FtpClient::~FtpClient() {
    Close();
}

void FtpClient::Reset() {
    if (control_ != INVALID_SOCKET) {
        closesocket(control_);
        control_ = INVALID_SOCKET;
    }
    buffer_.clear();
    utf8_names_ = false;
    epsv_ = true;
    mlsd_ = false;
}

void FtpClient::Close() {
    if (control_ != INVALID_SOCKET) {
        SendAll(control_, "QUIT\r\n", 6);
        shutdown(control_, SD_BOTH);
    }
    Reset();
}

bool FtpClient::FillBuffer(std::wstring* detail) {
    char chunk[kChunk];
    const int read = RecvSome(control_, chunk, kChunk);
    if (read > 0) {
        buffer_.append(chunk, static_cast<size_t>(read));
        return true;
    }
    if (read == 0) {
        SetDetail(detail, L"the server closed the connection");
        return false;
    }
    const int error = WSAGetLastError();
    SetDetail(detail, error == WSAETIMEDOUT ? std::wstring(L"the server did not answer in time")
                                            : std::wstring(L"the connection dropped"));
    return false;
}

bool FtpClient::TakeLine(std::string* line) {
    for (;;) {
        const size_t end = buffer_.find("\r\n");
        if (end != std::string::npos) {
            *line = buffer_.substr(0, end);
            buffer_.erase(0, end + 2);
            return true;
        }
        const size_t bare = buffer_.find('\n');
        if (bare != std::string::npos) {
            *line = buffer_.substr(0, bare);
            buffer_.erase(0, bare + 1);
            return true;
        }
        if (buffer_.size() > (1u << 20)) return false;  // no line breaks at all
        if (!FillBuffer(nullptr)) return false;
    }
}

FtpStatus FtpClient::ReadReply(Reply* reply, std::wstring* detail) {
    std::string first;
    if (!TakeLine(&first)) {
        Reset();
        return FtpStatus::Timeout;
    }

    if (first.size() < 3 || first[0] < '0' || first[0] > '9' || first[1] < '0' || first[1] > '9' ||
        first[2] < '0' || first[2] > '9') {
        Reset();
        SetDetail(detail, L"unexpected reply");
        return FtpStatus::Protocol;
    }
    const int code = atoi(first.substr(0, 3).c_str());
    reply->code = code;
    reply->text = first;
    const bool multiline = first.size() > 3 && first[3] == '-';
    if (multiline) {
        std::string marker = first.substr(0, 3) + " ";
        for (;;) {
            std::string line;
            if (!TakeLine(&line)) {
                Reset();
                return FtpStatus::Timeout;
            }
            reply->text += "\n";
            reply->text += line;
            if (line.rfind(marker, 0) == 0) break;
        }
    }
    if (code >= 400) {
        SetDetail(detail, reply->text);
        if (code == 421) {
            Reset();
            return FtpStatus::Network;
        }
    }
    return FtpStatus::Ok;
}

FtpStatus FtpClient::Command(const std::string& line, Reply* reply, std::wstring* detail) {
    if (control_ == INVALID_SOCKET) {
        SetDetail(detail, L"not connected");
        return FtpStatus::Network;
    }
    const std::string wire = line + "\r\n";
    if (!SendAll(control_, wire.data(), wire.size())) {
        const int error = WSAGetLastError();
        Reset();
        SetDetail(detail, error == WSAETIMEDOUT ? std::wstring(L"the server did not answer in time")
                                                : std::wstring(L"the connection dropped"));
        return error == WSAETIMEDOUT ? FtpStatus::Timeout : FtpStatus::Network;
    }
    return ReadReply(reply, detail);
}

FtpStatus FtpClient::Expect(int wanted, const std::string& line, Reply* reply, std::wstring* detail) {
    const FtpStatus status = Command(line, reply, detail);
    if (status != FtpStatus::Ok) return status;
    if (reply->code == wanted) return FtpStatus::Ok;
    if (reply->code == 530) return FtpStatus::Auth;
    if (reply->code == 550) return FtpStatus::NotFound;
    if (reply->code >= 500 && reply->code < 600) return FtpStatus::Denied;
    SetDetail(detail, reply->text);
    return FtpStatus::Protocol;
}

FtpStatus FtpClient::Connect(const std::wstring& host, uint16_t port, const std::wstring& user,
                             const std::wstring& password, std::wstring* detail) {
    Reset();
    control_ = ConnectTcp(host, port, settings_.connect_timeout_ms, detail);
    if (control_ == INVALID_SOCKET) return FtpStatus::Network;
    SetSocketTimeout(control_, settings_.io_timeout_ms);

    Reply reply;
    FtpStatus status = ReadReply(&reply, detail);
    if (status != FtpStatus::Ok) {
        Close();
        return status;
    }
    if (reply.code != 220) {
        SetDetail(detail, reply.text);
        Close();
        return FtpStatus::Protocol;
    }

    const std::wstring account = user.empty() ? std::wstring(L"anonymous") : user;
    const std::wstring secret = user.empty() ? std::wstring(L"pulse@") : password;
    status = Expect(331, "USER " + FtpUtf8(account), &reply, detail);
    if (status == FtpStatus::Ok || reply.code == 331) {
        status = Expect(230, "PASS " + FtpUtf8(secret), &reply, detail);
        if (reply.code == 230) status = FtpStatus::Ok;
    } else if (reply.code == 230) {
        status = FtpStatus::Ok;
    }
    if (status != FtpStatus::Ok) {
        Close();
        return reply.code == 530 ? FtpStatus::Auth : status;
    }

    // FEAT: MLSD and UTF8 names make listings exact; both are optional.
    if (Command("FEAT", &reply, detail) == FtpStatus::Ok && reply.code == 211) {
        std::string text = reply.text;
        for (char& c : text) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        mlsd_ = text.find("MLSD") != std::string::npos;
        if (text.find("UTF8") != std::string::npos) {
            Reply opts;
            if (Command("OPTS UTF8 ON", &opts, nullptr) == FtpStatus::Ok && opts.code == 200) utf8_names_ = true;
        }
    }
    // Binary transfers only: text mode would rewrite line endings.
    Expect(200, "TYPE I", &reply, detail);
    return FtpStatus::Ok;
}

FtpStatus FtpClient::OpenData(FtpHandle* data, std::wstring* detail) {
    *data = INVALID_SOCKET;
    Reply reply;
    std::wstring host;
    uint16_t port = 0;
    if (epsv_) {
        const FtpStatus status = Command("EPSV", &reply, detail);
        if (status != FtpStatus::Ok) return status;
        if (reply.code == 229) {
            const size_t open = reply.text.find('(');
            const size_t close = reply.text.rfind(')');
            if (open == std::string::npos || close == std::string::npos || close <= open)
                return FtpStatus::Protocol;
            // 229 Entering Extended Passive Mode (|||6446|): the port sits
            // between the third and fourth pipe.
            const std::string inside = reply.text.substr(open + 1, close - open - 1);
            std::vector<std::string> fields;
            size_t begin = 0;
            for (size_t i = 0; i <= inside.size(); ++i) {
                if (i != inside.size() && inside[i] != '|') continue;
                fields.emplace_back(inside.substr(begin, i - begin));
                begin = i + 1;
            }
            if (fields.size() < 5) return FtpStatus::Protocol;
            port = static_cast<uint16_t>(atoi(fields[3].c_str()));
            if (port == 0) return FtpStatus::Protocol;
        } else {
            epsv_ = false;  // the server does not know EPSV
        }
    }
    if (!epsv_) {
        const FtpStatus status = Command("PASV", &reply, detail);
        if (status != FtpStatus::Ok) return status;
        if (reply.code != 227) {
            SetDetail(detail, reply.text);
            return FtpStatus::Protocol;
        }
        const size_t open = reply.text.find('(');
        const size_t close = reply.text.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close <= open)
            return FtpStatus::Protocol;
        int parts[6]{};
        if (sscanf_s(reply.text.substr(open + 1, close - open - 1).c_str(), "%d,%d,%d,%d,%d,%d", &parts[0],
                     &parts[1], &parts[2], &parts[3], &parts[4], &parts[5]) != 6)
            return FtpStatus::Protocol;
        char address[64]{};
        _snprintf_s(address, sizeof(address), _TRUNCATE, "%d.%d.%d.%d", parts[0], parts[1], parts[2], parts[3]);
        host = FtpFromUtf8(address);
        port = static_cast<uint16_t>(parts[4] * 256 + parts[5]);
    }
    if (host.empty()) {
        // EPSV answers with a port only: the data connection goes to the same
        // address as the control connection.
        sockaddr_storage peer{};
        int length = sizeof(peer);
        if (getpeername(control_, reinterpret_cast<sockaddr*>(&peer), &length) != 0) return FtpStatus::Network;
        wchar_t text[64]{};
        DWORD text_length = sizeof(text);
        if (WSAAddressToStringW(reinterpret_cast<sockaddr*>(&peer), length, nullptr, text, &text_length) != 0)
            return FtpStatus::Network;
        host = text;
        const size_t percent = host.find(L'%');  // IPv6 scope suffix ("fe80::1%12")
        if (percent != std::wstring::npos) host.resize(percent);
        if (!host.empty() && host.front() == L'[') {
            const size_t close = host.find(L']');
            if (close != std::wstring::npos) host = host.substr(1, close - 1);
        } else if (const size_t colon = host.find(L':'); colon != std::wstring::npos) {
            host.resize(colon);
        }
    }
    *data = ConnectTcp(host, port, settings_.connect_timeout_ms, detail);
    if (*data == INVALID_SOCKET) return FtpStatus::Network;
    SetSocketTimeout(*data, settings_.io_timeout_ms);
    return FtpStatus::Ok;
}

FtpStatus FtpClient::FinishTransfer(Reply* reply, std::wstring* detail) {
    for (;;) {
        const FtpStatus status = ReadReply(reply, detail);
        if (status != FtpStatus::Ok) return status;
        if (reply->code == 226 || reply->code == 250) return FtpStatus::Ok;
        if (reply->code == 426 || reply->code == 451) return FtpStatus::Denied;
        if (reply->code >= 200 && reply->code < 300) return FtpStatus::Ok;
        if (reply->code == 150 || reply->code == 125) continue;
        SetDetail(detail, reply->text);
        return FtpStatus::Protocol;
    }
}

FtpStatus FtpClient::Abort() {
    if (control_ == INVALID_SOCKET) return FtpStatus::Cancelled;
    SendAll(control_, "ABOR\r\n", 6);
    // Drain whatever the server still sends; a desynchronised connection is
    // closed instead of guessing.
    Reply reply;
    for (int guard = 0; guard < 3; ++guard) {
        if (ReadReply(&reply, nullptr) != FtpStatus::Ok) break;
        if (reply.code == 226 || reply.code == 225) break;
    }
    return FtpStatus::Cancelled;
}

FtpStatus FtpClient::List(const std::wstring& path, std::vector<FtpItem>& out,
                          const FtpCancel& cancelled, std::wstring* detail) {
    out.clear();
    if (control_ == INVALID_SOCKET) {
        SetDetail(detail, L"not connected");
        return FtpStatus::Network;
    }
    const std::string encoded = FtpUtf8(FtpPercentEncode(path));
    const std::string verb = mlsd_ ? "MLSD" : "LIST";
    std::string listing;
    FtpStatus status = FtpStatus::Protocol;
    // A few servers refuse a path argument on LIST/MLSD: those get a CWD and a
    // bare verb on the second attempt.
    for (int attempt = 0; attempt < 2; ++attempt) {
        SOCKET data = INVALID_SOCKET;
        status = OpenData(&data, detail);
        if (status != FtpStatus::Ok) return status;
        const std::string command = attempt == 0 ? verb + " " + encoded : verb;
        Reply reply;
        status = Command(command, &reply, detail);
        if (status != FtpStatus::Ok) {
            closesocket(data);
            return status;
        }
        if (reply.code != 150 && reply.code != 125 && reply.code != 226) {
            closesocket(data);
            SetDetail(detail, reply.text);
            const bool refused_argument = reply.code == 500 || reply.code == 501 || reply.code == 550;
            if (attempt != 0 || path == L"/" || !refused_argument) {
                if (reply.code == 550) return FtpStatus::NotFound;
                return reply.code == 530 ? FtpStatus::Auth : FtpStatus::Denied;
            }
            Reply enter;
            const FtpStatus cwd = Command("CWD " + encoded, &enter, detail);
            if (cwd != FtpStatus::Ok) return FtpStatus::NotFound;
            if (enter.code != 250 && enter.code != 200) {
                SetDetail(detail, enter.text);
                return FtpStatus::NotFound;
            }
            continue;
        }

        if (reply.code == 226) {
            // An empty folder on some servers: the completion arrives together
            // with the preliminary reply, so the data connection carries
            // nothing and there is no second reply to wait for.
            closesocket(data);
            break;
        }

        bool aborted = false;
        for (;;) {
            if (cancelled && cancelled()) {
                aborted = true;
                break;
            }
            char chunk[kChunk];
            const int read = RecvSome(data, chunk, kChunk);
            if (read > 0) {
                if (listing.size() + static_cast<size_t>(read) > kMaxListingBytes) {
                    aborted = true;
                    break;
                }
                listing.append(chunk, static_cast<size_t>(read));
                continue;
            }
            if (read == 0) break;  // the server closes the data connection when done
            if (WSAGetLastError() == WSAETIMEDOUT) {
                closesocket(data);
                Reset();
                SetDetail(detail, L"the listing did not finish in time");
                return FtpStatus::Timeout;
            }
            break;
        }
        closesocket(data);
        if (aborted) {
            Abort();
            return FtpStatus::Cancelled;
        }
        status = FinishTransfer(&reply, detail);
        if (status != FtpStatus::Ok) return status;
        break;
    }

    size_t cursor = 0;
    while (cursor <= listing.size()) {
        const size_t end = listing.find('\n', cursor);
        const size_t stop = end == std::string::npos ? listing.size() : end;
        std::string line = listing.substr(cursor, stop - cursor);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        cursor = stop + 1;
        FtpItem item;
        const bool parsed = mlsd_ ? ParseFtpMlsdRow(line, item) : ParseFtpListRow(line, item);
        if (parsed) out.push_back(std::move(item));
        if (end == std::string::npos) break;
    }
    std::sort(out.begin(), out.end(), [](const FtpItem& a, const FtpItem& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return FtpStatus::Ok;
}

FtpStatus FtpClient::Stat(const std::wstring& path, uint64_t* size, FILETIME* mtime, bool* has_mtime,
                          std::wstring* detail) {
    if (size) *size = 0;
    if (mtime) *mtime = FILETIME{};
    if (has_mtime) *has_mtime = false;
    Reply reply;
    const std::string encoded = FtpUtf8(FtpPercentEncode(path));
    if (Command("SIZE " + encoded, &reply, detail) == FtpStatus::Ok && reply.code == 213) {
        const size_t space = reply.text.find(' ');
        if (size) *size = strtoull(reply.text.c_str() + (space == std::string::npos ? 0 : space + 1), nullptr, 10);
    }
    if (Command("MDTM " + encoded, &reply, detail) == FtpStatus::Ok && reply.code == 213) {
        const size_t space = reply.text.find(' ');
        if (space != std::string::npos && reply.text.size() >= space + 1 + 14) {
            const std::string stamp = reply.text.substr(space + 1, 14);
            SYSTEMTIME parts{};
            parts.wYear = static_cast<WORD>(atoi(stamp.substr(0, 4).c_str()));
            parts.wMonth = static_cast<WORD>(atoi(stamp.substr(4, 2).c_str()));
            parts.wDay = static_cast<WORD>(atoi(stamp.substr(6, 2).c_str()));
            parts.wHour = static_cast<WORD>(atoi(stamp.substr(8, 2).c_str()));
            parts.wMinute = static_cast<WORD>(atoi(stamp.substr(10, 2).c_str()));
            parts.wSecond = static_cast<WORD>(atoi(stamp.substr(12, 2).c_str()));
            FILETIME value{};
            if (mtime && SystemTimeToFileTime(&parts, &value)) {
                *mtime = value;
                if (has_mtime) *has_mtime = true;
            }
        }
    }
    return FtpStatus::Ok;
}

FtpStatus FtpClient::Download(const std::wstring& remote, const std::wstring& local,
                              const FtpCancel& cancelled, const FtpProgress& progress,
                              std::wstring* detail) {
    HANDLE file = CreateFileW(local.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetDetail(detail, L"cannot write " + local);
        return FtpStatus::LocalIo;
    }
    uint64_t total = 0;
    Stat(remote, &total, nullptr, nullptr, nullptr);

    SOCKET data = INVALID_SOCKET;
    FtpStatus status = OpenData(&data, detail);
    if (status != FtpStatus::Ok) {
        CloseHandle(file);
        DeleteFileW(local.c_str());
        return status;
    }
    const std::string encoded = FtpUtf8(FtpPercentEncode(remote));
    Reply reply;
    status = Command("RETR " + encoded, &reply, detail);
    if (status != FtpStatus::Ok || (reply.code != 150 && reply.code != 125 && reply.code != 226)) {
        closesocket(data);
        CloseHandle(file);
        DeleteFileW(local.c_str());
        if (status == FtpStatus::Ok) SetDetail(detail, reply.text);
        if (reply.code == 550) return FtpStatus::NotFound;
        return status == FtpStatus::Ok ? FtpStatus::Denied : status;
    }
    uint64_t written = 0;
    bool failed = false;
    if (reply.code == 226) {
        // The server answered the whole transfer at once: there is no data to
        // receive, and no second reply to wait for either.
        closesocket(data);
        CloseHandle(file);
        return FtpStatus::Ok;
    }
    for (;;) {
        if (cancelled && cancelled()) break;
        char chunk[kChunk];
        const int read = RecvSome(data, chunk, kChunk);
        if (read > 0) {
            DWORD put = 0;
            if (!WriteFile(file, chunk, static_cast<DWORD>(read), &put, nullptr) || put != static_cast<DWORD>(read)) {
                SetDetail(detail, L"cannot write " + local);
                failed = true;
                break;
            }
            written += static_cast<uint64_t>(read);
            if (progress && !progress(written, total)) {
                SetDetail(detail, L"cancelled");
                failed = true;
                break;
            }
            continue;
        }
        if (read == 0) break;
        if (WSAGetLastError() == WSAETIMEDOUT) {
            SetDetail(detail, L"the download stalled");
            failed = true;
        }
        break;
    }
    closesocket(data);
    CloseHandle(file);
    if (failed) {
        DeleteFileW(local.c_str());
        Abort();
        return FtpStatus::Cancelled;
    }
    status = FinishTransfer(&reply, detail);
    if (status != FtpStatus::Ok) {
        DeleteFileW(local.c_str());
        return status;
    }
    return FtpStatus::Ok;
}

FtpStatus FtpClient::Upload(const std::wstring& local, const std::wstring& remote,
                            const FtpCancel& cancelled, const FtpProgress& progress,
                            std::wstring* detail) {
    HANDLE file = CreateFileW(local.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetDetail(detail, L"cannot read " + local);
        return FtpStatus::LocalIo;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    const uint64_t total = static_cast<uint64_t>(size.QuadPart);

    SOCKET data = INVALID_SOCKET;
    FtpStatus status = OpenData(&data, detail);
    if (status != FtpStatus::Ok) {
        CloseHandle(file);
        return status;
    }
    const std::string encoded = FtpUtf8(FtpPercentEncode(remote));
    Reply reply;
    status = Command("STOR " + encoded, &reply, detail);
    if (status != FtpStatus::Ok || (reply.code != 150 && reply.code != 125)) {
        closesocket(data);
        CloseHandle(file);
        if (status == FtpStatus::Ok) SetDetail(detail, reply.text);
        return status == FtpStatus::Ok ? FtpStatus::Denied : status;
    }
    uint64_t sent = 0;
    bool failed = false;
    for (;;) {
        if (cancelled && cancelled()) break;
        char chunk[kChunk];
        DWORD read = 0;
        if (!ReadFile(file, chunk, kChunk, &read, nullptr)) {
            SetDetail(detail, L"cannot read " + local);
            failed = true;
            break;
        }
        if (read == 0) break;
        if (!SendAll(data, chunk, static_cast<size_t>(read))) {
            SetDetail(detail, L"the connection dropped");
            failed = true;
            break;
        }
        sent += static_cast<uint64_t>(read);
        if (progress && !progress(sent, total)) {
            failed = true;
            break;
        }
    }
    CloseHandle(file);
    shutdown(data, SD_SEND);
    closesocket(data);
    if (failed) {
        Abort();
        return FtpStatus::Cancelled;
    }
    return FinishTransfer(&reply, detail);
}

FtpStatus FtpClient::Remove(const std::wstring& path, std::wstring* detail) {
    Reply reply;
    const std::string encoded = FtpUtf8(FtpPercentEncode(path));
    const FtpStatus status = Command("DELE " + encoded, &reply, detail);
    if (status != FtpStatus::Ok) return status;
    if (reply.code == 250 || reply.code == 200) return FtpStatus::Ok;
    if (reply.code == 550) return FtpStatus::NotFound;
    SetDetail(detail, reply.text);
    return FtpStatus::Denied;
}

FtpStatus FtpClient::RemoveDirectory(const std::wstring& path, std::wstring* detail) {
    Reply reply;
    const std::string encoded = FtpUtf8(FtpPercentEncode(path));
    const FtpStatus status = Command("RMD " + encoded, &reply, detail);
    if (status != FtpStatus::Ok) return status;
    if (reply.code == 250 || reply.code == 200) return FtpStatus::Ok;
    if (reply.code == 550) return FtpStatus::NotFound;
    SetDetail(detail, reply.text);
    return FtpStatus::Denied;
}

FtpStatus FtpClient::MakeDirectory(const std::wstring& path, std::wstring* detail) {
    Reply reply;
    const std::string encoded = FtpUtf8(FtpPercentEncode(path));
    const FtpStatus status = Command("MKD " + encoded, &reply, detail);
    if (status != FtpStatus::Ok) return status;
    if (reply.code == 257 || reply.code == 250) return FtpStatus::Ok;
    SetDetail(detail, reply.text);
    return FtpStatus::Denied;
}

FtpStatus FtpClient::Rename(const std::wstring& from, const std::wstring& to, std::wstring* detail) {
    Reply reply;
    FtpStatus status = Command("RNFR " + FtpUtf8(FtpPercentEncode(from)), &reply, detail);
    if (status != FtpStatus::Ok) return status;
    if (reply.code != 350 && reply.code != 250) {
        if (reply.code == 550) return FtpStatus::NotFound;
        SetDetail(detail, reply.text);
        return FtpStatus::Denied;
    }
    status = Command("RNTO " + FtpUtf8(FtpPercentEncode(to)), &reply, detail);
    if (status != FtpStatus::Ok) return status;
    if (reply.code == 250 || reply.code == 200) return FtpStatus::Ok;
    SetDetail(detail, reply.text);
    return FtpStatus::Denied;
}

}  // namespace pulse::fs
