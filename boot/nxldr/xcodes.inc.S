/*
 * Xcode bytecode for MCPX.  These program the chipset (SMBUS, NV2A, MCPX,
 * SDRAM, video encoder I2C handshake, PIC) before the CPU comes out of
 * reset for real C code.  The values are load-bearing and identical to
 * what retail boots use.
 *
 * Imported verbatim from cromwell/boot_rom/Xcodes.h (GPLv2,
 * (C) 2002 Andy / Michael / Paul / Steve / Lehner Franz et al.).  Macros
 * are defined in startup.S to keep this file's payload pure bytecode.
 *
 * MCPX revision is locked to the pre-D5 variant.  xemu emulates a retail
 * MCPX (rev <= D5); MCPXREVD5 would target later devkit boards.
 */

	xcode_pciout(0x80000884, 0x00008001);
	xcode_pciout(0x80000810, 0x00008001);
	xcode_pciout(0x80000804, 0x00000003);
	xcode_outb(0x00008049, 0x00000008);
	xcode_outb(0x000080d9, 0x00000000);
	xcode_outb(0x00008026, 0x00000001);
	xcode_pciout(0x8000f04c, 0x00000001);
	xcode_pciout(0x8000f018, 0x00010100);
	xcode_pciout(0x80000084, 0x07ffffff);

	xcode_pciout(0x8000f020, 0x0ff00f00);
	xcode_pciout(0x8000f024, 0xf7f0f000);
	xcode_pciout(0x80010010, 0x0f000000);
	xcode_pciout(0x80010014, 0xf0000000);
	xcode_pciout(0x80010004, 0x00000007);
	xcode_pciout(0x8000f004, 0x00000007);

	xcode_poke(0x0f0010b0, 0x07633461);
	xcode_poke(0x0f0010cc, 0x66660000);

	xcode_peek(0x0f101000);
	xcode_bittoggle(0x000c0000,0x00000000);
	xcode_ifgoto(0x00000000,6);

	xcode_peek(0x0f101000);
	xcode_bittoggle(0xe1f3ffff,0x80000000);
	xcode_poke_a(0x0f101000);
	xcode_poke(0x0f0010b8, 0xeeee0000);
	xcode_goto(11);

	xcode_ifgoto(0x000c0000,6);

	xcode_peek(0x0f101000);
	xcode_bittoggle(0xe1f3ffff,0x860c0000);
	xcode_poke_a(0x0f101000);
	xcode_poke(0x0f0010b8, 0xffff0000);
	xcode_goto(5);

	xcode_peek(0x0f101000);
	xcode_bittoggle(0xe1f3ffff,0x820c0000);
	xcode_poke_a(0x0f101000);
	xcode_poke(0x0f0010b8, 0x11110000);
	xcode_poke(0x0f0010d4, 0x00000009);
	xcode_poke(0x0f0010b4, 0x00000000);
	xcode_poke(0x0f0010bc, 0x00005866);
	xcode_poke(0x0f0010c4, 0x0351c858);
	xcode_poke(0x0f0010c8, 0x30007d67);
	xcode_poke(0x0f0010d8, 0x00000000);
	xcode_poke(0x0f0010dc, 0xa0423635);
	xcode_poke(0x0f0010e8, 0x0c6558c6);
	xcode_poke(0x0f100200, 0x03070103);
	xcode_poke(0x0f100410, 0x11000016);
	xcode_poke(0x0f100330, 0x84848888);
	xcode_poke(0x0f10032c, 0xffffcfff);
	xcode_poke(0x0f100328, 0x00000001);
	xcode_poke(0x0f100338, 0x000000df);

	/* SMBus controller. */
	xcode_pciout(0x80000904, 0x00000001);
	xcode_pciout(0x80000914, 0x0000c001);
	xcode_pciout(0x80000918, 0x0000c201);
	xcode_outb(0x0000c200, 0x00000070);

	/* Video encoder probe (Conexant, Focus, Xcalibur in turn).  The PIC
	 * snoops the I2C bus and resets us if it doesn't see a successful
	 * encoder handshake within a tight window. */

	/* Conexant. */
	xcode_outb(0x0000c004, 0x0000008a);
	xcode_outb(SMBUS+8, 0x000000ba);
	xcode_outb(SMBUS+6, 0x0000003f);
	xcode_outb(SMBUS+2, 0x0000000a);
	xcode_inb(SMBUS);
	xcode_ifgoto(0x00000010,2);
	xcode_goto(4);
	xcode_bittoggle(0x00000008, 0x00000000);
	xcode_ifgoto(0x00000000,-4);
	xcode_goto(39);
	xcode_outb(SMBUS, 0x00000010);
	SMB_xcode_Write(0x6c,0x46);
	SMB_xcode_Write(0xb8,0x00);
	SMB_xcode_Write(0xce,0x19);
	SMB_xcode_Write(0xc6,0x9c);
	SMB_xcode_Write(0x32,0x08);
	SMB_xcode_Write(0xc4,0x01);
	xcode_goto(36);

	/* Focus. */
	xcode_outb(SMBUS, 0x000000ff);
	xcode_outb(SMBUS, 0x00000010);
	xcode_outb(0x0000c004, 0x000000d4);
	xcode_outb(SMBUS+8, 0x0000000c);
	xcode_outb(SMBUS+6, 0x00000000);
	xcode_outb(SMBUS+2, 0x0000000a);
	xcode_inb(SMBUS);
	xcode_ifgoto(0x00000010,2);
	xcode_goto(4);
	xcode_bittoggle(0x00000008, 0x00000000);
	xcode_ifgoto(0x00000000,-4);
	xcode_goto(9);
	xcode_outb(SMBUS, 0x00000010);
	SMB_xcode_Write(0x0d,0x20);
	xcode_goto(16);

	/* Xcalibur (no check; previous two already failed, last resort). */
	xcode_outb(SMBUS, 0x000000ff);
	xcode_outb(SMBUS, 0x00000010);
	xcode_outb(0x0000c004, 0x000000E0);
	SMB_xcode_Write(0x0,0x0);
	SMB_xcode_Write(0xb8,0x00);

	/* VIDEND -- encoder init complete.  Talk to the PIC. */

	xcode_outb(SMBUS+4, 0x00000020);
	SMB_xcode_Write(0x01,0x00);

	xcode_outb(SMBUS+4, 0x00000021);
	xcode_outb(SMBUS+8, 0x00000001);
	xcode_outb(SMBUS+2, 0x0000000a);
	xcode_inb(SMBUS);
	xcode_ifgoto(0x00000010,-1)
	xcode_outb(SMBUS, 0x00000010);

	xcode_inb(SMBUS+6);

	xcode_poke(0x0f680500, 0x00011c01);
	xcode_poke(0x0f68050c, 0x000a0400);
	xcode_poke(0x0f001220, 0x00000000);
	xcode_poke(0x0f001228, 0x00000000);
	xcode_poke(0x0f001264, 0x00000000);
	xcode_poke(0x0f001210, 0x00000010);
	xcode_peek(0x0f101000);
	xcode_bittoggle(0x06000000,0x00000000);
	xcode_ifgoto(0x00000000,4);
	xcode_poke(0x0f001214, 0x48480848);
	xcode_poke(0x0f00122c, 0x88888888);
	xcode_goto(7);
	xcode_ifgoto(0x06000000,4);
	xcode_poke(0x0f001214, 0x09090909);
	xcode_poke(0x0f00122c, 0xaaaaaaaa);
	xcode_goto(3);

	xcode_poke(0x0f001214, 0x09090909);
	xcode_poke(0x0f00122c, 0xaaaaaaaa);
	xcode_poke(0x0f001230, 0xffffffff);
	xcode_poke(0x0f001234, 0xaaaaaaaa);
	xcode_poke(0x0f001238, 0xaaaaaaaa);
	xcode_poke(0x0f00123c, 0x8b8b8b8b);
	xcode_poke(0x0f001240, 0xffffffff);
	xcode_poke(0x0f001244, 0x8b8b8b8b);
	xcode_poke(0x0f001248, 0x8b8b8b8b);
	xcode_poke(0x0f1002d4, 0x00000001);
	xcode_poke(0x0f1002c4, 0x00100042);
	xcode_poke(0x0f1002cc, 0x00100042);
	xcode_poke(0x0f1002c0, 0x00000011);
	xcode_poke(0x0f1002c8, 0x00000011);
	xcode_poke(0x0f1002c0, 0x00000032);
	xcode_poke(0x0f1002c8, 0x00000032);
	xcode_poke(0x0f1002c0, 0x00000132);
	xcode_poke(0x0f1002c8, 0x00000132);
	xcode_poke(0x0f1002d0, 0x00000001);
	xcode_poke(0x0f1002d0, 0x00000001);
	xcode_poke(0x0f100210, 0x80000000);
	xcode_poke(0x0f00124c, 0xaa8baa8b);
	xcode_poke(0x0f001250, 0x0000aa8b);
	xcode_poke(0x0f100228, 0x081205ff);

	xcode_poke(0x0f001218, 0x00010000);

	xcode_pciin_a(0x80000860);
	xcode_bittoggle(0xffffffff,0x00000400);
	xcode_pciout_a(0x80000860);

	xcode_pciout(0x8000084c, 0x0000fdde);
	xcode_pciout(0x8000089c, 0x871cc707);
	xcode_pciin_a(0x800008b4);
	xcode_bittoggle(0xfffff0ff,0x00000f00);
	xcode_pciout_a(0x800008b4);
	xcode_pciout(0x80000340, 0xf0f0c0c0);
	xcode_pciout(0x80000344, 0x00c00000);
	xcode_pciout(0x8000035c, 0x04070000);
	xcode_pciout(0x8000036c, 0x00230801);
	xcode_pciout(0x8000036c, 0x01230801);
	xcode_goto(1);
	xcode_goto(1);
	xcode_poke(0x0f100200, 0x03070103);
	xcode_poke(0x0f100204, 0x11448000);
	xcode_pciout(0x8000103c, 0x00000000);

	xcode_outb(SMBUS, 0x00000010);

	/* Report a "good memory" result to the PIC scratch register. */
	xcode_outb(SMBUS+4, 0x00000020);
	SMB_xcode_Write(0x13,0x0f);
	SMB_xcode_Write(0x12,0xf0);

	/* Reload NV2A registers. */
	xcode_pciout(0x8000f020, 0xfdf0fd00);
	xcode_pciout(0x80010010, 0xfd000000);

	/* Overflow trick: poke a tiny stub into RAM:0 that the CPU runs off the
	 * end of the bytecode into; it jumps to our post-Xcode entry:
	 *
	 *   B8 <entry:32>   mov  $entry, %eax
	 *   FF E0           jmp  *%eax
	 *   90              nop
	 *
	 * The 32-bit entry straddles the two poke value words.  The assembler
	 * can't fold the shifted entry address into a relocation, but the linker
	 * can: XcodeJumpLoValue / XcodeJumpHiValue (nxldr.ld) carry the split,
	 * computed-at-link-time stub words, so the entry tracks its real placement
	 * with no hardcoded offset. */
	.byte 0x03; .long 0x00000000   /* poke #1: addr RAM:0 */
	.long XcodeJumpLoValue         /* B8 (mov %eax) + entry[0:3]   */
	.byte 0x03; .long 0x00000004   /* poke #2: addr RAM:4 */
	.long XcodeJumpHiValue         /* entry[3] + FF E0 90 (jmp; nop) */

	xcode_END(0x806);
