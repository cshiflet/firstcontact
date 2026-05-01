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

option(FC_ENABLE_WEBENGINE "Build the optional QtWebEngine HTML renderer plugin" ON)
option(FC_BUILD_TESTS      "Build unit and integration tests"                    ON)
option(FC_ENABLE_SANITIZERS "Enable ASan/UBSan in Debug builds"                  OFF)

include(CompilerWarnings)
include(Sanitizers)
include(Qt6Setup)

add_subdirectory(src)

if(FC_ENABLE_WEBENGINE)
    add_subdirectory(src/ui/reader/webengine_plugin)
endif()

if(FC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(Packaging)
