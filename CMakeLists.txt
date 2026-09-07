cmake_minimum_required(VERSION 3.25)

set(GLZDB_VERSION "0.1.0")

project(glzdb VERSION ${GLZDB_VERSION} LANGUAGES CXX)

option(GLZDB_BUILD_TEST "Enable tests" ON)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(
    $<$<CXX_COMPILER_ID:GNU>:-Wall>
    $<$<CXX_COMPILER_ID:GNU>:-Wextra>
    $<$<CXX_COMPILER_ID:GNU>:-pedantic>
    $<$<CXX_COMPILER_ID:Clang>:-Wall>
    $<$<CXX_COMPILER_ID:Clang>:-Wextra>
    $<$<CXX_COMPILER_ID:Clang>:-pedantic>
    $<$<CXX_COMPILER_ID:AppleClang>:-Wall>
    $<$<CXX_COMPILER_ID:AppleClang>:-Wextra>
    $<$<CXX_COMPILER_ID:AppleClang>:-pedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
  # wasi-sdk の sysroot には C++ 例外ランタイムが無いため無効化する。
  # throw は abort になる。glzdb 自体は例外を投げず std::expected で返す。
  add_compile_options(-fno-exceptions)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(glzdb INTERFACE)
add_library(glzdb::glzdb ALIAS glzdb)
target_compile_features(glzdb INTERFACE cxx_std_23)

find_package(glaze CONFIG REQUIRED)
target_link_libraries(glzdb INTERFACE glaze::glaze)

target_include_directories(glzdb
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(TARGETS glzdb
    EXPORT glzdbTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/glzdb.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY
    ${CMAKE_CURRENT_SOURCE_DIR}/include/glzdb
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT glzdbTargets
    FILE glzdbTargets.cmake
    NAMESPACE glzdb::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/glzdb
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/glzdbConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/glzdbConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/glzdb
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/glzdbConfigVersion.cmake
    VERSION ${GLZDB_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/glzdbConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/glzdbConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/glzdb
)

if(GLZDB_BUILD_TEST)
    enable_testing()
    add_subdirectory(test)
    add_subdirectory(example)
endif()
