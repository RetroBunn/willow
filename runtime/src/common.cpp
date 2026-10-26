#include "common.hpp"

#include <mutex>
#include <set>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace willow {

static std::mutex log_mutex;

static std::string vformat(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int n = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    std::vector<char> buf(n > 0 ? n + 1 : 1);
    std::vsnprintf(buf.data(), buf.size(), fmt, args);
    return std::string(buf.data());
}

std::string format(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::string s = vformat(fmt, args);
    va_end(args);
    return s;
}

#ifdef _WIN32
static std::wstring widen(const std::string &s, unsigned code_page)
{
    int n = MultiByteToWideChar(code_page, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(code_page, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
#endif

std::string from_sjis(const std::string &text)
{
    std::string in;
    for (char c : text)  // a two-byte character's second byte is never a control code
        if (static_cast<unsigned char>(c) >= 0x20) in += c;
#ifdef _WIN32
    std::wstring w = widen(in, 932);
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
#else
    iconv_t cd = iconv_open("UTF-8", "CP932");
    if (cd == iconv_t(-1)) return in;
    std::string out(in.size() * 3 / 2 + 4, '\0');  // two bytes become three at most
    char *src = in.data(), *dst = out.data();
    size_t left = in.size(), room = out.size();
    iconv(cd, &src, &left, &dst, &room);
    iconv_close(cd);
    out.resize(out.size() - room);
    return out;
#endif
}

void log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::string s = "[willow] " + vformat(fmt, args) + "\n";
    va_end(args);
    std::lock_guard<std::mutex> lock(log_mutex);
#ifdef _WIN32
    // A Windows console shows UTF-8 only in the UTF-8 code page; hand it UTF-16 instead.
    HANDLE console = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode;
    if (console != INVALID_HANDLE_VALUE && GetConsoleMode(console, &mode)) {
        std::wstring w = widen(s, CP_UTF8);
        WriteConsoleW(console, w.data(), DWORD(w.size()), nullptr, nullptr);
        return;
    }
#endif
    std::fputs(s.c_str(), stderr);
}

void log_once(const std::string &key, const char *fmt, ...)
{
    static std::set<std::string> seen;
    {
        std::lock_guard<std::mutex> lock(log_mutex);
        if (!seen.insert(key).second) return;
    }
    va_list args;
    va_start(args, fmt);
    std::string s = vformat(fmt, args);
    va_end(args);
    log("%s", s.c_str());
}

}  // namespace willow
