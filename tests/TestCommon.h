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

#define LAU_REQUIRE_MSG(expr, msg)                                                                                     \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            throw std::runtime_error(std::string("Requirement failed: ") + #expr + " -- " + (msg));                    \
        }                                                                                                              \
    } while (false)

using TestFn = void (*)();

struct TestCase {
    const char* name;
    TestFn fn;
};
