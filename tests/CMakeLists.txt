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
    StateStoreTests.cpp
    UpdaterTests.cpp)

target_compile_features(libAutoUpdater_tests PRIVATE cxx_std_17)
target_link_libraries(libAutoUpdater_tests PRIVATE libAutoUpdater)
target_include_directories(libAutoUpdater_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(libAutoUpdater_tests PRIVATE ${CMAKE_SOURCE_DIR}/updater)
libautoupdater_apply_project_warnings(libAutoUpdater_tests)

if(LIBAUTOUPDATER_CONFIG_REQUIRES_OPENSSL)
    target_compile_definitions(libAutoUpdater_tests PRIVATE LIBAUTOUPDATER_TEST_HAS_OPENSSL=1)
endif()

add_test(NAME libAutoUpdater_tests COMMAND libAutoUpdater_tests)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND TARGET libAutoUpdater_cli AND TARGET autoupdater_apply)
    add_test(
        NAME libAutoUpdater_e2e_flow
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/e2e_flow.py
            --cli $<TARGET_FILE:libAutoUpdater_cli>
            --updater $<TARGET_FILE:autoupdater_apply>
            --make-manifest ${CMAKE_SOURCE_DIR}/tools/make_manifest.py
            --work-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e-flow)
endif()
