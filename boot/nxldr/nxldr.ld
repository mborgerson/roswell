/*
 * Linker script for boot/nxldr.  Produces the flash image, laid out at its
 * physical location: the top FLASH_SIZE_LMA of the 32-bit address space (the
 * flash mirrors across 0xff000000-0xffffffff; LOW_ROM_BASE is the last mirror,
 * where the CPU reset vector and MCPX's Xcode entry live).
 *
 *   file off      VMA           content
 *   0x000000      LOW_ROM_BASE  .low_rom: MCPX magic, Xcodes, post-Xcode
 *                               entry + ramcopy stub (runs in-place from flash)
 *   after .low_rom 0x00100000   .text/.rodata/.data source -- ramcopied to
 *                               RAM at boot, runs from VA RAM_CODE thereafter
 *   after .data   n/a           kernel-payload slot -- gen-flash-bin.py
 *                               writes the XZ-compressed xboxkrnl.exe here,
 *                               filling up to the reset window (0xff fill in
 *                               the linker output)
 *   FLASH-0x200   0xfffffe00    .reset_main (16-bit reset path)
 *   FLASH-0x10    0xfffffff0    .reset_vector (CPU power-on entry)
 *
 * Basing the whole in-flash image at the last mirror means every flash-resident
 * VMA matches where it actually executes -- no reliance on lower mirrors.
 *
 * Adapted from cromwell's boot_rom/bootrom.ld (GPLv2), trimmed.
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start_low_rom)

/* Total flash file size, which every file offset below derives from: 256 KB,
 * the real Xbox ROM size.  The debug kernel carries ~200 KB more DPRINT and
 * assert text than the release one and no longer fits that slot compressed, so
 * the DBG build overrides the PROVIDE below with --defsym (see
 * boot/nxldr/CMakeLists.txt) to link a 512 KB image.  DEFINED() would not work
 * here: ld does not consider a --defsym symbol defined for it.
 * That image is an xemu artifact -- xemu mirrors any multiple of 64 KB across
 * the ROM window -- and needs a >=512 KB part to flash on real hardware; the
 * release image stays retail-exact. */
PROVIDE(__nxldr_flash_size__ = 0x40000);
FLASH_SIZE_LMA = __nxldr_flash_size__;

/* The flash occupies the top FLASH_SIZE_LMA of the 32-bit address space (its
 * last mirror) -- that's where the CPU reset vector (0xfffffff0) and MCPX's
 * Xcode entry sit.  Linking the in-flash image here makes every flash-resident
 * VMA match where it actually executes. */
LOW_ROM_BASE   = 0x100000000 - FLASH_SIZE_LMA;   /* 0xfffc0000 at 256 KB */
RAM_CODE       = 0x00100000;   /* VA the ramcopied .text/.rodata/.data run at */

SECTIONS {
    /DISCARD/ : {
        *(.note.gnu.property)
        *(.note.gnu.build-id)
        *(.comment)
        *(.eh_frame)
    }

    /* .low_rom -- runs in-place from flash.  AT(0) so it lands at file
     * offset 0 in the objcopy -O binary output. */
    .low_rom LOW_ROM_BASE : AT (0) {
        *(.low_rom)
        *(.low_rom.*)
        EndLowRom = .;
    }

    /* The .text source follows .low_rom directly (32-byte aligned), so no
     * flash is wasted between them -- the kernel payload then gets everything
     * from the end of .data to the reset window. */
    TEXT_SRC_LMA = ALIGN(SIZEOF(.low_rom), 32);

    /* .text/.rodata/.data -- LMA in flash just past .low_rom (TEXT_SRC_LMA),
     * VMA at RAM_CODE.  Prologue ramcopies SRC(LOW_ROM_BASE+TEXT_SRC_LMA)
     * -> DST(RAM_CODE) at boot.
     *
     * CRITICAL: VMA gaps between sections must equal LMA gaps -- the
     * prologue does ONE flat rep-movsl from flash to RAM, so any VMA
     * pad ld inserts to align the next section must be reflected in
     * LMA too.  Force each section to end on a 32-byte boundary so
     * the next section's implicit alignment is a no-op (no gap).
     * Without these the first byte of .rodata lands 12 bytes below
     * its expected VMA and every string literal reads garbage. */
    .text RAM_CODE : AT (TEXT_SRC_LMA) {
        RamStart = .;
        *(.text)
        *(.text.*)
        . = ALIGN(32);
    }

    .rodata : AT (TEXT_SRC_LMA + SIZEOF(.text)) {
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(32);
    }

    .data : AT (TEXT_SRC_LMA + SIZEOF(.text) + SIZEOF(.rodata)) {
        *(.data)
        *(.data.*)
        . = ALIGN(32);
        RamEnd = .;
    }

    /* .bss is zero-init, no bytes in the output (NOLOAD). */
    .bss (NOLOAD) : {
        BssStart = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4);
        BssEnd = .;
    }

    /* Reset path -- runs when the MCPX bootrom overlay isn't applied
     * (xemu launched without `bootrom=mcpx.bin`).  The whole path fits in a
     * 512-byte window at the top of the LAST mirror (0xfffffe00-0xffffffff),
     * so the kernel payload above can run right up to it:
     *
     *   .reset_main   linear 0xfffffe00, file off FLASH-0x200:
     *                 16-bit rm_init + GDT + 32-bit pmode_entry +
     *                 xcode_interpreter (~270 bytes).
     *   .reset_vector linear 0xfffffff0, file off FLASH-0x10:
     *                 the 16-byte reset window the CPU lands on at power-on.
     *
     * The build's objcopy --gap-fill=0xff fills the gap between the payload
     * and .reset_main with 0xff. */
    /* KEEP() -- the linker entry is _start_low_rom (the MCPX path); the
     * reset path isn't reachable from there, so without KEEP() --gc-sections
     * drops both sections and the CPU lands on 0xff fill when booted
     * without an MCPX bootrom overlay. */
    .reset_main 0xfffffe00 : AT (FLASH_SIZE_LMA - 0x200) {
        KEEP(*(.reset_main))
    }

    .reset_vector 0xfffffff0 : AT (FLASH_SIZE_LMA - 0x10) {
        KEEP(*(.reset_vector))
    }
}

