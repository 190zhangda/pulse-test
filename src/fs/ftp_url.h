// ftp_url.h - ftp:// addresses as strings: parsing, normalising and the
// percent-encoding the protocol needs. Pure string work (no sockets, no name
// resolution) so the address bar, the listing and the tests share it.
//
// A tab whose folder lives on an FTP server keeps that folder as an
// "ftp://host:port/path" string: fs::IsVirtualPath() reports it as virtual, so
// the parts of the app that only understand Win32 paths leave it alone, and
// the listing/transfer code in ftp_source.h does the network work.
#pragma once
#include <cstdint>
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

// True for text that is meant as an FTP address ("ftp://..."), including the
// malformed ones: the address bar then explains instead of searching.
bool IsFtpUrl(std::wstring_view text);

// Parses "ftp://[user[:password]@]host[:port][/path]". Text without the
// ftp:// scheme, without a host, or with a non-numeric/out-of-range port
// returns a value with valid == false.
bool ParseFtpUrl(std::wstring_view text, FtpUrl& out);

// "ftp://user@host:2121/dir" - user kept (the server needs it), password never.
// Port 21 is left out.
std::wstring FtpCanonicalText(const FtpUrl& url);
// The same for a folder inside that server.
std::wstring FtpTextForPath(const FtpUrl& base, std::wstring_view path);

// "/a/b" -> "/a", "/a" -> "/", "/" -> "/".
std::wstring FtpParentPath(std::wstring_view path);
// "/a/b.txt" -> "b.txt", "/" -> "".
std::wstring FtpLeafName(std::wstring_view path);
// "/dir" + "name" -> "/dir/name"; both sides may or may not carry slashes.
std::wstring FtpJoinPath(std::wstring_view dir, std::wstring_view name);
// Collapses "//", resolves no "." or ".." (the server decides), keeps a
// leading '/' and drops a trailing one.
std::wstring FtpNormalizePath(std::wstring_view path);

// "%E4%B8%AD" -> the decoded text: bytes are read as UTF-8, and as the ANSI
// code page when they are not valid UTF-8 (old servers send GBK names).
std::wstring FtpPercentDecode(std::wstring_view text);
// The inverse, escaping everything outside A-Z a-z 0-9 - . _ ~ and '/'.
std::wstring FtpPercentEncode(std::wstring_view text);

std::string FtpUtf8(std::wstring_view text);
std::wstring FtpFromUtf8(std::string_view bytes);

}  // namespace pulse::fs
