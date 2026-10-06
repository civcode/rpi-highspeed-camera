cmake_minimum_required(VERSION 3.20)
project(rpi_highspeed_camera VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HSCAM_BUILD_TESTS "Build host unit tests" ON)
option(HSCAM_REQUIRE_LIBCAMERA "Fail configuration when libcamera is unavailable" OFF)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBCAMERA QUIET IMPORTED_TARGET libcamera)
endif()

if(HSCAM_REQUIRE_LIBCAMERA AND NOT LIBCAMERA_FOUND)
    message(FATAL_ERROR "libcamera development files are required")
endif()

add_library(hscam
    src/context.cpp
    src/camera.cpp
    src/types.cpp
    src/media/sensor_subdevice.cpp
    src/capture_format/bundle.cpp
)
add_library(hscam::hscam ALIAS hscam)

target_include_directories(hscam
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(hscam PUBLIC cxx_std_20)
target_compile_options(hscam PRIVATE -Wall -Wextra -Wpedantic)

if(LIBCAMERA_FOUND)
    target_sources(hscam PRIVATE src/libcamera_inventory.cpp)
    target_link_libraries(hscam PRIVATE PkgConfig::LIBCAMERA)
    target_compile_definitions(hscam PRIVATE HSCAM_HAS_LIBCAMERA=1)
else()
    message(WARNING "libcamera not found: camera discovery/capture support is disabled in this build")
endif()

add_library(hscam_raw
    src/raw/render.cpp
)
add_library(hscam::raw ALIAS hscam_raw)
target_include_directories(hscam_raw PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(hscam_raw PUBLIC cxx_std_20)
target_compile_options(hscam_raw PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(hscam_raw PUBLIC hscam::hscam)

find_package(TIFF QUIET)
add_executable(hscam-export tools/export/main.cpp)
target_link_libraries(hscam-export PRIVATE hscam::hscam hscam::raw)
target_compile_options(hscam-export PRIVATE -Wall -Wextra -Wpedantic)
if(TIFF_FOUND)
    target_link_libraries(hscam-export PRIVATE TIFF::TIFF)
    target_compile_definitions(hscam-export PRIVATE HSCAM_HAS_TIFF=1)
else()
    message(WARNING "libtiff not found: DNG export is disabled; inspect/image/video remain available")
endif()

add_executable(hscam-inspect tools/inspect/main.cpp)
target_link_libraries(hscam-inspect PRIVATE hscam::hscam)
target_compile_options(hscam-inspect PRIVATE -Wall -Wextra -Wpedantic)

add_executable(hscam-capture tools/capture/main.cpp)
target_link_libraries(hscam-capture PRIVATE hscam::hscam)
target_compile_options(hscam-capture PRIVATE -Wall -Wextra -Wpedantic)

if(HSCAM_BUILD_TESTS)
    enable_testing()
    add_executable(hscam-unit-tests tests/unit/main.cpp)
    target_link_libraries(hscam-unit-tests PRIVATE hscam::hscam hscam::raw)
    target_compile_options(hscam-unit-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hscam-unit-tests COMMAND hscam-unit-tests)
endif()
