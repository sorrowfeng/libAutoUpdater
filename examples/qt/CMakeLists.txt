find_package(Qt6 6.0 COMPONENTS Core Network QUIET)

if(NOT Qt6_FOUND)
    message(STATUS "Qt6 Core/Network not found; skipping Qt example")
    return()
endif()

add_executable(libAutoUpdater_qt
    main.cpp
    QtDispatcher.cpp
    QtNetworkClient.cpp)

target_compile_features(libAutoUpdater_qt PRIVATE cxx_std_17)
set_target_properties(libAutoUpdater_qt PROPERTIES AUTOMOC ON)
target_link_libraries(libAutoUpdater_qt
    PRIVATE
        libAutoUpdater
        Qt6::Core
        Qt6::Network)
target_include_directories(libAutoUpdater_qt PRIVATE ${libAutoUpdater_SOURCE_DIR}/src)
libautoupdater_apply_project_warnings(libAutoUpdater_qt)

if(LIBAUTOUPDATER_BUILD_TESTS)
    add_executable(libAutoUpdater_qt_network_tests
        QtNetworkClientTests.cpp
        QtNetworkClient.cpp)
    target_compile_features(libAutoUpdater_qt_network_tests PRIVATE cxx_std_17)
    set_target_properties(libAutoUpdater_qt_network_tests PROPERTIES AUTOMOC ON)
    target_link_libraries(libAutoUpdater_qt_network_tests
        PRIVATE
            libAutoUpdater
            Qt6::Core
            Qt6::Network)
    target_include_directories(libAutoUpdater_qt_network_tests PRIVATE ${libAutoUpdater_SOURCE_DIR}/src)
    libautoupdater_apply_project_warnings(libAutoUpdater_qt_network_tests)
    add_test(NAME libAutoUpdater_qt_network_contract COMMAND libAutoUpdater_qt_network_tests)
    set_tests_properties(libAutoUpdater_qt_network_contract PROPERTIES TIMEOUT 15)
endif()
