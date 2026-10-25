#include "common.hpp"

#include <mutex>
#include <set>
#include <vector>

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

void log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::string s = vformat(fmt, args);
    va_end(args);
    std::lock_guard<std::mutex> lock(log_mutex);
    std::fprintf(stderr, "[willow] %s\n", s.c_str());
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
