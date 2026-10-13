#pragma once

#include <stdexcept>

namespace ff7cc {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
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
