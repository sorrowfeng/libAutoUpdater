add_executable(libAutoUpdater_tests
    ${CMAKE_SOURCE_DIR}/updater/ApplyExecutor.cpp
    main.cpp
    VersionTests.cpp
    ManifestTests.cpp
    ManifestFetcherTests.cpp
    UpdatePlannerTests.cpp
    ApplyPlanTests.cpp
    HashTests.cpp
    SignatureVerifierTests.cpp
    ApplyExecutorTests.cpp
    StateStoreTests.cpp)

target_compile_features(libAutoUpdater_tests PRIVATE cxx_std_17)
target_link_libraries(libAutoUpdater_tests PRIVATE libAutoUpdater)
target_include_directories(libAutoUpdater_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(libAutoUpdater_tests PRIVATE ${CMAKE_SOURCE_DIR}/updater)

if(LIBAUTOUPDATER_CONFIG_REQUIRES_OPENSSL)
    target_compile_definitions(libAutoUpdater_tests PRIVATE LIBAUTOUPDATER_TEST_HAS_OPENSSL=1)
endif()

add_test(NAME libAutoUpdater_tests COMMAND libAutoUpdater_tests)
