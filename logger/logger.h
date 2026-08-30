#pragma once

#include <string_view>

#include <fmt/format.h>

enum class LogLevel {
    kDebug,
    kInfo,
    kWarning,
    kError,
};

class Logger {
   public:
    static void SetLevel(LogLevel level) { min_level_.store(level, std::memory_order_relaxed); }

    template <typename... Args>
    static void Log(LogLevel level, fmt::format_string<Args...> format, Args&&... args) {
        // Early-out BEFORE formatting: the arguments are never converted to
        // strings for a suppressed level, which is the whole point (§7).
        if (level < min_level_.load(std::memory_order_relaxed)) {
            return;
        }
        fmt::print("[{}] {}\n", ToString(level), fmt::vformat(format.get(), fmt::make_format_args(args...)));
    }

   private:
    static std::string_view ToString(LogLevel level) {
        switch (level) {
            case LogLevel::kDebug:
                return "DEBUG";
            case LogLevel::kInfo:
                return "INFO";
            case LogLevel::kWarning:
                return "WARNING";
            case LogLevel::kError:
                return "ERROR";
        }

        return "UNKNOWN";
    }
    static inline std::atomic<LogLevel> min_level_{LogLevel::kInfo};
};
