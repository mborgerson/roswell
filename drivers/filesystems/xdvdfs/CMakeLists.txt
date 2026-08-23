
list(APPEND SOURCE
    xdvdfs.c)

# read-only XDVDFS (Xbox optical) FSD folded into the kernel image as
# a static library; ntoskrnl runs XdvdfsDriverEntry via IoCreateDriver and the
# driver registers itself with IoRegisterFileSystem.  Registered AFTER stock
# cdfs so it wins the CdRom FS queue head (InsertHeadList -- last-registered
# is tried first for IRP_MN_MOUNT_VOLUME).
add_library(xdvdfs ${SOURCE})
target_compile_definitions(xdvdfs PRIVATE DriverEntry=XdvdfsDriverEntry)
add_dependencies(xdvdfs psdk xdk bugcodes)
