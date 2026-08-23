
list(APPEND SOURCE
    partition.c
    partmgr.c
    utils.c)

list(APPEND PCH_SKIP_SOURCE
    guid.c)

# the partition manager is linked into the kernel image as a static
# library; ntoskrnl runs PartmgrDriverEntry via IoCreateDriver.
add_library(partmgr ${SOURCE} ${PCH_SKIP_SOURCE})

add_pch(partmgr partmgr.h "${PCH_SKIP_SOURCE}")
target_compile_definitions(partmgr PRIVATE DriverEntry=PartmgrDriverEntry)
add_dependencies(partmgr psdk xdk bugcodes)
add_registry_inf(partmgr_reg.inf)
