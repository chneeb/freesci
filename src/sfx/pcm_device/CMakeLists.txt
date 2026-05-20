set(pcm_sources audiobuf.c pcm_devices.c)

if(PLATFORM STREQUAL "desktop")
    list(APPEND pcm_sources sdl.c)
endif()

add_library(scipcm STATIC ${pcm_sources})

if(PLATFORM STREQUAL "desktop")
    target_compile_options(scipcm PRIVATE ${SDL2_CFLAGS_OTHER})
    target_include_directories(scipcm PRIVATE ${SDL2_INCLUDE_DIRS})
    target_compile_definitions(scipcm PRIVATE HAVE_SDL=1)
endif()
