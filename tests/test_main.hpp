#pragma once

// Minimal self-registering test harness (no external dependencies).

#include <cstdio>
#include <functional>

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/strings/format.h"

namespace psprecore::test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline nx::vector<Case>& registry() {
    static nx::vector<Case> cases;
    return cases;
}

struct Failure {
    nx::string message;
};

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace psprecore::test

#define PSPRECORE_CONCAT_(a, b) a##b
#define PSPRECORE_CONCAT(a, b) PSPRECORE_CONCAT_(a, b)

#define TEST_CASE(name)                                                                               \
    static void PSPRECORE_CONCAT(test_fn_, __LINE__)();                                                   \
    static const ::psprecore::test::Registrar PSPRECORE_CONCAT(test_reg_, __LINE__){name,                     \
                                                                            &PSPRECORE_CONCAT(test_fn_, __LINE__)}; \
    static void PSPRECORE_CONCAT(test_fn_, __LINE__)()

#define CHECK(expr)                                                                                    \
    do {                                                                                               \
        if (!(expr))                                                                                   \
            throw ::psprecore::test::Failure{nx::string(__FILE__) + ":" + nx::format("{}", __LINE__) + ": CHECK(" #expr ")"}; \
    } while (0)

#define CHECK_THROWS_AS(expr, type)                                                                    \
    do {                                                                                               \
        bool caught_ = false;                                                                          \
        try {                                                                                          \
            (void)(expr);                                                                              \
        } catch (const type&) {                                                                        \
            caught_ = true;                                                                            \
        }                                                                                              \
        CHECK(caught_ && "expected " #type);                                                           \
    } while (0)
