add_library(fc_models STATIC
    Message.h
    MessageListModel.h MessageListModel.cpp
    LabelTreeModel.h   LabelTreeModel.cpp
)
target_include_directories(fc_models PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fc_models PUBLIC Qt6::Core fc_cache)
fc_set_warnings(fc_models)
fc_apply_sanitizers(fc_models)
