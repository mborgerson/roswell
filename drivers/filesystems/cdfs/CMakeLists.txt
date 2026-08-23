
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/drivers)

list(APPEND SOURCE
    allocsup.c
    cachesup.c
    cddata.c
    cdinit.c
    cdprocssrc.c
    cleanup.c
    close.c
    create.c
    devctrl.c
    deviosup.c
    dirctrl.c
    dirsup.c
    fileinfo.c
    filobsup.c
    fsctrl.c
    fspdisp.c
    lockctrl.c
    namesup.c
    pathsup.c
    pnp.c
    prefxsup.c
    read.c
    resrcsup.c
    shutdown.c
    strucsup.c
    verfysup.c
    volinfo.c
    workque.c
    write.c)

# the CDFS filesystem is linked into the kernel image as a static
# library; ntoskrnl runs CdfsDriverEntry via IoCreateDriver(\FileSystem\Cdfs)
# and it registers itself with IoRegisterFileSystem.  Built unchanged, in the
# normal driver view -- kernel-API imports resolve through the in-kernel
# import table.
add_library(cdfs ${SOURCE})
target_link_libraries(cdfs ${PSEH_LIB} memcmp)
target_compile_definitions(cdfs PRIVATE DriverEntry=CdfsDriverEntry)
add_dependencies(cdfs psdk xdk bugcodes)
add_registry_inf(cdfs_reg.inf)
