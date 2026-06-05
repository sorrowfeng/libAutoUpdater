add_executable(autoupdater_apply
    main.cpp
    ApplyExecutor.cpp)

target_compile_features(autoupdater_apply PRIVATE cxx_std_17)
target_link_libraries(autoupdater_apply PRIVATE libAutoUpdater)
target_include_directories(autoupdater_apply PRIVATE ${libAutoUpdater_SOURCE_DIR}/src)
libautoupdater_apply_project_warnings(autoupdater_apply)

if(WIN32)
    target_compile_definitions(autoupdater_apply PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(autoupdater_apply PRIVATE shell32)
    # 以 GUI 子系统构建，避免运行时弹出控制台黑窗口；
    # 仍保留标准 int main 入口（MSVC 用 mainCRTStartup，MinGW 用 -mwindows）。
    set_target_properties(autoupdater_apply PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_link_options(autoupdater_apply PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()

add_executable(libAutoUpdater::autoupdater_apply ALIAS autoupdater_apply)

install(TARGETS autoupdater_apply
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
