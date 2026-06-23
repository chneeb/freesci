set(SCISOFTSEQ_SRCS
    softsequencers.c
    pcspeaker.c
    SN76496.c
    fmopl.c
    opl2.c
    amiga.c
    fluidsynth.c
)

# Flash-resident OPL2 tables (generated, ~64KB of const .rodata). Only compiled
# when PICO_PWM_AUDIO is ON; fmopl.c references them under FMOPL_FLASH_TABLES.
if(PICO_PWM_AUDIO)
    list(APPEND SCISOFTSEQ_SRCS fmopl_tables.c)
endif()

add_library(scisoftseq STATIC ${SCISOFTSEQ_SRCS})