/* rm_init starts .reset_main; its VMA low word must equal startup.S's
 * RESET_MAIN_OFF (the reset vector + lgdtl encode offsets against it). */
ASSERT(rm_init == 0xfffffe00,
       "boot/nxldr: .reset_main VMA != startup.S RESET_MAIN_OFF (0xfffffe00)");
ASSERT(SIZEOF(.reset_main) <= 0xfffffff0 - 0xfffffe00,
       "boot/nxldr: .reset_main overflows into the reset vector");

/* Symbols consumed by the .low_rom prologue. */
RamCopySrc       = LOW_ROM_BASE + TEXT_SRC_LMA;
RamCopyDst       = RAM_CODE;
RamCopySizeLongs = (SIZEOF(.text) + SIZEOF(.rodata) + SIZEOF(.data) + 3) / 4;
BssSizeLongs     = (BssEnd - BssStart + 3) / 4;
StackTop         = 0x001ffff0;

/* Kernel payload region: the XZ-compressed xboxkrnl follows the .data source
 * and runs up to the reset page -- its offset is wherever the loader code
 * ends, not a hardcoded slot.  gen-flash-bin.py reads KernelPayloadOffset /
 * KernelPayloadMaxSize via `nm` to know where + how much to inject; loader.c
 * reads the payload at KernelPayloadVa (its flash-MMIO address). */
KernelPayloadOffset  = ALIGN(TEXT_SRC_LMA + SIZEOF(.text) + SIZEOF(.rodata) + SIZEOF(.data), 32);
KernelPayloadMaxSize = (FLASH_SIZE_LMA - 0x200) - KernelPayloadOffset;
KernelPayloadVa      = LOW_ROM_BASE + KernelPayloadOffset;
ASSERT(KernelPayloadOffset < FLASH_SIZE_LMA - 0x200,
       "boot/nxldr: loader code leaves no room for the kernel payload");

/* Where MCPX's on-die interpreter -- and our stand-in for it in startup.S --
 * reads the Xcode bytecode: .low_rom offset 0x80, in this image's mirror. */
XcodeStart       = LOW_ROM_BASE + 0x80;

/* MCPX overflow-trick jump stub (xcodes.inc.S).  The poked stub does
 * `mov $XcodeJumpAddr, %eax; jmp *%eax`, jumping to the post-Xcode entry.  The
 * target MUST be _post_xcode_entry's own link VMA (in the LOW_ROM_BASE mirror),
 * NOT a different flash mirror: _post_xcode_entry ends in a PC-relative
 * `jmp _ram_entry`, so it only resolves when run-address == link-address.
 * Jumping to the 0xff000000 mirror instead would land _ram_entry off by the
 * mirror delta and triple-fault -- the MCPX-overlay boot regression that
 * appeared when LOW_ROM_BASE moved off 0xff000000.
 *
 * The 32-bit address straddles two poke value words; the assembler can't fold
 * a shifted relocation, so the linker computes the split here and xcodes.inc.S
 * emits XcodeJumpLoValue / XcodeJumpHiValue as plain .long words:
 *
 *   B8 <addr[0:3]>   -> XcodeJumpLoValue = 0xB8 | (addr[0:2] << 8)
 *   <addr[3]> FF E0 90 -> XcodeJumpHiValue = addr[3] | (0x90E0FF << 8) */
XcodeJumpAddr    = _post_xcode_entry;
XcodeJumpLoValue = 0x000000b8 | ((XcodeJumpAddr & 0x00ffffff) << 8);
XcodeJumpHiValue = 0x90e0ff00 | ((XcodeJumpAddr >> 24) & 0xff);
