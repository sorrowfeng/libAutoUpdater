add_executable(libAutoUpdater_tests
    ${CMAKE_SOURCE_DIR}/updater/ApplyExecutor.cpp
    main.cpp
    VersionTests.cpp
    ManifestTests.cpp
    ManifestFetcherTests.cpp
    UpdatePlannerTests.cpp
    ApplyPlanTests.cpp
    HashTests.cpp
    ApplyExecutorTests.cpp
    StateStoreTests.cpp)

target_compile_features(libAutoUpdater_tests PRIVATE cxx_std_17)
target_link_libraries(libAutoUpdater_tests PRIVATE libAutoUpdater)
target_include_directories(libAutoUpdater_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(libAutoUpdater_tests PRIVATE ${CMAKE_SOURCE_DIR}/updater)

add_test(NAME libAutoUpdater_tests COMMAND libAutoUpdater_tests)
