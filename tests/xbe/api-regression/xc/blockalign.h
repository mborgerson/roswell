#pragma once

/*
 * The console's block cipher reaches into its caller's input buffer when
 * that buffer is not eight-byte aligned -- whatever it writes there it
 * puts back, so in RAM the call is correct and the write invisible, but
 * on a read-only page it bugchecks.  Eight is the block size.
 *
 * Every buffer handed to XcKeyTable / XcBlockCrypt / XcBlockCryptCBC
 * carries this, so no buffer's alignment depends on where the linker
 * happened to put it.  A `char` array has no natural alignment to fall
 * back on: it is one byte, wherever it lives.
 */
#define XC_BLOCK_ALIGNED __attribute__((aligned(8)))
