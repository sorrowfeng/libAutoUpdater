cmake_minimum_required(VERSION 3.16)

project(libAutoUpdaterConsumerProbe LANGUAGES CXX)

find_package(libAutoUpdater CONFIG REQUIRED)

add_executable(libAutoUpdater_consumer_probe main.cpp)
target_compile_features(libAutoUpdater_consumer_probe PRIVATE cxx_std_17)
target_link_libraries(libAutoUpdater_consumer_probe PRIVATE libAutoUpdater::libAutoUpdater)

