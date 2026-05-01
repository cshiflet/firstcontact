add_library(fc_auth STATIC
    ClientConfig.h ClientConfig.cpp
    TokenStore.h TokenStore.cpp
    OAuthClient.h OAuthClient.cpp
)
target_include_directories(fc_auth PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fc_auth
    PUBLIC  Qt6::Core Qt6::Network Qt6::NetworkAuth Qt6::Gui
            fc_util)

# qtkeychain may install as Qt6::Keychain (vcpkg) or qt6keychain (apt). Try both.
if(TARGET Qt6Keychain::Qt6Keychain)
    target_link_libraries(fc_auth PUBLIC Qt6Keychain::Qt6Keychain)
elseif(TARGET qt6keychain)
    target_link_libraries(fc_auth PUBLIC qt6keychain)
else()
    target_link_libraries(fc_auth PUBLIC qt6keychain)
endif()

fc_set_warnings(fc_auth)
fc_apply_sanitizers(fc_auth)
