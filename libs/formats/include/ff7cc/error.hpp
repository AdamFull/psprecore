#pragma once

#include <stdexcept>

#include "core/foundation/strings/utf8_string.h"

namespace ff7cc {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
    explicit Error(nx::string_view message) : std::runtime_error(nx::string(message).c_str()) {}
};

// File system / stream failure.
struct IoError : Error {
    using Error::Error;
};

// Input data does not match the expected format.
struct FormatError : Error {
    using Error::Error;
};

}  // namespace ff7cc
