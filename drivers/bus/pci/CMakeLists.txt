
include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/drivers)

list(APPEND SOURCE
    fdo.c
    pci.c
    pdo.c
    pci.h)

# the PCI bus driver is linked into the kernel image as a static
# library, not a loadable pci.sys.  ntoskrnl's IopInitializeBootDrivers runs
# its DriverEntry via IoCreateDriver.  _NTSYSTEM_ makes its kernel-API
# references direct; DriverEntry is renamed so it does not collide with the
# other folded-in drivers.
add_library(pci ${SOURCE})
add_pch(pci pci.h SOURCE)
target_compile_definitions(pci PRIVATE _NTSYSTEM_ DriverEntry=PciDriverEntry)
add_dependencies(pci psdk xdk bugcodes)
