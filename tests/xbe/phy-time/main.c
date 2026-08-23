/*
 * Measure how long PhyInitialize takes, and verify clock granularity by
 * also timing a no-op pair of KeQuerySystemTime calls.  Each line of
 * serial output is one measurement in milliseconds.
 */
#include <xboxkrnl/xboxkrnl.h>
#include <hal/xbox.h>

static ULONG
DeltaMs(LARGE_INTEGER t0, LARGE_INTEGER t1)
{
    return (ULONG)((t1.QuadPart - t0.QuadPart) / 10000);
}

static void
DumpClockBaseline(void)
{
    LARGE_INTEGER t0, t1;
    int i;

    /* 5 consecutive system-time samples -- inter-sample delta exposes the
     * clock-tick granularity. */
    KeQuerySystemTime(&t0);
    for (i = 0; i < 5; i++) {
        KeQuerySystemTime(&t1);
        DbgPrint("phy-time: baseline KeQuerySystemTime delta #%d = %lu ms\n",
                 i, DeltaMs(t0, t1));
        t0 = t1;
    }
}

static ULONG
TimePhyInit(BOOLEAN forceReset)
{
    LARGE_INTEGER t0, t1;
    NTSTATUS s;

    KeQuerySystemTime(&t0);
    s = PhyInitialize(forceReset, NULL);
    KeQuerySystemTime(&t1);
    DbgPrint("phy-time: PhyInitialize(%s) status=%08lx\n",
             forceReset ? "TRUE" : "FALSE", (ULONG)s);
    return DeltaMs(t0, t1);
}

int main(void)
{
    DumpClockBaseline();

    ULONG ms_cold1 = TimePhyInit(TRUE);
    DbgPrint("phy-time: cold(1)  PhyInitialize(TRUE) -> %lu ms\n", ms_cold1);

    ULONG ms_cached = TimePhyInit(FALSE);
    DbgPrint("phy-time: cached   PhyInitialize(FALSE) -> %lu ms\n", ms_cached);

    ULONG ms_cold2 = TimePhyInit(TRUE);
    DbgPrint("phy-time: cold(2)  PhyInitialize(TRUE) -> %lu ms\n", ms_cold2);

    ULONG link = PhyGetLinkState(TRUE);
    DbgPrint("phy-time: PhyGetLinkState=%08lx (active=%c speed=%s duplex=%s)\n",
             link,
             (link & 0x01) ? 'Y' : 'N',
             (link & 0x02) ? "100M" : (link & 0x04) ? "10M" : "?",
             (link & 0x08) ? "FD"   : (link & 0x10) ? "HD"  : "?");

    HalReturnToFirmware(HalHaltRoutine);
    return 0;
}
