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
    template <typename... Args>
    static void Log(LogLevel level, fmt::format_string<Args...> format, Args&&... args) {
        fmt::print("[{}] {}\n", ToString(level), fmt::format(format, args...));
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
};
