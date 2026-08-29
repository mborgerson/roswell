/*
 * PROJECT:     nxkrnl -- a free kernel for the original Xbox
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Data exports published at the console's size, with nothing
 *              in them.
 *
 * Two of the console's data exports carry content this kernel does not
 * have and has decided not to invent.  That decision is about their
 * *content*; their *size* is a separate matter, and getting it wrong is
 * a bug rather than a gap.
 *
 * The export generator's default scaffold for an unmapped data ordinal
 * is a single four-byte word.  A title that reads one of these reads
 * hundreds of bytes of whatever the linker placed next, and -- worse --
 * a title that WRITES one writes over it.  Halo 2 hooks the IDE channel
 * object for its DVD streaming path, installing routines at +0x10 and
 * +0x14 and clearing a field at +0x20; against a four-byte scaffold
 * those three writes landed on unrelated kernel globals.  It survived
 * only because of what the linker happened to put there.
 *
 * So they are published at the size the console publishes them, zeroed.
 * A title reading one still finds nothing, which is the honest answer
 * and the one already decided on; a title writing one now writes into
 * the object instead of into the kernel.
 *
 * Sizes were measured from the console's own headers under the
 * toolchain that builds titles, not counted by hand.
 */

#include <ntdef.h>
#include <ntifs.h>

#define XE_PUBLIC_KEY_SIZE      284

__declspec(align(16)) UCHAR XePublicKeyData[XE_PUBLIC_KEY_SIZE];

/*
 * The IDE channel object the console's ATAPI path hangs off.  Filling
 * it in means giving KINTERRUPT, two KDPCs, a KTIMER and a
 * KDEVICE_QUEUE the console's layout first, so it stays zero and a
 * title's fast path stays unclaimed -- Halo 2 falls back to ordinary
 * IRP I/O, which is what it does today.
 */
#define IDE_CHANNEL_OBJECT_SIZE 264

__declspec(align(16)) UCHAR IdexChannelObject[IDE_CHANNEL_OBJECT_SIZE];

/*
 * The remaining three are not imported by any title in the swept
 * corpus, so nothing reads or writes them today.  They are sized anyway:
 * the rule "every data export is the size the console publishes it" is
 * one a test can check and a reader can rely on, where "sized if a title
 * happens to import it" is neither.  All three are decided against on
 * content, and zero is the answer that decision calls for.
 */

/* The key the console seals its EEPROM with.  This kernel opens the
 * part without it and holds it only in a form it cannot be read back
 * out of, so there is nothing to publish here. */
__declspec(align(16)) UCHAR XboxEEPROMKey[16];

/* Eight pointers into the console's MM internals.  Ours are NT's, and
 * of the eight only the page count has an equivalent here -- there are
 * no VADs to point a root at -- so a title that walks them would read
 * the wrong shape where zero makes it give up. */
__declspec(align(16)) UCHAR MmGlobalData[32];

/* The object handle table.  Same argument: the layout behind it is
 * ntoskrnl's, not the console's. */
__declspec(align(16)) UCHAR ObpObjectHandleTable[48];

/* EOF */
