find_package(Qt6 6.4 COMPONENTS WebEngineWidgets REQUIRED)

qt_add_library(firstcontact_html_renderer SHARED
    Plugin.cpp
)
target_include_directories(firstcontact_html_renderer PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(firstcontact_html_renderer
    PRIVATE Qt6::Widgets Qt6::WebEngineWidgets)
fc_set_warnings(firstcontact_html_renderer)
fc_apply_sanitizers(firstcontact_html_renderer)

# Installed alongside the binary (Linux: /usr/lib, Windows: bin/, macOS: bundle Frameworks).
install(TARGETS firstcontact_html_renderer
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    BUNDLE  DESTINATION Frameworks)
