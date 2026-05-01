qt_add_library(fc_sync STATIC
    SyncService.h SyncService.cpp
    OutboxWorker.h OutboxWorker.cpp
    PendingOpsWorker.h PendingOpsWorker.cpp
    DraftSync.h DraftSync.cpp
)
target_include_directories(fc_sync PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fc_sync
    PUBLIC  Qt6::Core
            fc_api fc_cache fc_models fc_util)
fc_set_warnings(fc_sync)
fc_apply_sanitizers(fc_sync)
