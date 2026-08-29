
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
    shutdown.c
    string.c
    volume.c
    vfat.h)

if(KDBG)
    add_definitions(-DKDBG)
endif()

# the FAT/FATX filesystem is linked into the kernel image as a static
# library; ntoskrnl runs VfatfsDriverEntry via IoCreateDriver(\FileSystem\Vfatfs)
# and it registers itself with IoRegisterFileSystem.  vfatfs already carries a
# complete FATX (Xbox HDD filesystem) read/write implementation -- folding it
# in is what gives the kernel FATX.  Built unchanged, in the normal driver
# view -- kernel-API imports resolve through the in-kernel import table.
# Write-through durability: flush a FATX operation's data + metadata to the
# platter before it returns, matching retail (which survives an abrupt SMC
# power-off).  On by default; turn OFF to relax power-loss durability for
# throughput.  Only meaningful on the Xbox FATX build.
option(NXK_FATX_WRITE_THROUGH "FATX write-through durability (retail parity)" ON)

add_library(vfatfs ${SOURCE})
target_link_libraries(vfatfs ${PSEH_LIB})
target_compile_definitions(vfatfs PRIVATE DriverEntry=VfatfsDriverEntry)
if(NXK_FATX_WRITE_THROUGH)
    target_compile_definitions(vfatfs PRIVATE NXK_FATX_WRITE_THROUGH=1)
endif()
add_dependencies(vfatfs psdk xdk bugcodes)
add_registry_inf(vfatfs_reg.inf)
