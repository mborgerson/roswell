
list(APPEND SOURCE
    database.c
    device.c
    mountmgr.c
    notify.c
    point.c
    symlink.c
    uniqueid.c
    mntmgr.h)

# the mount manager is linked into the kernel image as a static
# library; ntoskrnl runs MountmgrDriverEntry via IoCreateDriver.
add_library(mountmgr ${SOURCE})
target_link_libraries(mountmgr wdmguid ioevent memcmp)
add_pch(mountmgr mntmgr.h SOURCE)
target_compile_definitions(mountmgr PRIVATE DriverEntry=MountmgrDriverEntry)
add_dependencies(mountmgr psdk xdk bugcodes)
