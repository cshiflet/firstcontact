qt_add_library(fc_account STATIC
    AccountContext.h AccountContext.cpp
    AccountManager.h AccountManager.cpp
)
target_include_directories(fc_account PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fc_account
    PUBLIC  Qt6::Core Qt6::Sql
            fc_cache fc_auth fc_api fc_sync fc_util)
fc_set_warnings(fc_account)
fc_apply_sanitizers(fc_account)
