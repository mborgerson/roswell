/*
 * boot/nxldr -- minimal Xbox loader entry.
 *
 * Replaces cromwell+freeldr in the boot chain.  MCPX runs our Xcodes at
 * flash file offset 0x80, the Xcodes-end overflow trick lands the CPU at
 * 0xfffc1000 (top-256K of flash, mirrored copy), and from there we set up
 * MTRRs, copy the RAM-resident half of this binary into PA 0x100000, and
 * call LoaderMain() in C on the flat segments we were entered with.
 *
 * Adapted from cromwell/boot_rom/2bBootStartup.S (GPLv2, (C) 2002 Andy,
 * Michael, Paul, Steve, Lehner Franz et al.).  Trimmed of:
 *   - imagebld signature (xemu's MCPX doesn't verify)
 *   - PCI IDE/NIC bring-up (kernel does this -- see nxkrnl-owns-machine-config)
 *   - tail jmp into cromwell C (we go straight to our LoaderMain)
 */

/* Xcode interpreter encoding (1-byte opcode + 2x u32 operand).  These mirror
 * cromwell's 2bBootStartup.S macros 1:1 so xcodes.inc.S can be lifted
 * verbatim. */
#define xcode_peek(val1)         .byte 0x2;  .long val1; .long 0x0
#define xcode_poke(val1, val2)   .byte 0x3;  .long val1; .long val2
#define xcode_pciout(val1, val2) .byte 0x4;  .long val1; .long val2
#define xcode_pciin_a(val1)      .byte 0x5;  .long val1; .long 0x0
#define xcode_bittoggle(v1, v2)  .byte 0x6;  .long v1;   .long v2
#define xcode_ifgoto(v1, v2)     .byte 0x8;  .long v1;   .long (9 * (v2 - 1))
#define xcode_outb(val1, val2)   .byte 0x11; .long val1; .long val2
#define xcode_inb(val1)          .byte 0x12; .long val1; .long 0x0
#define xcode_poke_a(val1)       .byte 0x7;  .long 0x3;  .long val1
#define xcode_pciout_a(val1)     .byte 0x7;  .long 0x4;  .long val1
#define xcode_outb_a(val1)       .byte 0x7;  .long 0x11; .long val1
#define xcode_goto(val1)         .byte 0x9;  .long 0x0;  .long (9 * (val1 - 1))
#define xcode_END(val1)          .byte 0xEE; .long val1; .long 0x0

#define SMBUS 0x0000c000

#define SMB_xcode_Write(val1, val2) \
    xcode_outb(SMBUS+8, val1); \
    xcode_outb(SMBUS+6, val2); \
    xcode_outb(SMBUS+2, 0x0000000a); \
    xcode_inb(SMBUS); \
    xcode_ifgoto(0x00000010, -1); \
    xcode_outb(SMBUS, 0x00000010);

.code32

/*
 * .low_rom -- runs in-place from flash MMIO.  VMA = 0xfffc0000+.
 * Layout matches what MCPX expects at the start of an Xbox flash image.
 */
.section .low_rom, "ax"

.global _start_low_rom
_start_low_rom:

.org 0x0
    /* MCPX clock timings (magic values; see Xcalibur reverse-engineering
     * docs).  MCPX validates these before clocking the CPU out of reset. */
    .long 0xff000009
    .long 0xff000008
    .long 0x2b16d065
    .long 0x3346322d
    .long 0x01010101
    .long 0x08080808
    .long 0x00000801
    .long 0xc8fc7c8a    /* MCPX <= D5 */
    .long 0x44290213
    .long 0x90004998
    .long 0x00000000
    .long 0xffffffff
    .long 0xffffffff

.org 0x6c
    .long 0x00000107

.org 0x70                /* MCPX config area */
    .long 0x0000000f
    .long 0x40004400
    .long 0x12d10070     /* MCPX <= D5 */
    .long 0x00000c90

.org 0x80
    /* Xcode bytecode -- MCPX's on-die interpreter reads from this offset
     * and stops at xcode_END.  Macros above; payload pulled from cromwell
     * verbatim.  When xemu is launched without `bootrom=mcpx.bin` the same
     * bytecode is executed by xcode_interpreter() in xcode.c (called via
     * the reset path at the end of this file). */
