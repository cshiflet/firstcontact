add_library(fc_ui STATIC
    MainWindow.h MainWindow.cpp
    setup/SetupWizard.h setup/SetupWizard.cpp
    messagelist/MessageItemDelegate.h messagelist/MessageItemDelegate.cpp
    messagelist/MessageListView.h messagelist/MessageListView.cpp
    reader/ReaderPane.h reader/ReaderPane.cpp
)
target_include_directories(fc_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fc_ui
    PUBLIC  Qt6::Widgets Qt6::Gui Qt6::Core
            fc_models fc_util fc_auth fc_api)
fc_set_warnings(fc_ui)
fc_apply_sanitizers(fc_ui)
