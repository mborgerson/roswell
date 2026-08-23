/*
 * Stub the embedded pci_vendors.ids / pci_classes.ids databases.
 *
 * ReactOS bin2c-embeds the entire pci.ids file (~1.5 MB of human-readable
 * vendor/device strings) into the kernel just so HalpDebugPciDumpBus()'s
 * boot-time bus walk can print pretty names like "NVIDIA Corporation" next
 * to PCI BDFs.  HalpDebugPciDumpBus already falls back to "Unknown" when
 * strstr() returns NULL on these tables (see hal/halx86/legacy/bussupp.c),
 * so replacing the tables with empty strings keeps the kernel functional
 * and just costs us cosmetic device names in the debug log.
 *
 * The kernel won't fit in the single-ROM flash budget with the full pci.ids
 * in INITDATA, so we wire this stub into xbox.cmake instead of pci_vendors.c
 * / pci_classes.c.
 *
 * The DATA_SEG("INITDATA") attribute matches the generated files so this TU
 * lands in the same section bin2c would have produced; HAL's INITDATA gets
 * freed after MmInitializeBootMemory the same way.
 */
#include <hal.h>

DATA_SEG("INITDATA") unsigned char ClassTable[1]  = { 0 };
DATA_SEG("INITDATA") unsigned char VendorTable[1] = { 0 };
