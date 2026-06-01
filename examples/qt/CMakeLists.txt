find_package(Qt6 COMPONENTS Core Network QUIET)

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
