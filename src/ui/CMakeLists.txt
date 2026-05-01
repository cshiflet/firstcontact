qt_add_library(fc_ui STATIC
    MainWindow.h MainWindow.cpp
    setup/SetupWizard.h setup/SetupWizard.cpp
    sidebar/SidebarWidget.h sidebar/SidebarWidget.cpp
    messagelist/MessageItemDelegate.h messagelist/MessageItemDelegate.cpp
    messagelist/MessageListView.h messagelist/MessageListView.cpp
    reader/ReaderPane.h reader/ReaderPane.cpp
    reader/IHtmlRenderHost.h
    reader/HtmlRenderHostLoader.h reader/HtmlRenderHostLoader.cpp
    compose/ComposeWindow.h compose/ComposeWindow.cpp
    tray/Notifier.h tray/Notifier.cpp
    tray/TrayController.h tray/TrayController.cpp
    common/Shortcuts.h common/Shortcuts.cpp
)
target_include_directories(fc_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fc_ui
    PUBLIC  Qt6::Widgets Qt6::Gui Qt6::Core
            fc_models fc_util fc_auth fc_api fc_cache fc_sync)
fc_set_warnings(fc_ui)
fc_apply_sanitizers(fc_ui)
