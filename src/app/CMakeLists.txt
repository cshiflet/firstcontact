qt_add_executable(firstcontact
    main.cpp
    Application.h Application.cpp
    Bootstrap.h Bootstrap.cpp
    Cli.h Cli.cpp
)

target_link_libraries(firstcontact
    PRIVATE Qt6::Widgets Qt6::Gui Qt6::Core
            fc_util fc_models fc_auth fc_api fc_cache fc_account fc_sync fc_ui)

set_target_properties(firstcontact PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE    ON)

install(TARGETS firstcontact
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION bin)

if(UNIX AND NOT APPLE)
    install(FILES   ${CMAKE_SOURCE_DIR}/packaging/linux/firstcontact.desktop
            DESTINATION share/applications)
    install(FILES   ${CMAKE_SOURCE_DIR}/packaging/linux/org.firstcontact.FirstContact.metainfo.xml
            DESTINATION share/metainfo)
    install(FILES   ${CMAKE_SOURCE_DIR}/resources/icons/firstcontact.png
            DESTINATION share/icons/hicolor/256x256/apps)
    install(FILES   ${CMAKE_SOURCE_DIR}/resources/icons/firstcontact.svg
            DESTINATION share/icons/hicolor/scalable/apps)
endif()

fc_set_warnings(firstcontact)
fc_apply_sanitizers(firstcontact)