#include "xcodes.inc.S"

/*
 * Post-Xcode entry.  Reached two ways:
 *   - With an MCPX bootrom overlay: the Xcode tail-poke jumps the CPU here
 *     directly (in flat 32-bit pmode, CS = MCPX's bring-up selector).
 *   - Without: the reset vector at the end of this file runs our 16-bit
 *     init, transitions to pmode, calls xcode_interpreter() (which replays
 *     the bytecode above), then `jmp _post_xcode_entry`.
 * Must sit at exactly offset 0x1000 -- 0xfffc0000 + 0x1000 = 0xfffc1000,
 * the absolute address the Xcode overflow trick targets.
 */
.org 0x1000
.global _post_xcode_entry
_post_xcode_entry:
    /* Disable APIC base / clear pending interrupts (MSR 0x1b). */
    movl  $0x1b, %ecx
    xor   %eax, %eax
    xor   %edx, %edx
    wrmsr

    xor   %eax, %eax
    xor   %edx, %edx
    xor   %ecx, %ecx

    /* Disable cache before reprogramming MTRRs: CR0.CD (bit 30) +
     * CR0.NW (bit 29). */
    mov   %cr0, %eax
    orl   $0x60000000, %eax
    mov   %eax, %cr0
    wbinvd

    /* Clear CR3 (no paging yet anyway). */
    mov   %eax, %eax
    mov   %eax, %cr3

    /* Clear IA32_MTRR_DEF_TYPE. */
    movl  $0x2ff, %ecx
    xor   %eax, %eax
    xor   %edx, %edx
    wrmsr

    /* MTRR_PHYSBASE0: 0x00000000, WB caching. */
    movl  $0x200, %ecx
    movl  $0x00000000, %edx
    movl  $0x00000006, %eax
    wrmsr
    /* MTRR_PHYSMASK0: 128 MB range. */
    movl  $0x201, %ecx
    movl  $0x0000000f, %edx
    movl  $0xf8000800, %eax
    wrmsr

    /* MTRR_PHYSBASE1: 0xf0000000 (NV2A framebuffer aperture), WC. */
    movl  $0x202, %ecx
    movl  $0x00000000, %edx
    movl  $0xf0000001, %eax
    wrmsr
    /* MTRR_PHYSMASK1: 128 MB range. */
    movl  $0x203, %ecx
    movl  $0x0000000f, %edx
    movl  $0xf8000800, %eax
    wrmsr

    /* MTRR_PHYSBASE2 / MASK2: cover flash MMIO as uncacheable.  Match
     * cromwell's values (top 1MB at 0xfff00000): nothing we run touches
     * flash outside this range and the kernel reprograms MTRRs anyway. */
    movl  $0x204, %ecx
    movl  $0x00000000, %edx
    movl  $0xfff00000, %eax
    wrmsr
    movl  $0x205, %ecx
    movl  $0x0000000f, %edx
    movl  $0xfff00800, %eax
    wrmsr

    /* Zero the remaining MTRR pair registers. */
    xor   %eax, %eax
    xor   %edx, %edx
    movl  $0x206, %ecx; wrmsr
    movl  $0x207, %ecx; wrmsr
    movl  $0x208, %ecx; wrmsr
    movl  $0x209, %ecx; wrmsr
    movl  $0x20a, %ecx; wrmsr
    movl  $0x20b, %ecx; wrmsr
    movl  $0x20c, %ecx; wrmsr
    movl  $0x20d, %ecx; wrmsr
    movl  $0x20e, %ecx; wrmsr
    movl  $0x20f, %ecx; wrmsr

    /* Enable MTRRs. */
    movl  $0x2ff, %ecx
    xor   %edx, %edx
    movl  $0x800, %eax
    wrmsr

    /* Turn cache back on (clear CR0.CD + CR0.NW). */
    movl  %cr0, %eax
    andl  $0x9fffffff, %eax
    movl  %eax, %cr0

    cld

    /* Copy .text/.rodata/.data from flash (right after .low_rom) to
     * RAM (PA 0x100000+). */
    mov   $RamCopyDst, %edi
    mov   $RamCopySrc, %esi
    mov   $RamCopySizeLongs, %ecx
    rep   movsl

    /* Jump into the RAM-resident half. */
    jmp   _ram_entry

