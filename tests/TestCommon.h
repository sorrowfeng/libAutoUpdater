#pragma once

#include <iostream>
#include <stdexcept>
#include <string>

#define LAU_REQUIRE(expr) \
    do { \
        if (!(expr)) { \
            throw std::runtime_error(std::string("Requirement failed: ") + #expr); \
        } \
    } while (false)

using TestFn = void (*)();

struct TestCase {
    const char* name;
    TestFn fn;
};

