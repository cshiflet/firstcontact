qt_add_library(fc_cache STATIC
    Database.h Database.cpp
    Migrations.h Migrations.cpp
    MessageRepository.h MessageRepository.cpp
    AttachmentRepository.h AttachmentRepository.cpp
    LabelRepository.h LabelRepository.cpp
    OutboxRepository.h OutboxRepository.cpp
    PendingOpsRepository.h PendingOpsRepository.cpp
    DraftRepository.h DraftRepository.cpp
    MetaRepository.h MetaRepository.cpp
)
qt_add_resources(fc_cache "fc_cache_schema"
    PREFIX "/schema"
    BASE   "${CMAKE_SOURCE_DIR}/resources/schema"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/schema/0001_init.sql"
        "${CMAKE_SOURCE_DIR}/resources/schema/0002_fts.sql"
        "${CMAKE_SOURCE_DIR}/resources/schema/0003_body_html.sql"
        "${CMAKE_SOURCE_DIR}/resources/schema/0004_send_at.sql"
        "${CMAKE_SOURCE_DIR}/resources/schema/0005_snooze.sql"
        "${CMAKE_SOURCE_DIR}/resources/schema/0006_multi_account.sql"
        "${CMAKE_SOURCE_DIR}/resources/schema/0007_drop_legacy_seed.sql"
)
target_include_directories(fc_cache PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fc_cache
    PUBLIC  Qt6::Core Qt6::Sql
            fc_util)
fc_set_warnings(fc_cache)
fc_apply_sanitizers(fc_cache)
