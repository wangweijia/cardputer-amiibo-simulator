cmake_minimum_required(VERSION 3.28)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    message(FATAL_ERROR "The Cardputer Zero application requires a Linux ARM64 toolchain.")
endif()

enable_language(C)
find_package(Freetype REQUIRED)
find_package(Threads REQUIRED)
include(FetchContent)
set(LV_BUILD_CONF_PATH "${PROJECT_SOURCE_DIR}/src/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(lvgl
    URL https://codeload.github.com/lvgl/lvgl/tar.gz/refs/tags/v9.5.0
    URL_HASH SHA256=34a955cdf3a2d005507b704e87357af669a114523b6d3f77b5344fdc68717bc6
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(lvgl)
target_link_libraries(lvgl PRIVATE Freetype::Freetype)

add_executable(amiibozero_device src/device_main.cpp)
set_target_properties(amiibozero_device PROPERTIES OUTPUT_NAME amiibozero
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/device" CXX_EXTENSIONS OFF)
target_compile_features(amiibozero_device PRIVATE cxx_std_17)
add_library(cap_listener STATIC src/cap_listener.cpp)
target_compile_features(cap_listener PUBLIC cxx_std_17)
target_link_libraries(cap_listener PUBLIC Threads::Threads amiibo_crypto)
target_link_libraries(amiibozero_device PRIVATE lvgl::lvgl bin_catalog cap_listener)

add_executable(cap_listen_probe EXCLUDE_FROM_ALL src/cap_listen_probe.cpp)
target_link_libraries(cap_listen_probe PRIVATE cap_listener)
add_executable(cap_cycle_check EXCLUDE_FROM_ALL tests/cap_cycle_check.cpp)
target_link_libraries(cap_cycle_check PRIVATE cap_listener bin_catalog)
if(BUILD_TESTING)
    add_executable(device_ui_test tests/device_ui_test.cpp)
    target_link_libraries(device_ui_test PRIVATE lvgl::lvgl bin_catalog cap_listener)
    add_test(NAME device_ui COMMAND device_ui_test)
endif()

install(TARGETS amiibozero_device RUNTIME DESTINATION bin)
install(FILES assets/applications/amiibozero.desktop DESTINATION share/APPLaunch/applications)
install(FILES assets/images/amiibozero.png DESTINATION share/APPLaunch/share/images)
install(FILES "${lvgl_SOURCE_DIR}/LICENCE.txt" DESTINATION share/doc/amiibozero)
install(FILES third_party/amiitool/LICENSE DESTINATION share/doc/amiibozero RENAME amiitool-LICENSE)
install(FILES hardware/setup-cardputer.sh hardware/cap-power.sh
    hardware/amiibozero-cap-power.service DESTINATION share/amiibozero/hardware)

set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_PACKAGE_NAME amiibozero)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "AmiiboZero for Cardputer Zero")
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "AmiiboZero contributors")
set(CPACK_DEBIAN_PACKAGE_SECTION APPLaunch)
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE arm64)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
