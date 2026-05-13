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
# Public-corpus-trained zstd dictionary used as the per-account
# fallback in MessageRepository::dictionaryFor. See
# tools/bundled-dict/README.md for how the artifact is produced.
qt_add_resources(fc_cache "fc_cache_bundled_dict"
    PREFIX "/compression"
    BASE   "${CMAKE_SOURCE_DIR}/resources/compression"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/compression/bundled-bodies-v1.zdict"
)
target_include_directories(fc_cache PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fc_cache
    PUBLIC  Qt6::Core Qt6::Sql
            fc_util)
fc_set_warnings(fc_cache)
fc_apply_sanitizers(fc_cache)