/*
 * .text -- ram-resident.  Runs from VMA 0x100000+ after the copy above.
 */
.section .text, "ax"

/* No loader GDT.  We were entered in flat 32-bit pmode -- via the reset
 * path's reset_gdt (CS=0x08/DS=0x10) or, with an MCPX bootrom overlay, MCPX's
 * flat bring-up selectors -- and the loader never reloads a segment register,
 * so the cached flat code/data descriptors carry it all the way to the
 * handoff.  The next segment reload is in KiSystemStartup, which builds and
 * loads the kernel's own GDT (KiGdt) first.  So a loader GDT would just be a
 * redundant re-init.  Same for the IDT: the loader fields no interrupts and
 * KiSystemStartup installs KiIdt, so we just run with whatever IDTR the entry
 * left and keep interrupts masked. */

.global _ram_entry
_ram_entry:
    /* Mask interrupts -- the loader has no IDT (the kernel installs KiIdt in
     * KiSystemStartup), so any IRQ would have no handler.  cli blocks INTR;
     * mask both PICs at the source too (an NMI/SMI still gets through, but
     * those shouldn't fire here).  The kernel re-enables interrupts after it
     * installs its own IDT. */
    cli
    movb  $0xff, %al
    outb  %al,  $0x21
    outb  %al,  $0xa1

    /* Stack. */
    movl  $StackTop, %esp

    /* Clear .bss. */
    xor   %eax, %eax
    movl  $BssSizeLongs, %ecx
    movl  $BssStart, %edi
    rep   stosl

    /* Into C. */
    call  LoaderMain

    /* LoaderMain should not return; if it does, halt. */
.Lhalt:
    cli
    hlt
    jmp   .Lhalt

/*
 * Reset path -- runs when no MCPX bootrom overlay is applied (e.g. xemu
 * launched without `bootrom=mcpx.bin`).  The CPU enters at 0xfffffff0 in
 * 16-bit real mode with CS hidden base = 0xffff0000.  We do everything in
 * flash and registers; no RAM, no stack, no descriptor tables besides a
 * tiny GDT in this section.
 *
 *   1. Reset vector at 0xfffffff0 -- 5-byte short jmp backward to rm_init.
 *   2. rm_init (16-bit) -- lgdt + CR0.PE + ljmp to pmode_entry.
 *   3. pmode_entry (32-bit, flat segments) -- run xcode_interpreter, then
 *      `jmp _post_xcode_entry` to hand off to the existing init path.
 *
 * Layout placement (see nxldr.ld): the whole reset path lives in a 512-byte
 * window at the very top of the address space (the last flash mirror), so the
 * kernel payload can use the flash right up to it.
 *   .reset_main   linear 0xfffffe00, file off FLASH-0x200 -- rm_init + GDT +
 *                                                            pmode_entry +
 *                                                            xcode_interpreter
 *   .reset_vector linear 0xfffffff0, file off FLASH-0x10  -- 5-byte jmp +
 *                                                            padding
 *
 * Both live in the 64-KiB real-mode window the CPU reaches at reset with CS
 * hidden base = 0xffff0000 (offsets 0xfe00 / 0xfff0).  RESET_MAIN_OFF is
 * rm_init's offset in that window; it must equal (.reset_main VMA & 0xffff),
 * which nxldr.ld asserts.
 */

#define RESET_MAIN_OFF 0xfe00 /* rm_init's offset in the reset real-mode window */

.section .reset_main, "ax"
.code16

/*
 * 16-bit real-mode init.  CS hidden base = 0xffff0000; CS selector = 0xf000.
 * Symbol `reset_gdt_desc` resolves to its 32-bit linear address; in real
 * mode that's reachable as CS:(linear & 0xffff) because the hidden base
 * already supplies the high 0xffff0000.
 */
