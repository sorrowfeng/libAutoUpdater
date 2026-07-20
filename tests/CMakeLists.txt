add_executable(libAutoUpdater_tests
    ${libAutoUpdater_SOURCE_DIR}/updater/ApplyJournal.cpp
    ${libAutoUpdater_SOURCE_DIR}/updater/ApplyExecutor.cpp
    main.cpp
    ErrorTests.cpp
    VersionTests.cpp
    ManifestTests.cpp
    ManifestFetcherTests.cpp
    UrlPolicyTests.cpp
    NetworkRequestTests.cpp
    ResourceLimitTests.cpp
    UpdatePlannerTests.cpp
    ApplyPlanTests.cpp
    HashTests.cpp
    FileSystemTests.cpp
    SignatureVerifierTests.cpp
    ApplyExecutorTests.cpp
    ApplyCrashRecoveryTests.cpp
    ProcessLauncherTests.cpp
    RootedFileSystemTests.cpp
    StateStoreTests.cpp
    UpdaterTests.cpp
    JsonContractTests.cpp
    Rfc3339Tests.cpp
    FuzzSmokeTests.cpp
    SecurityBoundaryRegressionTests.cpp)

target_compile_features(libAutoUpdater_tests PRIVATE cxx_std_17)
target_link_libraries(libAutoUpdater_tests PRIVATE libAutoUpdater)
target_include_directories(libAutoUpdater_tests PRIVATE ${libAutoUpdater_SOURCE_DIR}/src)
target_include_directories(libAutoUpdater_tests PRIVATE ${libAutoUpdater_SOURCE_DIR}/updater)
target_compile_definitions(libAutoUpdater_tests PRIVATE
    LIBAUTOUPDATER_JSON_CORPUS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/json_conformance_corpus.txt"
    LIBAUTOUPDATER_RFC3339_CORPUS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/rfc3339_conformance_corpus.txt")
libautoupdater_apply_project_warnings(libAutoUpdater_tests)

add_executable(libAutoUpdater_process_launcher_helper process_launcher_helper.cpp)
target_compile_features(libAutoUpdater_process_launcher_helper PRIVATE cxx_std_17)
libautoupdater_apply_project_warnings(libAutoUpdater_process_launcher_helper)
set_target_properties(libAutoUpdater_process_launcher_helper PROPERTIES
    OUTPUT_NAME "libAutoUpdater process launcher helper")
add_dependencies(libAutoUpdater_tests libAutoUpdater_process_launcher_helper)

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
    target_compile_definitions(libAutoUpdater_process_launcher_helper PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(libAutoUpdater_process_launcher_helper PRIVATE shell32)
endif()

if(LIBAUTOUPDATER_CONFIG_REQUIRES_OPENSSL)
    target_compile_definitions(libAutoUpdater_tests PRIVATE LIBAUTOUPDATER_TEST_HAS_OPENSSL=1)
endif()

add_test(NAME libAutoUpdater_tests COMMAND libAutoUpdater_tests)

if(TARGET autoupdater_apply)
    set(libautoupdater_install_package_test_args
        -DPROJECT_BINARY_DIR=${libAutoUpdater_BINARY_DIR}
        -DCONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/consumer
        -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/install-package-$<CONFIG>
        -DINSTALL_CONFIG=$<CONFIG>
        -DINSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}
        -DINSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}
        -DEXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}
        -DGENERATOR=${CMAKE_GENERATOR})

    if(CMAKE_GENERATOR_PLATFORM)
        list(APPEND libautoupdater_install_package_test_args
            -DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM})
    endif()
    if(CMAKE_GENERATOR_TOOLSET)
        list(APPEND libautoupdater_install_package_test_args
            -DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET})
    endif()
    if(CMAKE_GENERATOR_INSTANCE)
        list(APPEND libautoupdater_install_package_test_args
            -DGENERATOR_INSTANCE=${CMAKE_GENERATOR_INSTANCE})
    endif()
    if(CMAKE_TOOLCHAIN_FILE)
        list(APPEND libautoupdater_install_package_test_args
            -DTOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
    endif()

    add_test(
        NAME libAutoUpdater_install_package_validation
        COMMAND
            ${CMAKE_COMMAND}
            ${libautoupdater_install_package_test_args}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/install_package_test.cmake)
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME libAutoUpdater_gc_objects_validation
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/gc_objects_tests.py
            --gc ${libAutoUpdater_SOURCE_DIR}/tools/gc_objects.py
            --json-corpus ${CMAKE_CURRENT_SOURCE_DIR}/json_conformance_corpus.txt
            --rfc3339-corpus ${CMAKE_CURRENT_SOURCE_DIR}/rfc3339_conformance_corpus.txt
            --work-dir ${CMAKE_CURRENT_BINARY_DIR}/gc-objects-validation)
    add_test(
        NAME libAutoUpdater_metadata_tools_validation
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/metadata_tool_tests.py
            --make-manifest ${libAutoUpdater_SOURCE_DIR}/tools/make_manifest.py
            --make-index ${libAutoUpdater_SOURCE_DIR}/tools/make_index.py
            --sign-manifest ${libAutoUpdater_SOURCE_DIR}/tools/sign_manifest.py
            --work-dir ${CMAKE_CURRENT_BINARY_DIR}/metadata-tools-validation)
    add_test(
        NAME libAutoUpdater_sbom_tool_validation
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/sbom_tool_tests.py
            --make-sbom ${libAutoUpdater_SOURCE_DIR}/tools/make_sbom.py
            --configured-dependencies ${libAutoUpdater_BINARY_DIR}/libAutoUpdater-sbom-dependencies.json
            --work-dir ${CMAKE_CURRENT_BINARY_DIR}/sbom-tool-validation)
endif()

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

if(Python3_Interpreter_FOUND AND TARGET autoupdater_apply)
    add_test(
        NAME libAutoUpdater_updater_argument_validation
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/updater_argument_validation.py
            --updater $<TARGET_FILE:autoupdater_apply>
            --work-dir ${CMAKE_CURRENT_BINARY_DIR}/updater-argument-validation)
endif()
