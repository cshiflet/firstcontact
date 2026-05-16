# Bundled-dict trainer. Built only when -DBUILD_BUNDLED_DICT_TRAINER=ON;
# normal app and test builds skip this whole subtree. Linked against
# the same libzstd the app uses so the dictionary format is guaranteed
# compatible with BodyCodec at runtime.

add_executable(train_bundled_dict
    train_bundled_dict.cpp
)

# zstd's dictionary builder API lives in libzstd; the top-level CMakeLists
# imports a cross-platform fc_zstd interface target.
target_link_libraries(train_bundled_dict PRIVATE fc_zstd)

set_target_properties(train_bundled_dict PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # No Qt in this tool — opt out of the top-level AUTOMOC/AUTOUIC/AUTORCC
    # so we don't run moc on a Qt-free CLI.
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
