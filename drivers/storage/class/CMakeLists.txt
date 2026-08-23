# the stock cdrom driver is KMDF (32 KLOC, pulls in wdf01000/wdfldr);
# nxcdrom is a minimal classpnp-based replacement folded into the kernel.
add_subdirectory(nxcdrom)
add_subdirectory(classpnp)
add_subdirectory(disk)
if(NOT SARCH STREQUAL "xbox")
    add_subdirectory(ramdisk)
endif()
#add_subdirectory(sfloppy)
