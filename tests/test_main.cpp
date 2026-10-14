#include "test_main.hpp"

#include <exception>

int main() {
    int failed = 0;
    for (const auto& c : psprecore::test::registry()) {
        try {
            c.fn();
            std::printf("[pass] %s\n", c.name);
        } catch (const psprecore::test::Failure& f) {
            ++failed;
            std::printf("[FAIL] %s\n       %s\n", c.name, f.message.c_str());
        } catch (const std::exception& e) {
            ++failed;
            std::printf("[FAIL] %s\n       unexpected exception: %s\n", c.name, e.what());
        }
    }
    std::printf("%zu tests, %d failed\n", psprecore::test::registry().size(), failed);
    return failed == 0 ? 0 : 1;
}