.global rm_init
rm_init:
    cli
    cld

    /* Load a flat 32-bit GDT.  lgdtl is the 32-bit-base form of LGDT (the
     * `l` suffix tells GAS to emit the 0x66 operand-size prefix in .code16).
     * The memory operand is a CS-relative displacement; we compute it as a
     * 16-bit literal entirely at assembly time so the linker never sees a
     * 32-bit value it tries to truncate.
     *
     *   rm_init is at the start of .reset_main (VMA 0xfffffe00), so its
     *   offset within the CS=0xf000 real-mode window is RESET_MAIN_OFF.  The
     *   offset of reset_gdt_desc is RESET_MAIN_OFF + (reset_gdt_desc -
     *   rm_init); the difference is a same-section, assembly-time constant. */
    lgdtl %cs:(RESET_MAIN_OFF + reset_gdt_desc - rm_init)

    /* Enable protected mode (CR0.PE = 1). */
    movl  %cr0, %eax
    orl   $0x1, %eax
    movl  %eax, %cr0

    /* Far jmp to 32-bit code segment.  Operand-size prefix + ljmp encodes
     * a 32-bit offset; selector 0x08 = our flat 32-bit code descriptor.
     * After this jmp the CPU executes in 32-bit pmode, CS base = 0. */
    .byte 0x66, 0xea
    .long pmode_entry
    .word 0x08

/*
 * Flat 32-bit GDT used only for the reset transition; the loader installs
 * its own GDT in _ram_entry (above).  Three entries:
 *
 *   0x00 null
 *   0x08 code32, base 0, limit 4G
 *   0x10 data32, base 0, limit 4G
 */
.balign 8
reset_gdt:
    .quad 0x0000000000000000
    .quad 0x00cf9b000000ffff
    .quad 0x00cf93000000ffff
reset_gdt_end:

reset_gdt_desc:
    .word reset_gdt_end - reset_gdt - 1
    .long reset_gdt

/*
 * 32-bit pmode entry.  CS = 0x08 (flat code), other segs uninitialized;
 * load them from our flat data selector, then drop into the interpreter.
 * No stack -- the interpreter is jmp-only, no call/ret.
 */
.code32
.global pmode_entry
pmode_entry:
    movw  $0x10, %ax
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %ss
    movw  %ax, %fs
    movw  %ax, %gs

    /* Fall through into xcode_interpreter. */

/*
 * Xcode interpreter.  Walks the bytecode at 0xfffc0080 and emulates what
 * MCPX's on-die interpreter would have done.  Register map matches a
 * straight transliteration of the C sketch:
 *
 *   ESI -- pc          (next bytecode instruction)
 *   AL  -- opcode      (low byte of EAX)
 *   EBX -- operand_1
 *   ECX -- operand_2
 *   EDI -- result      (the "accumulator" reads/PCI reads write into)
 *   EBP -- scratch     (opcode 0x10 bit-toggle target)
 *
 * Every dispatch path ends with `jmp xc_advance` (which adds 9 to ESI and
 * loops); 0xEE and unknown opcodes `jmp _post_xcode_entry`.
 *
 * The bytecode encoding is 1 byte opcode + two 32-bit operands == 9 bytes
 * per instruction.
 */
.global xcode_interpreter
xcode_interpreter:
    movl  $0xfffc0080, %esi      /* pc -> Xcode start (LOW_ROM_BASE + 0x80) */
    xorl  %edi, %edi             /* result = 0 */
    xorl  %ebp, %ebp             /* scratch = 0 */

xc_loop:
    movzbl (%esi),   %eax        /* opcode in AL */
    movl   1(%esi),  %ebx        /* operand_1 */
    movl   5(%esi),  %ecx        /* operand_2 */

    /* 0x07 indirection: re-dispatch with opcode = (u8)operand_1,
     * operand_1 = operand_2, operand_2 = result. */
    cmpb  $0x07, %al
    jne   xc_dispatch
    movb  %bl,   %al
    movl  %ecx,  %ebx
    movl  %edi,  %ecx

xc_dispatch:
    cmpb  $0xee, %al
    je    xc_done
    cmpb  $0x02, %al
    je    xc_peek
    cmpb  $0x03, %al
    je    xc_poke
    cmpb  $0x04, %al
    je    xc_pciout
    cmpb  $0x05, %al
    je    xc_pciin
    cmpb  $0x06, %al
    je    xc_bittoggle
    cmpb  $0x08, %al
    je    xc_ifgoto
    cmpb  $0x09, %al
    je    xc_goto
    cmpb  $0x10, %al
    je    xc_scratch_toggle
    cmpb  $0x11, %al
    je    xc_outb
    cmpb  $0x12, %al
    je    xc_inb
    /* unknown opcode -- stop gracefully */
    jmp   xc_done

