qt_add_library(fc_ui STATIC
    MainWindow.h MainWindow.cpp
    setup/SetupWizard.h setup/SetupWizard.cpp
    sidebar/SidebarWidget.h sidebar/SidebarWidget.cpp
    messagelist/MessageItemDelegate.h messagelist/MessageItemDelegate.cpp
    messagelist/MessageListView.h messagelist/MessageListView.cpp
    reader/ReaderPane.h reader/ReaderPane.cpp
    reader/IHtmlRenderHost.h
    reader/HtmlRenderHostLoader.h reader/HtmlRenderHostLoader.cpp
    reader/LocalHtmlServer.h reader/LocalHtmlServer.cpp
    compose/ComposeWindow.h compose/ComposeWindow.cpp
    tray/Notifier.h tray/Notifier.cpp
    tray/TrayController.h tray/TrayController.cpp
    common/Shortcuts.h common/Shortcuts.cpp
    common/Theme.h common/Theme.cpp
    common/SettingsDialog.h common/SettingsDialog.cpp
    common/IconLoader.h common/IconLoader.cpp
    common/Preferences.h common/Preferences.cpp
)
qt_add_resources(fc_ui "fc_ui_assets"
    PREFIX "/"
    BASE   "${CMAKE_SOURCE_DIR}/resources"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/themes/light.qss"
        "${CMAKE_SOURCE_DIR}/resources/themes/dark.qss"
        "${CMAKE_SOURCE_DIR}/resources/icons/symbolic/compose.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/symbolic/refresh.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/symbolic/reply.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/symbolic/reply-all.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/symbolic/forward.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/symbolic/search.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/symbolic/settings.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/symbolic/login.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/symbolic/logout.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/symbolic/quit.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/symbolic/star.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/symbolic/star-filled.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/symbolic/paperclip.svg"
)
target_include_directories(fc_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fc_ui
    PUBLIC  Qt6::Widgets Qt6::Gui Qt6::Core Qt6::Svg Qt6::Network
            fc_models fc_util fc_auth fc_api fc_cache fc_sync)
fc_set_warnings(fc_ui)
fc_apply_sanitizers(fc_ui)
