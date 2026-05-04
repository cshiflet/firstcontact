find_package(Qt6 6.4 COMPONENTS
    Core Gui Widgets Network NetworkAuth Sql Svg Concurrent LinguistTools
    REQUIRED)

if(FC_BUILD_TESTS)
    find_package(Qt6 6.4 COMPONENTS Test REQUIRED)
endif()

# Qt6::WebEngineWidgets used to be looked up here for the inline HTML
# preview path (FC_ENABLE_WEBENGINE option + src/ui/reader/webengine_plugin
# subdirectory). That path was dropped — see the "Drop inline WebEngine
# HTML preview" commit. To resurrect, revert that commit.

find_package(Qt6Keychain QUIET)
if(NOT Qt6Keychain_FOUND)
    find_package(Qt5Keychain QUIET)
endif()
if(NOT Qt6Keychain_FOUND AND NOT Qt5Keychain_FOUND)
    message(FATAL_ERROR
        "qtkeychain (Qt 6 build) not found. Install qtkeychain-qt6-dev (apt) or qtkeychain-qt6 (vcpkg).")
endif()

qt_standard_project_setup()
