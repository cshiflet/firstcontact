find_package(Qt6 6.4 COMPONENTS Test REQUIRED)

function(fc_add_test name)
    qt_add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE
        Qt6::Test Qt6::Core Qt6::Gui
        fc_util fc_models fc_api fc_auth)
    fc_set_warnings(${name})
    fc_apply_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

fc_add_test(test_base64url      unit/test_base64url.cpp)
fc_add_test(test_rate_limiter   unit/test_rate_limiter.cpp)
fc_add_test(test_message_parser unit/test_message_parser.cpp)
fc_add_test(test_linkify        unit/test_linkify.cpp)
fc_add_test(test_html2text      unit/test_html2text.cpp)
