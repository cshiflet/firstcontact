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

option(FC_BUILD_TESTS      "Build unit and integration tests"                    ON)
option(FC_ENABLE_SANITIZERS "Enable ASan/UBSan in Debug builds"                  OFF)

include(CompilerWarnings)
include(Sanitizers)
include(Qt6Setup)

# libzstd: message-body compression with per-account trained dictionary.
# System package (apt: libzstd-dev / dnf: libzstd-devel / pacman: zstd /
# vcpkg: zstd). pkg-config is the most portable lookup; the IMPORTED
# target keeps target_link_libraries call sites clean across platforms.
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

add_subdirectory(src)

if(FC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(Packaging)
