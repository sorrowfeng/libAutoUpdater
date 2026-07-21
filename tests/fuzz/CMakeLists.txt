cmake_minimum_required(VERSION 3.16)

project(libAutoUpdaterFuzzers LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang)$" OR MSVC)
    message(FATAL_ERROR
        "The persistent fuzz targets require a GCC-style Clang compiler with libFuzzer support")
endif()

get_filename_component(LIBAUTOUPDATER_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# Instrument the library as well as the harness so libFuzzer can guide itself
# using coverage from the actual parsers and policy helpers. This is a
# standalone build and therefore cannot affect normal project configurations.
add_compile_options(
    -fsanitize=fuzzer-no-link,address,undefined
    -fno-omit-frame-pointer)
add_link_options(-fsanitize=address,undefined)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LIBAUTOUPDATER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBAUTOUPDATER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBAUTOUPDATER_BUILD_UPDATER OFF CACHE BOOL "" FORCE)
set(LIBAUTOUPDATER_WITH_CURL OFF CACHE BOOL "" FORCE)
set(LIBAUTOUPDATER_WITH_OPENSSL OFF CACHE BOOL "" FORCE)
set(LIBAUTOUPDATER_WITH_QT OFF CACHE BOOL "" FORCE)
set(LIBAUTOUPDATER_ENABLE_SANITIZERS OFF CACHE BOOL "" FORCE)

add_subdirectory("${LIBAUTOUPDATER_SOURCE_DIR}" libAutoUpdater-source EXCLUDE_FROM_ALL)

add_executable(libAutoUpdater_parser_fuzzer ParserAndPathFuzzer.cpp)
target_compile_features(libAutoUpdater_parser_fuzzer PRIVATE cxx_std_17)
target_include_directories(libAutoUpdater_parser_fuzzer PRIVATE
    "${LIBAUTOUPDATER_SOURCE_DIR}/src")
target_link_libraries(libAutoUpdater_parser_fuzzer PRIVATE
    libAutoUpdater::libAutoUpdater)
target_link_options(libAutoUpdater_parser_fuzzer PRIVATE -fsanitize=fuzzer)
