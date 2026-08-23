
remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    nxcdrom.c)

# the optical (CD/DVD) class driver is linked into the kernel image
# as a static library; ntoskrnl runs CdromDriverEntry via IoCreateDriver.
# It replaces the 32 KLOC KMDF cdrom driver and removes the KMDF dependency.
add_library(nxcdrom ${SOURCE})

# PRIVATE (not PUBLIC): these must not leak into ntoskrnl's own compilation
# now that the driver is linked into the kernel image.  NTDDI_VERSION must
# match classpnp's so the shared CLASS_* / FUNCTIONAL_DEVICE_EXTENSION
# layouts agree.
target_compile_definitions(nxcdrom PRIVATE
    DEBUG_USE_KDPRINT
    _WIN32_WINNT=0x602
    NTDDI_VERSION=0x06020000 # NTDDI_WIN8
    DriverEntry=CdromDriverEntry)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(nxcdrom PRIVATE -Wno-format -Wno-pointer-sign)
endif()

target_link_libraries(nxcdrom ntoskrnl_vista libcntpr)
add_dependencies(nxcdrom psdk xdk bugcodes)
add_registry_inf(nxcdrom_reg.inf)
