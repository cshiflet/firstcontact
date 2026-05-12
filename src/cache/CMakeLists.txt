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
    BodyCompressionWorker.h BodyCompressionWorker.cpp
)
qt_add_resources(fc_cache "fc_cache_schema"
    PREFIX "/schema"
    BASE   "${CMAKE_SOURCE_DIR}/resources/schema"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/schema/schema.sql"
)
target_include_directories(fc_cache PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fc_cache
    PUBLIC  Qt6::Core Qt6::Sql
            fc_util)
fc_set_warnings(fc_cache)
fc_apply_sanitizers(fc_cache)
