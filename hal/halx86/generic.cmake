
list(APPEND HAL_GENERIC_SOURCE
    generic/cmos.c
    generic/display.c
    generic/dma.c
    generic/halinit.c
    generic/misc.c
    generic/nmi.c
    generic/pic.c
    generic/reboot.c
    generic/sysinfo.c
    generic/usage.c
    generic/x86bios.c)

if(NOT SARCH STREQUAL "xbox")
    # beep.c, drive.c, kdpci.c, memory.c -- no live symbols on Xbox.
    # PC speaker beep, BIOS-disk DRIVE_LAYOUT, KD-over-PCI debug, BIOS
    # memory map -- none reachable from Xbox kernel.
    list(APPEND HAL_GENERIC_SOURCE
        generic/beep.c
        generic/drive.c
        generic/kdpci.c
        generic/memory.c)
endif()

if(ARCH STREQUAL "i386")
    list(APPEND HAL_GENERIC_SOURCE
        generic/bios.c
        generic/portio.c)

    list(APPEND HAL_GENERIC_ASM_SOURCE
        generic/v86.S)
endif()

add_asm_files(lib_hal_generic_asm ${HAL_GENERIC_ASM_SOURCE})
add_library(lib_hal_generic OBJECT ${HAL_GENERIC_SOURCE} ${lib_hal_generic_asm})
add_dependencies(lib_hal_generic asm)
