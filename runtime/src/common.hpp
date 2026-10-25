// Shared runtime definitions: logging and the errors that unwind the game thread.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace willow {

void log(const char *fmt, ...);
// Logs a message only the first time `key` is seen, for things that repeat every frame.
void log_once(const std::string &key, const char *fmt, ...);
std::string format(const char *fmt, ...);

// Something the runtime cannot continue from: a missing function, an unsupported call.
struct Fatal : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Thrown into the game thread to stop it, unwinding through the recompiled code.
struct Shutdown {};

}  // namespace willow
