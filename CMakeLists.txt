cmake_minimum_required(VERSION 3.16)

project(AmiiboZero VERSION 0.2.3 LANGUAGES CXX)

add_library(bin_catalog STATIC src/bin_catalog.cpp src/nfc_flow.cpp)
target_compile_features(bin_catalog PUBLIC cxx_std_17)
target_include_directories(bin_catalog PUBLIC src)
set_target_properties(bin_catalog PROPERTIES CXX_EXTENSIONS OFF)

add_executable(amiibozero src/main.cpp)
target_link_libraries(amiibozero PRIVATE bin_catalog)
set_target_properties(amiibozero PROPERTIES CXX_EXTENSIONS OFF)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME startup_smoke COMMAND amiibozero)
    add_executable(input_test tests/input_test.cpp)
    target_compile_features(input_test PRIVATE cxx_std_17)
    target_include_directories(input_test PRIVATE src)
    add_test(NAME device_input COMMAND input_test)
    add_executable(bin_catalog_test tests/bin_catalog_test.cpp)
    target_link_libraries(bin_catalog_test PRIVATE bin_catalog)
    add_test(NAME bin_catalog COMMAND bin_catalog_test)
endif()

option(AMIIBOZERO_CARDPUTER "Build the Cardputer Zero LVGL application" OFF)
if(AMIIBOZERO_CARDPUTER)
    include(cmake/cardputer.cmake)
endif()
