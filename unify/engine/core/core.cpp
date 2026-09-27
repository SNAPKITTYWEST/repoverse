#include "core.h"
#include <chrono>
#include <mutex>

namespace unify {

uint64_t wall_clock_ns() {
    static const auto start = std::chrono::steady_clock::now();
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

static LogLevel g_min_level = LogLevel::Info;
static std::mutex g_log_mutex;

void set_log_level(LogLevel min_level) { g_min_level = min_level; }

void log_message(LogLevel level, const char* category, const char* fmt, ...) {
    if (level < g_min_level) return;
    static const char* names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::fprintf(level >= LogLevel::Warn ? stderr : stdout, "[%s] [%s] %s\n", names[int(level)], category, buf);
}

}  // namespace unify
