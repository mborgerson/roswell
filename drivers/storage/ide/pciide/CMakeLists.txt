
if(NOT ENABLE_UNIATA)
# the PCI IDE miniport is linked into the kernel image as a static
# library; ntoskrnl runs PciideDriverEntry via IoCreateDriver.  It calls the
# PciIdeX* ABI of the folded-in pciidex library directly.
add_library(pciide pciide.c)
target_compile_definitions(pciide PRIVATE DriverEntry=PciideDriverEntry)
add_dependencies(pciide psdk xdk bugcodes)
endif()
add_registry_inf(pciide_reg.inf)
