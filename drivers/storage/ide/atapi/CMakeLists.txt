
include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/sptilib)

list(APPEND SOURCE
    atapi.c
    atapi.h
    debug.h
    dev_config.c
    dev_error.c
    dev_identify.c
    dev_power.c
    dev_timings.c
    enum.c
    fdo.c
    ioctl.c
    pdo.c
    portstate.c
    satl.c
    scsi.c
    scsiex.h
    smart.c
    wmi.c)

if(SARCH STREQUAL "xbox")
    # No WMI provider surface on Xbox; the IRP_MJ_SYSTEM_CONTROL dispatcher
    # is guarded out in atapi.c so wmi.c's only entry point is unreachable.
    list(REMOVE_ITEM SOURCE wmi.c)
    # dev_power.c stays in -- satl.c needs AtaDeviceGetFlushCacheCommand
    # at runtime.  With -ffunction-sections the unused power helpers GC
    # individually.
endif()

# the ATA/ATAPI driver is linked into the kernel image as a static
# library; ntoskrnl runs AtapiDriverEntry via IoCreateDriver.  wmilib and
# sptilib are folded in alongside.  The kernel already links gcc_ssp_nt, so
# atapi does not pull its own copy.
add_library(atapi ${SOURCE})
target_link_libraries(atapi memcmp sptilib ${PSEH_LIB})
add_pch(atapi atapi.h SOURCE)
target_compile_definitions(atapi PRIVATE DriverEntry=AtapiDriverEntry)
add_dependencies(atapi psdk xdk bugcodes)
add_registry_inf(atapi_reg.inf)
