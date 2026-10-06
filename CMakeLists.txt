cmake_minimum_required(VERSION 3.20)
project(rpi_highspeed_camera VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
set_target_properties(hscam_raw PROPERTIES EXPORT_NAME raw)
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

find_package(Git QUIET)
set(HSCAM_SOURCE_REVISION "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --verify HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE HSCAM_SOURCE_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=normal
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE HSCAM_GIT_DIRTY
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(HSCAM_GIT_DIRTY)
        string(APPEND HSCAM_SOURCE_REVISION "-dirty")
    endif()
endif()

add_executable(hscam-qualify tools/qualification/main.cpp)
target_link_libraries(hscam-qualify PRIVATE hscam::hscam hscam::raw)
target_include_directories(hscam-qualify PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(hscam-qualify PRIVATE HSCAM_SOURCE_REVISION="${HSCAM_SOURCE_REVISION}")
target_compile_options(hscam-qualify PRIVATE -Wall -Wextra -Wpedantic)


# Installable v0.1 library/tooling package.
set(HSCAM_CONFIG_REQUIRES_LIBCAMERA ${LIBCAMERA_FOUND})
configure_package_config_file(
    cmake/hscamConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/hscamConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hscam
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/hscamConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
    TARGETS hscam hscam_raw
    EXPORT hscamTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(
    TARGETS hscam-inspect hscam-capture hscam-export hscam-qualify
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)
install(
    EXPORT hscamTargets
    FILE hscamTargets.cmake
    NAMESPACE hscam::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hscam
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/hscamConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/hscamConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hscam
)

if(HSCAM_BUILD_TESTS)
    enable_testing()
    add_executable(hscam-unit-tests tests/unit/main.cpp)
    target_link_libraries(hscam-unit-tests PRIVATE hscam::hscam hscam::raw)
    target_compile_options(hscam-unit-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hscam-unit-tests COMMAND hscam-unit-tests)
endif()
