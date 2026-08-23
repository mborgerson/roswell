/* chainload test -- target XBE.  Loaded in place of the loader.
 *
 * Halts explicitly via HalReturnToFirmware(HalHaltRoutine) instead of
 * returning from main, which nxdk crt0 would turn into a HalQuickReboot
 * (SMC reset) -- that would re-boot the box and (because the disc's
 * default.xbe is the loader) start the chainload over again. */
#include <xboxkrnl/xboxkrnl.h>
#include <hal/xbox.h>

int main(void)
{
    DbgPrint("chainload-target: chainloaded successfully\n");
    HalReturnToFirmware(HalHaltRoutine);
    return 0;
}
