# Platform support library for PicoCalc (RP2350 + ILI9488 + I2C keyboard).
# All third-party code is vendored under src/platform/pico/:
#   fatfs/   — FatFS + SD SPI driver
#   hw/      — i2ckbd + lcdspi (PicoCalc hardware drivers)

set(PSRAM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/psram/psram_spi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/psram_alloc.c
)

set(FATFS_SOURCES
    ${FATFS_DIR}/ff15/source/ff.c
    ${FATFS_DIR}/ff15/source/ffunicode.c
    ${FATFS_DIR}/ff15/source/ffsystem.c
    ${FATFS_DIR}/sd_driver/sd_spi.c
    ${FATFS_DIR}/sd_driver/spi.c
    ${FATFS_DIR}/sd_driver/sd_card.c
    ${FATFS_DIR}/sd_driver/crc.c
    ${FATFS_DIR}/sd_driver/demo_logging.c
    ${FATFS_DIR}/src/glue.c
    ${FATFS_DIR}/src/f_util.c
    ${FATFS_DIR}/src/my_debug.c
)

set(HW_SOURCES
    ${PICOCALC_HW_DIR}/i2ckbd/i2ckbd.c
    ${PICOCALC_HW_DIR}/lcdspi/lcdspi.c
)

add_library(scipicoplatform STATIC
    pico_main.c
    pico_time.c
    pico_io.c
    pico_sdcard.c
    kbd_input.c
    hw_config.c
    audio/pwm_synth.c
    ${PSRAM_SOURCES}
    ${FATFS_SOURCES}
    ${HW_SOURCES}
)

pico_generate_pio_header(scipicoplatform
    ${CMAKE_CURRENT_SOURCE_DIR}/psram/psram_spi.pio
)

target_compile_definitions(scipicoplatform PUBLIC
    HAVE_PICO=1
    PSRAM_PIN_CS=20
    PSRAM_PIN_SCK=21
    PSRAM_PIN_MOSI=2
    PSRAM_PIN_MISO=3
    PSRAM_MUTEX=1
    PICO_DEBUG_MALLOC=1
    PICO_DEBUG_MALLOC_LOW_WATER=0x20080000
)

target_include_directories(scipicoplatform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}           # pico_sdcard.h, kbd_input.h, psram_alloc.h, etc.
    ${CMAKE_CURRENT_SOURCE_DIR}/psram     # psram_spi.h
    ${CMAKE_CURRENT_SOURCE_DIR}/audio     # pwm_synth.h
    ${PICOCALC_HW_DIR}/i2ckbd             # i2ckbd.h
    ${PICOCALC_HW_DIR}/lcdspi             # lcdspi.h
    ${FATFS_DIR}/ff15/source              # ff.h
    ${FATFS_DIR}/sd_driver                # sd_card.h, hw_config.h, spi.h
    ${FATFS_DIR}/include                  # f_util.h, my_debug.h
    ${CMAKE_SOURCE_DIR}/src/include       # FreeSCI headers
    ${CMAKE_BINARY_DIR}                   # generated config.h
)

target_link_libraries(scipicoplatform PUBLIC
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_spi
    hardware_i2c
    hardware_pwm
    hardware_timer
    hardware_dma
    hardware_irq
    hardware_pio
)
