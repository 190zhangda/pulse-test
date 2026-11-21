// ftp_url.h - ftp:// addresses as strings: parsing, normalising and the
// percent-encoding the protocol needs. Pure string work (no sockets, no name
// resolution) so the address bar, the listing and the tests share it.
//
// A tab whose folder lives on an FTP server keeps that folder as an
// "ftp://host:port/path" string: fs::IsVirtualPath() reports it as virtual, so
// the parts of the app that only understand Win32 paths leave it alone, and
// the listing/transfer code in ftp_client.h does the network work.
//
// Everything here is inline: app_model.cpp and the other listing targets are
// compiled into several executables and DLLs, and a separate translation unit
// would leave each of them with an unresolved symbol.
#pragma once
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <string>
#include <string_view>

namespace pulse::fs {

struct FtpUrl {
    bool valid = false;
    std::wstring host;      // "192.168.1.7", "[fe80::1]" keeps its brackets
    std::wstring user;      // decoded; empty means anonymous
    std::wstring password;  // decoded; empty means no password
    uint16_t port = 21;
    // Decoded remote path: always starts with '/', never ends with one
    // (the root itself is "/").
    std::wstring path = L"/";
};

namespace ftp_detail {

inline std::wstring_view Trim(std::wstring_view text) {
    while (!text.empty() && iswspace(text.front())) text.remove_prefix(1);
    while (!text.empty() && iswspace(text.back())) text.remove_suffix(1);
    return text;
}

inline bool EqualsNoCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (towlower(a[i]) != towlower(b[i])) return false;
    return true;
}

inline int HexDigit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

inline bool IsPlainCharacter(wchar_t c) {
    if ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')) return true;
    return c == L'-' || c == L'.' || c == L'_' || c == L'~';
}

}  // namespace ftp_detail

inline std::string FtpUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                            out.data(), needed, nullptr, nullptr);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written));
    return out;
}

inline std::wstring FtpFromUtf8(std::string_view bytes) {
    if (bytes.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                           static_cast<int>(bytes.size()), nullptr, 0);
    if (needed > 0) {
        std::wstring out(static_cast<size_t>(needed), L'\0');
        const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                                static_cast<int>(bytes.size()), out.data(), needed);
        if (written > 0) {
            out.resize(static_cast<size_t>(written));
            return out;
        }
    }
    // Not UTF-8: an old server sent the local code page (GBK on a Chinese PC).
    const int ansi = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()),
                                         nullptr, 0);
    if (ansi <= 0) return {};
    std::wstring out(static_cast<size_t>(ansi), L'\0');
    const int written = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()),
                                            out.data(), ansi);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written));
    return out;
}

// Collapses "//", resolves no "." or ".." (the server decides), keeps a
// leading '/' and drops a trailing one.
inline std::wstring FtpNormalizePath(std::wstring_view path) {
    std::wstring out;
    out.reserve(path.size() + 1);
    for (wchar_t c : path) {
        if (c == L'\\') c = L'/';
        if (c == L'/' && !out.empty() && out.back() == L'/') continue;
        out.push_back(c);
    }
    if (out.empty() || out.front() != L'/') out.insert(out.begin(), L'/');
    while (out.size() > 1 && out.back() == L'/') out.pop_back();
    return out;
}

// "%E4%B8%AD" -> the decoded text: bytes are read as UTF-8, and as the ANSI
// code page when they are not valid UTF-8 (old servers send GBK names).
inline std::wstring FtpPercentDecode(std::wstring_view text) {
    std::string bytes;
    bytes.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c == L'%' && i + 2 < text.size()) {
            const int high = ftp_detail::HexDigit(text[i + 1]);
            const int low = ftp_detail::HexDigit(text[i + 2]);
            if (high >= 0 && low >= 0) {
                bytes.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        const std::string encoded = FtpUtf8(std::wstring_view(&c, 1));
        bytes.append(encoded);
    }
    return FtpFromUtf8(bytes);
}

// The inverse, escaping everything outside A-Z a-z 0-9 - . _ ~ and '/'.
inline std::wstring FtpPercentEncode(std::wstring_view text) {
    const std::string bytes = FtpUtf8(text);
    std::wstring out;
    out.reserve(bytes.size());
    char buffer[4]{};
    for (unsigned char byte : bytes) {
        if (byte < 0x80 && (ftp_detail::IsPlainCharacter(static_cast<wchar_t>(byte)) || byte == '/')) {
            out.push_back(static_cast<wchar_t>(byte));
            continue;
        }
        _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%%%02X", static_cast<unsigned>(byte));
        for (int i = 0; i < 3; ++i) out.push_back(static_cast<wchar_t>(buffer[i]));
    }
    return out;
}

// "/a/b" -> "/a", "/a" -> "/", "/" -> "/".
inline std::wstring FtpParentPath(std::wstring_view path) {
    const std::wstring normalized = FtpNormalizePath(path);
    if (normalized == L"/") return normalized;
    const size_t cut = normalized.find_last_of(L'/');
    if (cut == std::wstring::npos || cut == 0) return L"/";
    return normalized.substr(0, cut);
}

