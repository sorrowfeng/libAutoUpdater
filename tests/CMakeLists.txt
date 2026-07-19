add_executable(libAutoUpdater_tests
    ${libAutoUpdater_SOURCE_DIR}/updater/ApplyJournal.cpp
    ${libAutoUpdater_SOURCE_DIR}/updater/ApplyExecutor.cpp
    main.cpp
    VersionTests.cpp
    ManifestTests.cpp
    ManifestFetcherTests.cpp
    UrlPolicyTests.cpp
    NetworkRequestTests.cpp
    ResourceLimitTests.cpp
    UpdatePlannerTests.cpp
    ApplyPlanTests.cpp
    HashTests.cpp
    SignatureVerifierTests.cpp
    ApplyExecutorTests.cpp
    ApplyCrashRecoveryTests.cpp
    RootedFileSystemTests.cpp
    StateStoreTests.cpp
    UpdaterTests.cpp
    FuzzSmokeTests.cpp)

target_compile_features(libAutoUpdater_tests PRIVATE cxx_std_17)
target_link_libraries(libAutoUpdater_tests PRIVATE libAutoUpdater)
target_include_directories(libAutoUpdater_tests PRIVATE ${libAutoUpdater_SOURCE_DIR}/src)
target_include_directories(libAutoUpdater_tests PRIVATE ${libAutoUpdater_SOURCE_DIR}/updater)
libautoupdater_apply_project_warnings(libAutoUpdater_tests)

add_executable(libAutoUpdater_apply_crash_helper
    apply_crash_helper.cpp
    ${libAutoUpdater_SOURCE_DIR}/updater/ApplyExecutor.cpp
    ${libAutoUpdater_SOURCE_DIR}/updater/ApplyJournal.cpp)
target_compile_features(libAutoUpdater_apply_crash_helper PRIVATE cxx_std_17)
target_link_libraries(libAutoUpdater_apply_crash_helper PRIVATE libAutoUpdater)
target_include_directories(libAutoUpdater_apply_crash_helper PRIVATE ${libAutoUpdater_SOURCE_DIR}/src)
target_include_directories(libAutoUpdater_apply_crash_helper PRIVATE ${libAutoUpdater_SOURCE_DIR}/updater)
libautoupdater_apply_project_warnings(libAutoUpdater_apply_crash_helper)
add_dependencies(libAutoUpdater_tests libAutoUpdater_apply_crash_helper)

if(WIN32)
    target_compile_definitions(libAutoUpdater_apply_crash_helper PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(LIBAUTOUPDATER_CONFIG_REQUIRES_OPENSSL)
    target_compile_definitions(libAutoUpdater_tests PRIVATE LIBAUTOUPDATER_TEST_HAS_OPENSSL=1)
endif()

add_test(NAME libAutoUpdater_tests COMMAND libAutoUpdater_tests)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND TARGET libAutoUpdater_cli AND TARGET autoupdater_apply)
    add_test(
        NAME libAutoUpdater_apply_plan_handoff
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/apply_plan_handoff.py
            --updater $<TARGET_FILE:autoupdater_apply>
            --work-dir ${CMAKE_CURRENT_BINARY_DIR}/apply-plan-handoff)
    add_test(
        NAME libAutoUpdater_e2e_flow
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/e2e_flow.py
            --cli $<TARGET_FILE:libAutoUpdater_cli>
            --updater $<TARGET_FILE:autoupdater_apply>
            --make-manifest ${libAutoUpdater_SOURCE_DIR}/tools/make_manifest.py
            --work-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e-flow)
    add_test(
        NAME libAutoUpdater_e2e_flow_unicode_paths
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/e2e_flow.py
            --cli $<TARGET_FILE:libAutoUpdater_cli>
            --updater $<TARGET_FILE:autoupdater_apply>
            --make-manifest ${libAutoUpdater_SOURCE_DIR}/tools/make_manifest.py
            --work-dir ${CMAKE_CURRENT_BINARY_DIR}/e2e-flow-中文路径)
endif()
