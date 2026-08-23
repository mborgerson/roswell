
remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    data.c
    disk.c
    geometry.c
    pnp.c
    disk.h)

if(NOT SARCH STREQUAL "xbox")
    # Xbox: no WMI surface, no SMART IOCTLs, no failure-prediction reporting
    # -- the whole file is dead. The single external caller in disk.c
    # (DiskInitializeReregistration) is gated.
    list(APPEND SOURCE diskwmi.c)
endif()

# the disk class driver is linked into the kernel image as a static
# library; ntoskrnl runs DiskDriverEntry via IoCreateDriver.
add_library(disk ${SOURCE})

# PRIVATE (not PUBLIC): must not leak into ntoskrnl's own compilation.
target_compile_definitions(disk PRIVATE
    DEBUG_USE_KDPRINT
    _WIN32_WINNT=0x602
    NTDDI_VERSION=0x06020000 # NTDDI_WIN8
    DriverEntry=DiskDriverEntry
    # StorDebugPrint is a non-static definition in both disk and
    # classpnp -- rename disk's copy so the two do not collide once both are
    # folded into the kernel image.
    StorDebugPrint=DiskStorDebugPrint)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(disk PRIVATE -Wno-format -Wno-pointer-sign)
    target_compile_options(disk PRIVATE -Wno-pointer-to-int-cast -Wno-switch)
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(disk PRIVATE -Wno-unused-but-set-variable)
endif()

target_link_libraries(disk ntoskrnl_vista libcntpr wdmguid)
add_dependencies(disk psdk xdk bugcodes)
add_registry_inf(disk_reg.inf)
