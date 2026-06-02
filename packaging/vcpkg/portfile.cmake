vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO sorrowfeng/libAutoUpdater
    REF v${VERSION}
    SHA512 0
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DLIBAUTOUPDATER_BUILD_EXAMPLES=OFF
        -DLIBAUTOUPDATER_BUILD_TESTS=OFF
        -DLIBAUTOUPDATER_BUILD_UPDATER=ON
        -DLIBAUTOUPDATER_WITH_QT=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/libAutoUpdater)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
