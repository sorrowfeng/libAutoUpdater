cmake_minimum_required(VERSION 3.16)

foreach(required_variable IN ITEMS
        PROJECT_BINARY_DIR
        CONSUMER_SOURCE_DIR
        TEST_ROOT
        INSTALL_BINDIR
        INSTALL_LIBDIR
        GENERATOR)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

set(install_prefix "${TEST_ROOT}/prefix")
set(consumer_binary_dir "${TEST_ROOT}/consumer-build")
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")

set(install_command
    "${CMAKE_COMMAND}"
    --install "${PROJECT_BINARY_DIR}"
    --prefix "${install_prefix}")
if(DEFINED INSTALL_CONFIG AND NOT "${INSTALL_CONFIG}" STREQUAL "")
    list(APPEND install_command --config "${INSTALL_CONFIG}")
endif()

execute_process(
    COMMAND ${install_command}
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "Installing libAutoUpdater failed (${install_result}).\n"
        "stdout:\n${install_stdout}\n"
        "stderr:\n${install_stderr}")
endif()

set(package_dir "${install_prefix}/${INSTALL_LIBDIR}/cmake/libAutoUpdater")
set(configure_command
    "${CMAKE_COMMAND}"
    -S "${CONSUMER_SOURCE_DIR}"
    -B "${consumer_binary_dir}"
    -G "${GENERATOR}"
    "-DlibAutoUpdater_DIR=${package_dir}")
if(DEFINED GENERATOR_PLATFORM AND NOT "${GENERATOR_PLATFORM}" STREQUAL "")
    list(APPEND configure_command -A "${GENERATOR_PLATFORM}")
endif()
if(DEFINED GENERATOR_TOOLSET AND NOT "${GENERATOR_TOOLSET}" STREQUAL "")
    list(APPEND configure_command -T "${GENERATOR_TOOLSET}")
endif()
if(DEFINED GENERATOR_INSTANCE AND NOT "${GENERATOR_INSTANCE}" STREQUAL "")
    list(APPEND configure_command
        "-DCMAKE_GENERATOR_INSTANCE=${GENERATOR_INSTANCE}")
endif()
if(DEFINED TOOLCHAIN_FILE AND NOT "${TOOLCHAIN_FILE}" STREQUAL "")
    list(APPEND configure_command "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}")
endif()
if(DEFINED INSTALL_CONFIG AND NOT "${INSTALL_CONFIG}" STREQUAL "")
    list(APPEND configure_command "-DCMAKE_BUILD_TYPE=${INSTALL_CONFIG}")
endif()
if(DEFINED CONSUMER_CXX_FLAGS AND NOT "${CONSUMER_CXX_FLAGS}" STREQUAL "")
    list(APPEND configure_command "-DCMAKE_CXX_FLAGS=${CONSUMER_CXX_FLAGS}")
endif()
if(DEFINED CONSUMER_LINK_FLAGS AND NOT "${CONSUMER_LINK_FLAGS}" STREQUAL "")
    list(APPEND configure_command "-DCMAKE_EXE_LINKER_FLAGS=${CONSUMER_LINK_FLAGS}")
endif()

execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Configuring the clean package consumer failed (${configure_result}).\n"
        "stdout:\n${configure_stdout}\n"
        "stderr:\n${configure_stderr}")
endif()

set(build_command "${CMAKE_COMMAND}" --build "${consumer_binary_dir}")
if(DEFINED INSTALL_CONFIG AND NOT "${INSTALL_CONFIG}" STREQUAL "")
    list(APPEND build_command --config "${INSTALL_CONFIG}")
endif()

execute_process(
    COMMAND ${build_command}
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "Building the clean package consumer failed (${build_result}).\n"
        "stdout:\n${build_stdout}\n"
        "stderr:\n${build_stderr}")
endif()

# Installed shared libraries must be discoverable while probing the clean
# consumer. This also verifies the documented deployment layout on Windows.
if(WIN32)
    set(original_path "$ENV{PATH}")
    set(ENV{PATH} "${install_prefix}/${INSTALL_BINDIR};$ENV{PATH}")
elseif(APPLE)
    set(original_dyld_library_path "$ENV{DYLD_LIBRARY_PATH}")
    set(ENV{DYLD_LIBRARY_PATH}
        "${install_prefix}/${INSTALL_LIBDIR}:$ENV{DYLD_LIBRARY_PATH}")
