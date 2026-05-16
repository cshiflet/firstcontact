cmake_minimum_required(VERSION 3.24)
project(FirstContact
    VERSION 0.1.0
    DESCRIPTION "Native Gmail client built on Qt 6"
    HOMEPAGE_URL "https://github.com/cshiflet/firstcontact"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

option(FC_BUILD_TESTS              "Build unit and integration tests"            ON)
option(FC_ENABLE_SANITIZERS        "Enable ASan/UBSan in Debug builds"           OFF)
option(BUILD_BUNDLED_DICT_TRAINER  "Build tools/bundled-dict/train_bundled_dict" OFF)

include(CompilerWarnings)
include(Sanitizers)
include(Qt6Setup)

# libzstd: message-body compression with per-account trained dictionary.
# Prefer CMake config packages (vcpkg/Windows, some Homebrew installs), then
# fall back to pkg-config (Linux distro packages, Homebrew).
find_package(zstd CONFIG QUIET)
add_library(fc_zstd INTERFACE)
if(TARGET zstd::libzstd_shared)
    target_link_libraries(fc_zstd INTERFACE zstd::libzstd_shared)
elseif(TARGET zstd::libzstd_static)
    target_link_libraries(fc_zstd INTERFACE zstd::libzstd_static)
elseif(TARGET zstd::libzstd)
    target_link_libraries(fc_zstd INTERFACE zstd::libzstd)
elseif(TARGET unofficial::zstd::libzstd)
    target_link_libraries(fc_zstd INTERFACE unofficial::zstd::libzstd)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
    target_link_libraries(fc_zstd INTERFACE PkgConfig::ZSTD)
endif()

add_subdirectory(src)

if(FC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BUILD_BUNDLED_DICT_TRAINER)
    add_subdirectory(tools/bundled-dict)
endif()

include(Packaging)