// "/a/b.txt" -> "b.txt", "/" -> "".
inline std::wstring FtpLeafName(std::wstring_view path) {
    const std::wstring normalized = FtpNormalizePath(path);
    if (normalized == L"/") return {};
    const size_t cut = normalized.find_last_of(L'/');
    return cut == std::wstring::npos ? normalized : normalized.substr(cut + 1);
}

// "/dir" + "name" -> "/dir/name"; both sides may or may not carry slashes.
inline std::wstring FtpJoinPath(std::wstring_view dir, std::wstring_view name) {
    std::wstring base = FtpNormalizePath(dir);
    std::wstring leaf(name);
    while (!leaf.empty() && (leaf.front() == L'/' || leaf.front() == L'\\')) leaf.erase(leaf.begin());
    while (!leaf.empty() && (leaf.back() == L'/' || leaf.back() == L'\\')) leaf.pop_back();
    if (leaf.empty()) return base;
    if (base == L"/") return L"/" + leaf;
    return base + L"/" + leaf;
}

// True for text that is meant as an FTP address ("ftp://..."), including the
// malformed ones: the address bar then explains instead of searching.
inline bool IsFtpUrl(std::wstring_view text) {
    const std::wstring_view trimmed = ftp_detail::Trim(text);
    return trimmed.size() > 6 && ftp_detail::EqualsNoCase(trimmed.substr(0, 6), L"ftp://");
}

// Parses "ftp://[user[:password]@]host[:port][/path]". Text without the
// ftp:// scheme, without a host, or with a non-numeric/out-of-range port
// returns a value with valid == false.
inline bool ParseFtpUrl(std::wstring_view text, FtpUrl& out) {
    out = FtpUrl{};
    const std::wstring_view trimmed = ftp_detail::Trim(text);
    if (!IsFtpUrl(trimmed)) return false;
    std::wstring_view rest = trimmed.substr(6);

    std::wstring_view authority = rest;
    std::wstring_view raw_path;
    if (const size_t slash = rest.find_first_of(L"/\\"); slash != std::wstring::npos) {
        authority = rest.substr(0, slash);
        raw_path = rest.substr(slash);
    }
    if (const size_t cut = authority.find_first_of(L"?#"); cut != std::wstring::npos)
        authority = authority.substr(0, cut);

    std::wstring_view user_info;
    if (const size_t at = authority.rfind(L'@'); at != std::wstring::npos) {
        user_info = authority.substr(0, at);
        authority = authority.substr(at + 1);
    }
    if (!user_info.empty()) {
        const size_t colon = user_info.find(L':');
        if (colon == std::wstring::npos) {
            out.user = FtpPercentDecode(user_info);
        } else {
            out.user = FtpPercentDecode(user_info.substr(0, colon));
            out.password = FtpPercentDecode(user_info.substr(colon + 1));
        }
    }

    std::wstring_view host = authority;
    std::wstring port_text;
    if (!host.empty() && host.front() == L'[') {
        const size_t close = host.find(L']');
        if (close == std::wstring_view::npos) return false;
        if (close + 1 < host.size()) {
            if (host[close + 1] != L':') return false;
            port_text = std::wstring(host.substr(close + 2));
        }
        host = host.substr(0, close + 1);  // keep the brackets for the socket call
    } else if (const size_t colon = host.rfind(L':'); colon != std::wstring_view::npos) {
        port_text = std::wstring(host.substr(colon + 1));
        host = host.substr(0, colon);
    }
    if (host.empty()) return false;
    for (wchar_t c : host)
        if (iswspace(c) || c == L'/' || c == L'\\' || c == L'@') return false;
    out.host = std::wstring(host);

    if (!port_text.empty()) {
        int port = 0;
        for (wchar_t c : port_text) {
            if (c < L'0' || c > L'9') return false;
            port = port * 10 + (c - L'0');
            if (port > 65535) return false;
        }
        if (port <= 0) return false;
        out.port = static_cast<uint16_t>(port);
    }

    out.path = raw_path.empty() ? std::wstring(L"/") : FtpNormalizePath(FtpPercentDecode(raw_path));
    out.valid = true;
    return true;
}

// "ftp://user@host:2121/dir" - user kept (the server needs it), password never.
// Port 21 is left out.
inline std::wstring FtpCanonicalText(const FtpUrl& url) {
    std::wstring out = L"ftp://";
    if (!url.user.empty()) {
        out += FtpPercentEncode(url.user);
        out.push_back(L'@');
    }
    out += url.host;
    if (url.port != 21) {
        out.push_back(L':');
        out += std::to_wstring(url.port);
    }
    out += url.path.empty() ? std::wstring(L"/") : url.path;
    return out;
}

// The same for a folder inside that server.
inline std::wstring FtpTextForPath(const FtpUrl& base, std::wstring_view path) {
    FtpUrl swapped = base;
    swapped.path = FtpNormalizePath(path);
    return FtpCanonicalText(swapped);
}

}  // namespace pulse::fs
