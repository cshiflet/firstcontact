add_library(fc_api STATIC
    Errors.h
    RateLimiter.h RateLimiter.cpp
    RestClient.h RestClient.cpp
    MessageParser.h MessageParser.cpp
    GmailClient.h GmailClient.cpp
    SessionTransfer.h SessionTransfer.cpp
)
target_include_directories(fc_api PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fc_api
    PUBLIC  Qt6::Core Qt6::Network
            fc_util fc_auth fc_models)
fc_set_warnings(fc_api)
fc_apply_sanitizers(fc_api)