elseif(UNIX)
    set(original_ld_library_path "$ENV{LD_LIBRARY_PATH}")
    set(ENV{LD_LIBRARY_PATH}
        "${install_prefix}/${INSTALL_LIBDIR}:$ENV{LD_LIBRARY_PATH}")
endif()

set(ctest_command "${CMAKE_CTEST_COMMAND}" --output-on-failure)
if(DEFINED INSTALL_CONFIG AND NOT "${INSTALL_CONFIG}" STREQUAL "")
    list(APPEND ctest_command -C "${INSTALL_CONFIG}")
endif()

execute_process(
    COMMAND ${ctest_command}
    WORKING_DIRECTORY "${consumer_binary_dir}"
    RESULT_VARIABLE consumer_test_result
    OUTPUT_VARIABLE consumer_test_stdout
    ERROR_VARIABLE consumer_test_stderr)

# The helper probe must rely on its own install RPATH on Unix. Restore the
# caller's loader environment before starting it so the test cannot mask a
# broken relative search path.
if(WIN32)
    set(ENV{PATH} "${original_path}")
elseif(APPLE)
    set(ENV{DYLD_LIBRARY_PATH} "${original_dyld_library_path}")
elseif(UNIX)
    set(ENV{LD_LIBRARY_PATH} "${original_ld_library_path}")
endif()

if(NOT consumer_test_result EQUAL 0)
    message(FATAL_ERROR
        "Running the clean package consumer failed (${consumer_test_result}).\n"
        "stdout:\n${consumer_test_stdout}\n"
        "stderr:\n${consumer_test_stderr}")
endif()

set(installed_helper
    "${install_prefix}/${INSTALL_BINDIR}/autoupdater_apply${EXECUTABLE_SUFFIX}")
if(NOT EXISTS "${installed_helper}")
    message(FATAL_ERROR "The installed updater helper was not found: ${installed_helper}")
endif()

execute_process(
    COMMAND "${installed_helper}" --help
    RESULT_VARIABLE helper_result
    OUTPUT_VARIABLE helper_stdout
    ERROR_VARIABLE helper_stderr)
if(NOT helper_result EQUAL 2)
    message(FATAL_ERROR
        "The installed updater helper did not return its argument-validation status (2); "
        "got ${helper_result}.\nstdout:\n${helper_stdout}\nstderr:\n${helper_stderr}")
endif()

set(installed_update_variables
    PYTHON_EXECUTABLE
    INSTALLED_UPDATE_SCRIPT
    INSTALLED_UPDATE_FIXTURE)
set(installed_update_variable_count 0)
foreach(variable IN LISTS installed_update_variables)
    if(DEFINED ${variable} AND NOT "${${variable}}" STREQUAL "")
        math(EXPR installed_update_variable_count "${installed_update_variable_count} + 1")
    endif()
endforeach()

if(installed_update_variable_count GREATER 0)
    if(NOT installed_update_variable_count EQUAL 3)
        message(FATAL_ERROR "Installed update validation requires all Python fixture variables")
    endif()
endif()

if(installed_update_variable_count EQUAL 3)
    foreach(path_variable IN ITEMS PYTHON_EXECUTABLE INSTALLED_UPDATE_SCRIPT INSTALLED_UPDATE_FIXTURE)
        if(NOT EXISTS "${${path_variable}}")
            message(FATAL_ERROR "${path_variable} does not exist: ${${path_variable}}")
        endif()
    endforeach()
    execute_process(
        COMMAND
            "${PYTHON_EXECUTABLE}"
            "${INSTALLED_UPDATE_SCRIPT}"
            --updater "${installed_helper}"
            --fixture "${INSTALLED_UPDATE_FIXTURE}"
            --work-dir "${TEST_ROOT}/installed-update-rollback"
        RESULT_VARIABLE installed_update_result
        OUTPUT_VARIABLE installed_update_stdout
        ERROR_VARIABLE installed_update_stderr)
    if(NOT installed_update_result EQUAL 0)
        message(FATAL_ERROR
            "Installed updater apply/rollback validation failed (${installed_update_result}).\n"
            "stdout:\n${installed_update_stdout}\n"
            "stderr:\n${installed_update_stderr}")
    endif()
endif()
