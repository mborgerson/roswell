
list(APPEND SOURCE
    blockdev.c
    cleanup.c
    close.c
    create.c
    dir.c
    direntry.c
    dirwr.c
    ea.c
    fat.c
    fastio.c
    fcb.c
    finfo.c
    flush.c
    fsctl.c
    iface.c
    kdbg.c
    misc.c
    pnp.c
    rw.c
    string.c
    volume.c
    vfat.h)

# Nothing sends IRP_MJ_SHUTDOWN on Xbox; reboot drives the SMC directly.
if(NOT SARCH STREQUAL "xbox")
    list(APPEND SOURCE shutdown.c)
endif()

if(KDBG)
    add_definitions(-DKDBG)
endif()

# the FAT/FATX filesystem is linked into the kernel image as a static
# library; ntoskrnl runs VfatfsDriverEntry via IoCreateDriver(\FileSystem\Vfatfs)
# and it registers itself with IoRegisterFileSystem.  vfatfs already carries a
# complete FATX (Xbox HDD filesystem) read/write implementation -- folding it
# in is what gives the kernel FATX.  Built unchanged, in the normal driver
# view -- kernel-API imports resolve through the in-kernel import table.
add_library(vfatfs ${SOURCE})
target_link_libraries(vfatfs ${PSEH_LIB})
target_compile_definitions(vfatfs PRIVATE DriverEntry=VfatfsDriverEntry)
add_dependencies(vfatfs psdk xdk bugcodes)
add_registry_inf(vfatfs_reg.inf)