xc_peek:
    /* result = *(uint32_t *)(operand_1 & 0x0fffffff) */
    andl  $0x0fffffff, %ebx
    movl  (%ebx), %edi
    jmp   xc_advance

xc_poke:
    /* *(uint32_t *)operand_1 = operand_2 */
    movl  %ecx, (%ebx)
    jmp   xc_advance

xc_pciout:
    /* if (operand_1 == 0x80000880) operand_2 &= 0xfffffffd */
    cmpl  $0x80000880, %ebx
    jne   1f
    andl  $0xfffffffd, %ecx
1:
    /* outl 0xcf8, operand_1 */
    movl  %ebx, %eax
    movw  $0xcf8, %dx
    outl  %eax, %dx
    /* outl 0xcfc, operand_2 */
    movl  %ecx, %eax
    movw  $0xcfc, %dx
    outl  %eax, %dx
    jmp   xc_advance

xc_pciin:
    /* outl 0xcf8, operand_1 */
    movl  %ebx, %eax
    movw  $0xcf8, %dx
    outl  %eax, %dx
    /* result = inl 0xcfc */
    movw  $0xcfc, %dx
    inl   %dx, %eax
    movl  %eax, %edi
    jmp   xc_advance

xc_bittoggle:
    /* result = (result & operand_1) | operand_2 */
    andl  %ebx, %edi
    orl   %ecx, %edi
    jmp   xc_advance

xc_ifgoto:
    /* if (result != operand_1) pc += operand_2 */
    cmpl  %ebx, %edi
    je    xc_advance
    addl  %ecx, %esi
    jmp   xc_advance

xc_goto:
    /* pc += operand_2 */
    addl  %ecx, %esi
    jmp   xc_advance

xc_scratch_toggle:
    /* scratch = (scratch & operand_1) | operand_2; result = scratch */
    andl  %ebx, %ebp
    orl   %ecx, %ebp
    movl  %ebp, %edi
    jmp   xc_advance

xc_outb:
    /* outb port=operand_1, byte=operand_2 */
    movw  %bx, %dx
    movl  %ecx, %eax
    outb  %al, %dx
    jmp   xc_advance

xc_inb:
    /* result = (uint32_t)inb(operand_1) */
    movw  %bx, %dx
    inb   %dx, %al
    movzbl %al, %edi
    jmp   xc_advance

xc_advance:
    addl  $9, %esi
    jmp   xc_loop

xc_done:
    /* Hand off to the same entry MCPX would have jumped to. */
    jmp   _post_xcode_entry


/*
 * Reset vector -- the CPU's first instruction at power-on / reset.
 * Linear address 0xfffffff0; CS hidden base = 0xffff0000, CS:IP = f000:fff0.
 *
 * Short jmp backward to `rm_init`, which sits at the start of .reset_main
 * (linear 0xfffffe00).  The remaining bytes pad to 0xff so the 16-byte window
 * is well-defined.
 */
.section .reset_vector, "ax"
.code16
.global _reset_vector
_reset_vector:
    /* The assembler can't compute a near-jmp displacement between sections
     * with different VMAs (the linker resolves both to absolute 32-bit
     * linear addresses and the 16-bit signed displacement overflows), so
     * encode the 3-byte rel16 jmp by hand from the constant offsets.
     *
     * Target: rm_init at CS-window offset RESET_MAIN_OFF (0xfe00).
     * Source: _reset_vector at offset 0xfff0; IP after the 3-byte jmp = 0xfff3.
     * Signed 16-bit displacement = RESET_MAIN_OFF - 0xfff3 (= -0x1f3 = 0xfe0d).
     *
     * Both rm_init and _reset_vector live in the same 64-KiB real-mode
     * segment (CS hidden base = 0xffff0000), so the 16-bit modular
     * arithmetic the CPU performs lands correctly on rm_init. */
    .byte 0xe9
    .word (RESET_MAIN_OFF - 0xfff3)

    /* Fill the remaining 13 bytes of the 16-byte reset window with 0xff
     * (matches the flash-default "blank" pattern). */
    .fill 13, 1, 0xff
