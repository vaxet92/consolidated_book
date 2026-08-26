#pragma once

#include <cstdint>
#include <string_view>

// Parses a decimal string like "0.0024" into a scaled uint64_t (x scale
// digits, default 1e8 - matching PriceTicks/QtyUnits). No floating point
// anywhere: venue prices/quantities arrive as JSON strings specifically so
// they can be parsed exactly, and doing that through a double would
// reintroduce the rounding-error problem CLAUDE.md §7 rules out.
inline uint64_t ParseScaledDecimal(std::string_view sv, int scale_digits = 8) {
    size_t dot_pos = sv.find('.');
    std::string_view int_str = (dot_pos == std::string_view::npos) ? sv : sv.substr(0, dot_pos);
    std::string_view frac_str = (dot_pos == std::string_view::npos) ? std::string_view{} : sv.substr(dot_pos + 1);

    uint64_t integer_part = 0;
    for (char c : int_str) {
        integer_part = integer_part * 10 + static_cast<uint64_t>(c - '0');
    }

    uint64_t fractional_part = 0;
    int fractional_digits = 0;
    for (char c : frac_str) {
        if (fractional_digits >= scale_digits) break;
        fractional_part = fractional_part * 10 + static_cast<uint64_t>(c - '0');
        ++fractional_digits;
    }
    for (; fractional_digits < scale_digits; ++fractional_digits) {
        fractional_part *= 10;  // pad missing digits, e.g. "0.5" at scale 8 -> 50000000
    }

    uint64_t scale = 1;
    for (int i = 0; i < scale_digits; ++i) scale *= 10;

    return integer_part * scale + fractional_part;
}
