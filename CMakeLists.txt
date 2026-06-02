cmake_minimum_required(VERSION 3.16)

project(libAutoUpdater
    VERSION 0.1.2
    DESCRIPTION "C++17 cross-platform desktop online update library"
    LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(LIBAUTOUPDATER_BUILD_UPDATER "Build the external updater executable" ON)
option(LIBAUTOUPDATER_BUILD_EXAMPLES "Build examples" ON)
option(LIBAUTOUPDATER_BUILD_TESTS "Build tests" ON)
option(LIBAUTOUPDATER_WITH_CURL "Build libcurl network adapter when available" ON)
option(LIBAUTOUPDATER_REQUIRE_CURL "Fail configure if libcurl network adapter cannot be built" OFF)
option(LIBAUTOUPDATER_WITH_WINHTTP "Build native WinHTTP network adapter on Windows when libcurl is unavailable" ON)
option(LIBAUTOUPDATER_WITH_CFNETWORK "Build native CFNetwork adapter on Apple platforms when libcurl is unavailable" ON)
option(LIBAUTOUPDATER_WITH_OPENSSL "Build OpenSSL signature verifier when available" ON)
option(LIBAUTOUPDATER_WITH_QT "Build optional Qt example when Qt6 is available" OFF)
option(LIBAUTOUPDATER_ENABLE_WARNINGS "Enable common compiler warnings for project targets" ON)
option(LIBAUTOUPDATER_WARNINGS_AS_ERRORS "Treat project compiler warnings as errors" OFF)
option(LIBAUTOUPDATER_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer on supported compilers" OFF)

set(LIBAUTOUPDATER_CONFIG_REQUIRES_CURL FALSE)
set(LIBAUTOUPDATER_CONFIG_REQUIRES_OPENSSL FALSE)
set(LIBAUTOUPDATER_HAS_HTTP_BACKEND FALSE)

if(LIBAUTOUPDATER_REQUIRE_CURL AND NOT LIBAUTOUPDATER_WITH_CURL)
    message(FATAL_ERROR "LIBAUTOUPDATER_REQUIRE_CURL=ON requires LIBAUTOUPDATER_WITH_CURL=ON")
endif()

if(LIBAUTOUPDATER_ENABLE_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "LIBAUTOUPDATER_ENABLE_SANITIZERS is supported only on GCC/Clang-style compilers")
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

function(libautoupdater_apply_project_warnings target)
    if(NOT LIBAUTOUPDATER_ENABLE_WARNINGS)
        return()
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(LIBAUTOUPDATER_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(LIBAUTOUPDATER_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

set(LIBAUTOUPDATER_PUBLIC_HEADERS
    include/libAutoUpdater/ApplyPlan.h
    include/libAutoUpdater/Config.h
    include/libAutoUpdater/Error.h
    include/libAutoUpdater/Manifest.h
    include/libAutoUpdater/Result.h
    include/libAutoUpdater/Types.h
    include/libAutoUpdater/Updater.h
    include/libAutoUpdater/Version.h
    include/libAutoUpdater/interfaces/IEventDispatcher.h
    include/libAutoUpdater/interfaces/IFileSystem.h
    include/libAutoUpdater/interfaces/IHashProvider.h
    include/libAutoUpdater/interfaces/INetworkClient.h
    include/libAutoUpdater/interfaces/IProcessLauncher.h
    include/libAutoUpdater/interfaces/ISignatureVerifier.h
    include/libAutoUpdater/interfaces/IStateStore.h)

set(LIBAUTOUPDATER_SOURCES
    src/ApplyLauncher.cpp
    src/ApplyPlan.cpp
    src/ApplyPlanWriter.cpp
    src/DownloadExecutor.cpp
    src/Error.cpp
    src/LocalSnapshotBuilder.cpp
    src/Manifest.cpp
    src/ManifestFetcher.cpp
    src/ManifestVerifier.cpp
    src/UpdatePlanner.cpp
    src/Updater.cpp
    src/Version.cpp
    src/default/BasicNetworkClient.cpp
    src/default/DirectDispatcher.cpp
    src/default/JsonStateStore.cpp
    src/default/NullSignatureVerifier.cpp
    src/default/ProcessLauncher.cpp
    src/default/Sha256HashProvider.cpp
    src/default/StdFileSystem.cpp
    src/util/Json.cpp
    src/util/PathUtil.cpp
    src/util/Sha256.cpp
    src/util/UrlUtil.cpp)

add_library(libAutoUpdater ${LIBAUTOUPDATER_PUBLIC_HEADERS} ${LIBAUTOUPDATER_SOURCES})
add_library(libAutoUpdater::libAutoUpdater ALIAS libAutoUpdater)

libautoupdater_apply_project_warnings(libAutoUpdater)

target_compile_features(libAutoUpdater PUBLIC cxx_std_17)
target_include_directories(libAutoUpdater
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(libAutoUpdater
    PUBLIC
        LIBAUTOUPDATER_VERSION="${PROJECT_VERSION}")

if(WIN32)
    target_compile_definitions(libAutoUpdater PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(LIBAUTOUPDATER_WITH_CURL)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        target_sources(libAutoUpdater PRIVATE src/default/CurlNetworkClient.cpp)
        target_link_libraries(libAutoUpdater PRIVATE CURL::libcurl)
        target_compile_definitions(libAutoUpdater PRIVATE LIBAUTOUPDATER_HAS_CURL=1)
        set(LIBAUTOUPDATER_CONFIG_REQUIRES_CURL TRUE)
        set(LIBAUTOUPDATER_HAS_HTTP_BACKEND TRUE)
    elseif(LIBAUTOUPDATER_REQUIRE_CURL)
        message(FATAL_ERROR "LIBAUTOUPDATER_REQUIRE_CURL=ON but CMake could not find CURL")
    endif()
endif()

if(WIN32 AND LIBAUTOUPDATER_WITH_WINHTTP AND NOT LIBAUTOUPDATER_HAS_HTTP_BACKEND)
    target_sources(libAutoUpdater PRIVATE src/default/WinHttpNetworkClient.cpp)
    target_link_libraries(libAutoUpdater PRIVATE winhttp)
    target_compile_definitions(libAutoUpdater PRIVATE LIBAUTOUPDATER_HAS_WINHTTP=1)
    set(LIBAUTOUPDATER_HAS_HTTP_BACKEND TRUE)
endif()

if(APPLE AND LIBAUTOUPDATER_WITH_CFNETWORK AND NOT LIBAUTOUPDATER_HAS_HTTP_BACKEND)
    target_sources(libAutoUpdater PRIVATE src/default/CfNetworkClient.cpp)
    set_source_files_properties(src/default/CfNetworkClient.cpp PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-declarations")
    target_link_libraries(libAutoUpdater PRIVATE "-framework CFNetwork" "-framework CoreFoundation")
    target_compile_definitions(libAutoUpdater PRIVATE LIBAUTOUPDATER_HAS_CFNETWORK=1)
    set(LIBAUTOUPDATER_HAS_HTTP_BACKEND TRUE)
endif()

if(LIBAUTOUPDATER_WITH_OPENSSL)
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_sources(libAutoUpdater PRIVATE src/default/OpenSslSignatureVerifier.cpp)
        target_link_libraries(libAutoUpdater PRIVATE OpenSSL::Crypto)
        target_compile_definitions(libAutoUpdater PRIVATE LIBAUTOUPDATER_HAS_OPENSSL=1)
        set(LIBAUTOUPDATER_CONFIG_REQUIRES_OPENSSL TRUE)
    endif()
endif()

if(LIBAUTOUPDATER_BUILD_UPDATER)
    add_subdirectory(updater)
endif()

if(LIBAUTOUPDATER_BUILD_EXAMPLES)
    add_subdirectory(examples/cli)
    if(LIBAUTOUPDATER_WITH_QT)
        add_subdirectory(examples/qt)
    endif()
endif()

if(LIBAUTOUPDATER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS libAutoUpdater
    EXPORT libAutoUpdaterTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/libAutoUpdater
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT libAutoUpdaterTargets
    FILE libAutoUpdaterTargets.cmake
    NAMESPACE libAutoUpdater::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libAutoUpdater)

configure_package_config_file(
    cmake/libAutoUpdaterConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/libAutoUpdaterConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libAutoUpdater)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/libAutoUpdaterConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/libAutoUpdaterConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/libAutoUpdaterConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libAutoUpdater)
