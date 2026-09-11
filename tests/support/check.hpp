#pragma once

/**
 * Shared assertion harness for the native test targets.
 *
 * Contract: every check records a failure and continues, so one run reports every
 * failing assertion instead of stopping at the first. main() returns report(), which
 * is non-zero when any check failed. This header defines no test cases and must not be
 * registered as a test source.
 */

#include <exception>
#include <iostream>
#include <string>

#include "context_hmi/engine.hpp"

namespace context_hmi_test {

/** Process-wide failure tally. Function-local storage keeps initialization ordered. */
inline int &failure_count() {
    static int count = 0;
    return count;
}

inline void check(bool condition, const std::string &message) {
    if (!condition) {
        ++failure_count();
        std::cerr << "FAIL: " << message << "\n";
    }
}

/**
 * Assert that fn() throws DomainError carrying exactly expected_code. A different
 * exception type, a different code, or no exception at all is a distinct failure so the
 * report says which of the three happened.
 */
template <typename Fn>
void expects_domain_error(Fn &&fn, const std::string &expected_code, const std::string &message) {
    try {
        fn();
    } catch (const context_hmi::DomainError &error) {
        check(error.code == expected_code,
              message + " (expected code '" + expected_code + "', got '" + error.code + "')");
        return;
    } catch (const std::exception &error) {
        check(false, message + " (wrong exception type: " + error.what() + ")");
        return;
    }
    check(false, message + " (no DomainError was thrown)");
}

/** Assert that fn() completes without throwing. */
template <typename Fn> void expects_no_throw(Fn &&fn, const std::string &message) {
    try {
        fn();
    } catch (const std::exception &error) {
        check(false, message + " (unexpected exception: " + error.what() + ")");
    }
}

inline int report(const std::string &suite) {
    if (failure_count() != 0) {
        std::cerr << failure_count() << " checks failed in " << suite << "\n";
        return 1;
    }
    std::cout << suite << " passed\n";
    return 0;
}

}  /* namespace context_hmi_test */
