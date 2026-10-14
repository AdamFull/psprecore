#pragma once

// Minimal self-registering test harness (no external dependencies).

#include <cstdio>
#include <functional>

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/strings/format.h"

namespace ff7cc::test {

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

}  // namespace ff7cc::test

#define FF7CC_CONCAT_(a, b) a##b
#define FF7CC_CONCAT(a, b) FF7CC_CONCAT_(a, b)

#define TEST_CASE(name)                                                                               \
    static void FF7CC_CONCAT(test_fn_, __LINE__)();                                                   \
    static const ::ff7cc::test::Registrar FF7CC_CONCAT(test_reg_, __LINE__){name,                     \
                                                                            &FF7CC_CONCAT(test_fn_, __LINE__)}; \
    static void FF7CC_CONCAT(test_fn_, __LINE__)()

#define CHECK(expr)                                                                                    \
    do {                                                                                               \
        if (!(expr))                                                                                   \
            throw ::ff7cc::test::Failure{nx::string(__FILE__) + ":" + nx::format("{}", __LINE__) + ": CHECK(" #expr ")"}; \
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
