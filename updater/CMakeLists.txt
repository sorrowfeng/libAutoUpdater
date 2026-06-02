add_executable(autoupdater_apply
    main.cpp
    ApplyExecutor.cpp)

target_compile_features(autoupdater_apply PRIVATE cxx_std_17)
target_link_libraries(autoupdater_apply PRIVATE libAutoUpdater)
target_include_directories(autoupdater_apply PRIVATE ${libAutoUpdater_SOURCE_DIR}/src)
libautoupdater_apply_project_warnings(autoupdater_apply)

if(WIN32)
    target_compile_definitions(autoupdater_apply PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_executable(libAutoUpdater::autoupdater_apply ALIAS autoupdater_apply)

install(TARGETS autoupdater_apply
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
