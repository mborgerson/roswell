/*
 * chainload test -- loader XBE.
 *
 * Calls XLaunchXBE on the target XBE on the same disc.  XLaunchXBE fills
 * LaunchDataPage with the target path, then HalReturnToFirmware's the
 * HalQuickRebootRoutine -- our kernel's XeHalReturnToFirmware detects the
 * chainload and signals the supervisor to load the target.
 *
 * Expected serial output, end-to-end:
 *   chainload-loader: hello, about to chainload \Device\CdRom0\target.xbe
 *   chainload requested -> \Device\CdRom0\target.xbe
 *   opened \Device\CdRom0\target.xbe
 *   chainload-target: chainloaded successfully
 */
#include <xboxkrnl/xboxkrnl.h>
#include <hal/xbox.h>

int main(void)
{
    const char *target = "\\Device\\CdRom0\\target.xbe";
    DbgPrint("chainload-loader: hello, about to chainload %s\n", target);
    XLaunchXBE(target);
    /* XLaunchXBE doesn't return; if it does, something went wrong. */
    DbgPrint("chainload-loader: XLaunchXBE returned -- chainload failed\n");
    return 1;
}
