/*
 * KeBugCheckEx probe -- a one-off XBE, because the ordinal under test
 * does not return and so cannot live in the api-regression suite.
 *
 * It prints the address of the kernel's KiBugCheckData over COM1 (the
 * DATA export hands the array's address straight to the title), then
 * bugchecks with five values chosen to be unmistakable.  Read the array
 * back over xemu's QEMU monitor once the box has stopped:
 *
 *   qemu-system-i386 -config_path <build>/run/xemu-<which>-128.toml \
 *       -snapshot -device lpc47m157 -serial file:LOG \
 *       -monitor unix:SOCK,server,nowait
 *   (over SOCK, with the address LOG printed)  x/5xw <address>
 *
 * The same XBE runs on the retail kernel, where the export is real, so
 * the two can be compared word for word.
 */

#include <xboxkrnl/xboxkrnl.h>
#include <stdio.h>
#include "../api-regression/tap.h"

#define BUGCHECK_CODE 0x000000E3
#define PARAM1        0x11111111
#define PARAM2        0x22222222
#define PARAM3        0x33333333
#define PARAM4        0x44444444

int main(void)
{
    char line[96];

    snprintf(line, sizeof(line), "bugcheck: KiBugCheckData at %p\n",
             (void *)KiBugCheckData);
    tap_puts(line);
    snprintf(line, sizeof(line),
             "bugcheck: at rest %08lx %08lx %08lx %08lx %08lx\n",
             (unsigned long)KiBugCheckData[0], (unsigned long)KiBugCheckData[1],
             (unsigned long)KiBugCheckData[2], (unsigned long)KiBugCheckData[3],
             (unsigned long)KiBugCheckData[4]);
    tap_puts(line);
    snprintf(line, sizeof(line), "bugcheck: calling KeBugCheckEx(%08x)\n",
             BUGCHECK_CODE);
    tap_puts(line);
    tap_drain();

    KeBugCheckEx(BUGCHECK_CODE, PARAM1, PARAM2, PARAM3, PARAM4);

    tap_puts("bugcheck: RETURNED -- it must not\n");
    tap_drain();
    return 0;
}
