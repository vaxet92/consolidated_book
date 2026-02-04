#pragma once
#include <string>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <fast_float/fast_float.h>  // Added for fast double parsing

namespace Utilities {

inline uint64_t GetCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

inline std::optional<std::string> ReadNextJsonObject(std::ifstream& in) {
    std::string line;
    std::string json;
    int depth = 0;
    bool started = false;

    while (std::getline(in, line)) {
        if (!started) {
            auto pos = line.find('{');
            if (pos == std::string::npos) continue;
            started = true;
            line = line.substr(pos);
        }

        json += line;
        json.push_back('\n');

        for (char c : line) {
            if (c == '{')
                depth++;
            else if (c == '}')
                depth--;
        }

        if (started && depth == 0) {
            return json;
        }
    }
    return std::nullopt;
}

inline double ParseDouble(std::string_view sv) noexcept {
    double v;
    fast_float::from_chars(sv.data(), sv.data() + sv.size(), v);
    return v;
}

inline uint64_t ParseUint64(std::string_view sv) noexcept {
    uint64_t v;
    fast_float::from_chars(sv.data(), sv.data() + sv.size(), v);
    return v;
}
}  // namespace Utilities