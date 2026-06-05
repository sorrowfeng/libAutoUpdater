add_executable(libAutoUpdater_cli main.cpp)
target_compile_features(libAutoUpdater_cli PRIVATE cxx_std_17)
target_link_libraries(libAutoUpdater_cli PRIVATE libAutoUpdater)
target_include_directories(libAutoUpdater_cli PRIVATE ${libAutoUpdater_SOURCE_DIR}/src)
libautoupdater_apply_project_warnings(libAutoUpdater_cli)

if(WIN32)
    target_link_libraries(libAutoUpdater_cli PRIVATE shell32)
endif()
