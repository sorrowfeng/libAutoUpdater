cmake_minimum_required(VERSION 3.16)

project(libAutoUpdaterConsumerProbe LANGUAGES CXX)

find_package(libAutoUpdater CONFIG REQUIRED)

if(NOT TARGET libAutoUpdater::autoupdater_apply)
    message(FATAL_ERROR "The installed package does not export libAutoUpdater::autoupdater_apply")
endif()

get_target_property(libautoupdater_helper_type
    libAutoUpdater::autoupdater_apply TYPE)
if(NOT libautoupdater_helper_type STREQUAL "EXECUTABLE")
    message(FATAL_ERROR
        "libAutoUpdater::autoupdater_apply must be an imported executable target")
endif()

add_executable(libAutoUpdater_consumer_probe main.cpp)
target_compile_features(libAutoUpdater_consumer_probe PRIVATE cxx_std_17)
target_link_libraries(libAutoUpdater_consumer_probe PRIVATE libAutoUpdater::libAutoUpdater)

# Referencing the imported executable through a generator expression verifies
# that its installed location is complete and usable by downstream projects.
add_custom_target(libAutoUpdater_consumer_helper_probe ALL
    COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:libAutoUpdater::autoupdater_apply>
        ${CMAKE_CURRENT_BINARY_DIR}/referenced-autoupdater_apply${CMAKE_EXECUTABLE_SUFFIX}
    VERBATIM)

enable_testing()
add_test(NAME libAutoUpdater_consumer_probe COMMAND libAutoUpdater_consumer_probe)
