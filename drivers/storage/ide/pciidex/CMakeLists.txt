
list(APPEND SOURCE
    chipset/ahci_generic.c
    chipset/ahci_hw.c
    chipset/ahci_io.c
    chipset/amd.c
    chipset/ati.c
    chipset/cmd.c
    chipset/intel.c
    chipset/pata_generic.c
    chipset/pata_hw.c
    chipset/pata_io.c
    chipset/pctech.c
    chipset/sil680.c
    chipset/svw_pata.c
    chipset/svw_sata.c
    chipset/toshiba.c
    chipset/via.c
    acpi.c
    ahci.h
    debug.h
    fdo.c
    miniport.c
    pciidex.c
    pciidex.h
    pdo.c
    power.c)

if(ARCH STREQUAL "i386" AND NOT SARCH STREQUAL "xbox")
    # Xbox has only the PnP-enumerated NVIDIA controller -- no legacy
    # non-PnP ATA detection. Dropping the define unanchors the legacy
    # function-pointer registration in pciidex.c.
    add_definitions(-DATA_DETECT_LEGACY_DEVICES)
    list(APPEND SOURCE chipset/pata_legacy.c)
endif()

if(SARCH STREQUAL "xbox")
    # MCPX southbridge is a fixed VEN_NVIDIA PATA controller -- AHCI/SATA and
    # every other vendor-specific chipset is dead.  Drop them so the linker
    # can GC ~30 KB of unused detection + transfer-mode code.  amd.c stays
    # because the NVIDIA case in PciIdeGetControllerProperties dispatches to
    # AmdGetControllerProperties.
    list(REMOVE_ITEM SOURCE
        chipset/ahci_generic.c
        chipset/ahci_hw.c
        chipset/ahci_io.c
        chipset/ati.c
        chipset/cmd.c
        chipset/intel.c
        chipset/pctech.c
        chipset/sil680.c
        chipset/svw_pata.c
        chipset/svw_sata.c
        chipset/toshiba.c)
    # via.c stays in because amd.c uses its ViaClampTimings helper for the
    # NVIDIA chipset timing path.  Most of the file is dead but the helper is
    # cheap to keep.
endif()

# the PCI IDE bus-extension library is linked into the kernel image
# as a static library -- it exports the PciIdeX* ABI the folded-in pciide
# miniport calls directly.  Built unchanged, in the normal driver view; its
# kernel-API imports resolve through the in-kernel import table.
add_library(pciidex ${SOURCE})
add_pch(pciidex pciidex.h SOURCE)
target_compile_definitions(pciidex PRIVATE DriverEntry=PciidexDriverEntry)
add_dependencies(pciidex psdk xdk bugcodes)
