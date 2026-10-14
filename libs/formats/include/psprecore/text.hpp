#pragma once

// Number parsing for command lines and text files: the whole text must be a number (no locale, no partial parses).

#include <charconv>
#include <cstdint>

#include "core/foundation/serialization/number_parse.h"
#include "core/foundation/strings/utf8_string.h"
#include "psprecore/error.hpp"

namespace psprecore {

// Base 0: decimal, or hexadecimal with a 0x prefix. Throws Error when `text` is not a number of that base.
inline std::uint64_t parse_u64(nx::string_view text, int base = 10) {
    if (base == 0) {
        const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
        if (hex) text.remove_prefix(2);
        base = hex ? 16 : 10;
    }
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty())
        throw Error(nx::string("not a number: '") + text + "'");
    return value;
}

inline std::int64_t parse_i64(nx::string_view text, int base = 10) {
    const bool negative = !text.empty() && text[0] == '-';
    if (negative) text.remove_prefix(1);
    const auto magnitude = static_cast<std::int64_t>(parse_u64(text, base));
    return negative ? -magnitude : magnitude;
}

inline double parse_double(nx::string_view text) {
    double value = 0;
    const auto [end, error] = nx::number::parse_f64(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty())
        throw Error(nx::string("not a number: '") + text + "'");
    return value;
}

}  // namespace psprecore
