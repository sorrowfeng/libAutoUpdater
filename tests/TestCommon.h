#pragma once

#include <iostream>
#include <stdexcept>
#include <string>

#define LAU_REQUIRE(expr)                                                                                              \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            throw std::runtime_error(std::string("Requirement failed: ") + #expr);                                     \
        }                                                                                                              \
    } while (false)

class TestSkipped final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

#define LAU_SKIP(reason) throw TestSkipped(reason)

using TestFn = void (*)();

struct TestCase {
    const char* name;
    TestFn fn;
};
