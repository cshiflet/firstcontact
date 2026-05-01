add_library(fc_util STATIC
    Paths.h Paths.cpp
    Base64Url.h Base64Url.cpp
    Logger.h Logger.cpp
    Linkify.h Linkify.cpp
    Html2Text.h Html2Text.cpp
    HtmlSanitizer.h HtmlSanitizer.cpp
    MimeBuilder.h MimeBuilder.cpp
    Browser.h Browser.cpp
)
target_include_directories(fc_util PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fc_util PUBLIC Qt6::Core Qt6::Gui)
fc_set_warnings(fc_util)
fc_apply_sanitizers(fc_util)
