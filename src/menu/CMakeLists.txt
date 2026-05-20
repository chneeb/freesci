add_library(scimenu STATIC game_select_init.c game_select_screen.c)

if(PLATFORM STREQUAL "desktop")
    target_compile_options(scimenu PRIVATE ${SDL2_CFLAGS_OTHER})
    target_include_directories(scimenu PRIVATE ${SDL2_INCLUDE_DIRS})
endif()
