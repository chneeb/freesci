add_library(scigfxresources STATIC
    sci_font.c
    sci_resmgr.c
    sci_pic_0.c
    sci_view_0.c
    sci_cursor_0.c
    sci_pal_1.c
    sci_view_1.c
)

if(PLATFORM STREQUAL "pico")
    target_link_libraries(scigfxresources PRIVATE scipicoplatform)
endif()
