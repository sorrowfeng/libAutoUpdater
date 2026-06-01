add_executable(libAutoUpdater_tests
    main.cpp
    VersionTests.cpp
    ManifestTests.cpp
    UpdatePlannerTests.cpp
    ApplyPlanTests.cpp
    HashTests.cpp)

target_compile_features(libAutoUpdater_tests PRIVATE cxx_std_17)
target_link_libraries(libAutoUpdater_tests PRIVATE libAutoUpdater)
target_include_directories(libAutoUpdater_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME libAutoUpdater_tests COMMAND libAutoUpdater_tests)
