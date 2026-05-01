find_package(Qt6 6.4 COMPONENTS
    Core Gui Widgets Network NetworkAuth Sql Concurrent LinguistTools
    REQUIRED)

if(FC_BUILD_TESTS)
    find_package(Qt6 6.4 COMPONENTS Test REQUIRED)
endif()

if(FC_ENABLE_WEBENGINE)
    find_package(Qt6 6.4 COMPONENTS WebEngineWidgets QUIET)
    if(NOT Qt6WebEngineWidgets_FOUND)
        message(WARNING "QtWebEngineWidgets not found; HTML-on-demand renderer will be disabled at runtime.")
        set(FC_ENABLE_WEBENGINE OFF CACHE BOOL "" FORCE)
    endif()
endif()

find_package(Qt6Keychain QUIET)
if(NOT Qt6Keychain_FOUND)
    find_package(Qt5Keychain QUIET)
endif()
if(NOT Qt6Keychain_FOUND AND NOT Qt5Keychain_FOUND)
    message(FATAL_ERROR
        "qtkeychain (Qt 6 build) not found. Install qtkeychain-qt6-dev (apt) or qtkeychain-qt6 (vcpkg).")
endif()

qt_standard_project_setup()
