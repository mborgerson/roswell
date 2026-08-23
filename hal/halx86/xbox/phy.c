/*
 * PROJECT:     nxkrnl -- a free kernel for the original Xbox
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Ethernet PHY init + link-state for the MCPX nForce NIC.
 *
 * The MCPX integrated NIC's on-die PHY is reached through the MAC's MII
 * access registers (NvRegMIIControl / NvRegMIIData) at PCI BAR
 * 0xFEF00000.  PhyInitialize drives a reset + auto-negotiate cycle;
 * PhyGetLinkState returns cached state or re-polls the MII status reg.
 *
 * Clean-room from xemu/xqemu hw/xbox/mcpx/nvnet (authoritative for the
 * MII access bit layout), cromwell etherboot forcedeth, and
 * cxbx-reloaded EmuKrnlPhy.cpp.  MII register names follow IEEE 802.3.
 * The full sequence is verified against xemu, not against real silicon.
 */

#include <ntifs.h>
#include <intrin.h>
#include <xb-debug.h>


/* --- NV MAC registers used for MII access -------------------------------- */

#define NXK_NVNET_MMIO_BASE       0xFEF00000UL

#define NXK_NVREG_ADAPTER_CONTROL 0x188   /* PHY enable / running bits */
#define NXK_NVREG_MII_SPEED       0x18C   /* MII clock divisor + delay */
#define NXK_NVREG_MII_CONTROL     0x190   /* MDIO address + lock + write bit */
#define NXK_NVREG_MII_DATA        0x194   /* MDIO data word */

/* NvRegMIIControl bit fields. */
#define NXK_MII_ADR_LOCK          (1u << 15)   /* HW sets when xfer in flight */
#define NXK_MII_ADR_WRITE         (1u << 10)
#define NXK_MII_ADR_PHY_SHIFT     5
#define NXK_MII_ADR_REG_SHIFT     0
#define NXK_PHY_ADDR              1            /* the on-die PHY address */

/* --- IEEE 802.3 MII register set ---------------------------------------- */
#define MII_CONTROL                0x00
#define   MII_CR_RESET                 0x8000
#define   MII_CR_SPEED_SEL_BIT1        0x2000   /* clause-22 speed bits */
#define   MII_CR_AUTONEG_RESTART       0x0200
#define   MII_CR_SPEED_SEL_BIT0        0x0040
#define MII_STATUS                 0x01
#define   MII_SR_100T4_CAPABLE         0x8000
#define   MII_SR_100TX_HD_CAPABLE      0x2000
#define   MII_SR_10T_HD_CAPABLE        0x0800
#define   MII_SR_100T2_HD_CAPABLE      0x0200
#define   MII_SR_AUTONEG_COMPLETE      0x0020
#define   MII_SR_LINK_UP               0x0004
#define MII_AUTONEG_ADVERTISE      0x04
#define MII_AUTONEG_LINK_PARTNER   0x05
#define   MII_ADV_10T_HD               0x0020
#define   MII_ADV_10T_FD               0x0040
#define   MII_ADV_100T_HD              0x0080
#define   MII_ADV_100T_FD              0x0100
#define   MII_ADV_100T4                0x0200

/* --- Link-state flags returned by PhyGetLinkState ------------------------ */
#define XNET_ETHERNET_LINK_ACTIVE      0x01
#define XNET_ETHERNET_LINK_100MBPS     0x02
#define XNET_ETHERNET_LINK_10MBPS      0x04
#define XNET_ETHERNET_LINK_FULL_DUPLEX 0x08
#define XNET_ETHERNET_LINK_HALF_DUPLEX 0x10

/*
 * Polling timings.  MII transfers take ~200 us on real MCPX silicon and
 * PHY state changes (reset, auto-neg, link up) take 10s-100s of ms.
 *
 * Inner MII waits use KeStallExecutionProcessor (calibrated busy-loop) --
 * xemu clears the lock synchronously so these typically exit on the
 * first sample.
 *
 * Outer PHY-state polls use KeDelayExecutionThread (yields the thread,
 * lets the idle thread HLT) so the emulator advances virtual time
 * efficiently between PIT IRQs.  KeDelayExecutionThread takes 100-ns
 * units; negative = relative.
 */
#define NXK_MII_XFER_STEP_US        50         /* MII poll step          */
#define NXK_MII_XFER_TIMEOUT_US     204800     /* per-transfer cap       */
/* PHY-register polling step: KeStallExecutionProcessor takes microseconds
 * and busy-waits, so it works at any IRQL (titles invoke PhyInitialize at
 * DISPATCH_LEVEL after their nvnet driver raises IRQL).  KeDelay's assert
 * on IRQL <= APC_LEVEL fired every step otherwise. */
#define NXK_PHY_POLL_STEP_US        100        /* 100 us per poll        */
/* Budgets match retail's observed PhyInitialize wall-clock profile.
 * The PHY usually clears BMCR.RESET in microseconds and latches AN_COMP
 * / LINK_UP before our first poll, so these are upper bounds for an
 * unresponsive bus or a slow-negotiating link partner. */
#define NXK_PHY_RESET_STEPS         4000       /* 4000 * 100us = 400 ms  */
#define NXK_PHY_AUTONEG_STEPS       16000      /* 16000 * 100us = 1.6 s  */
#define NXK_PHY_LINKUP_STEPS        10000      /* 10000 * 100us = 1.0 s  */

/*
 * Sticky kernel state across PhyInitialize / PhyGetLinkState invocations.
 * The try-lock keeps two callers from racing the MDIO latch (the same
 * hardware lock bit titles see).
 */
static volatile LONG  NxpPhyLock      = 0;
static          ULONG NxpPhyInitFlag  = 0;
static          ULONG NxpPhyLinkState = 0;

/* --- MMIO helpers -------------------------------------------------------- */

static inline ULONG
NxpNvRegRead(_In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)(NXK_NVNET_MMIO_BASE + Offset));
}

static inline VOID
NxpNvRegWrite(_In_ ULONG Offset, _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(NXK_NVNET_MMIO_BASE + Offset), Value);
}

/*
 * The MII access engine latches MDIO_LOCK while a transfer is in flight.
 * If a prior call left it set (a timeout, or a torn xfer), poke the lock
 * bit and wait for the controller to clear it -- the bit is RW1C-style
 * from the controller's perspective.  Bounded by NXK_MII_XFER_TIMEOUT_US.
 */
static VOID
NxpPhyClearLock(VOID)
{
    LONG remaining;

    NxpNvRegWrite(NXK_NVREG_MII_CONTROL, NXK_MII_ADR_LOCK);
    for (remaining = NXK_MII_XFER_TIMEOUT_US;
         remaining > 0;
         remaining -= NXK_MII_XFER_STEP_US)
    {
        if (!(NxpNvRegRead(NXK_NVREG_MII_CONTROL) & NXK_MII_ADR_LOCK))
            break;
        KeStallExecutionProcessor(NXK_MII_XFER_STEP_US);
    }
}

/*
 * Single MII register read.  Write (PHY << 5) | reg to MIIControl; the
 * controller drives MDIO, raises LOCK while in flight, presents the result
 * in MIIData when LOCK clears.  Bounded poll; FALSE on timeout.
 */
static BOOLEAN
NxpPhyReadReg(_In_ ULONG Reg, _Out_ PULONG Value)
{
    ULONG mdio = 0;
    LONG  remaining;

    if (NxpNvRegRead(NXK_NVREG_MII_CONTROL) & NXK_MII_ADR_LOCK)
        NxpPhyClearLock();

    mdio = (NXK_PHY_ADDR << NXK_MII_ADR_PHY_SHIFT) |
           (Reg          << NXK_MII_ADR_REG_SHIFT);
    NxpNvRegWrite(NXK_NVREG_MII_CONTROL, mdio);

    /* Poll for LOCK to clear (= data available). */
    for (remaining = NXK_MII_XFER_TIMEOUT_US;
         remaining > 0;
         remaining -= NXK_MII_XFER_STEP_US)
    {
        mdio = NxpNvRegRead(NXK_NVREG_MII_CONTROL);
        if (!(mdio & NXK_MII_ADR_LOCK))
            break;
        KeStallExecutionProcessor(NXK_MII_XFER_STEP_US);
    }

    *Value = NxpNvRegRead(NXK_NVREG_MII_DATA);
    return (mdio & NXK_MII_ADR_LOCK) == 0;
}

/*
 * Single MII register write.  Symmetric to NxpPhyReadReg -- stage the data
 * first, then drive MIIControl with the WRITE bit; the controller latches
 * MDIO_LOCK for the duration of the xfer.
 */
static BOOLEAN
NxpPhyWriteReg(_In_ ULONG Reg, _In_ ULONG Value)
{
    ULONG mdio = 0;
    LONG  remaining;

    if (NxpNvRegRead(NXK_NVREG_MII_CONTROL) & NXK_MII_ADR_LOCK)
        NxpPhyClearLock();

    NxpNvRegWrite(NXK_NVREG_MII_DATA, Value);

    mdio = (NXK_PHY_ADDR << NXK_MII_ADR_PHY_SHIFT) |
           (Reg          << NXK_MII_ADR_REG_SHIFT) |
           NXK_MII_ADR_WRITE;
    NxpNvRegWrite(NXK_NVREG_MII_CONTROL, mdio);

    for (remaining = NXK_MII_XFER_TIMEOUT_US;
         remaining > 0;
         remaining -= NXK_MII_XFER_STEP_US)
    {
        mdio = NxpNvRegRead(NXK_NVREG_MII_CONTROL);
        if (!(mdio & NXK_MII_ADR_LOCK))
            break;
        KeStallExecutionProcessor(NXK_MII_XFER_STEP_US);
    }

    return (mdio & NXK_MII_ADR_LOCK) == 0;
}

/*
 * Bounded wait for MII_SR_LINK_UP to assert.  Returns whatever the status
 * reg held when we stopped polling so the caller can OR XNET_ETHERNET_LINK_
 * ACTIVE in iff the bit really came up.
 */
static ULONG
NxpPhyWaitForLinkUp(VOID)
{
    ULONG status = 0;
    LONG  i;

    for (i = 0; i < NXK_PHY_LINKUP_STEPS; i++)
    {
        if (!NxpPhyReadReg(MII_STATUS, &status))
            break;
        if (status & MII_SR_LINK_UP)
            break;
        KeStallExecutionProcessor(NXK_PHY_POLL_STEP_US);
    }
    return status;
}

/*
 * Re-read the PHY's status + advertisement registers and rebuild
 * NxpPhyLinkState.  Returns FALSE if the MII bus is unresponsive.
 */
static BOOLEAN
NxpPhyUpdateLinkState(VOID)
{
    ULONG anar, lpanar, status, state = 0;

    if (!NxpPhyReadReg(MII_AUTONEG_ADVERTISE,    &anar)    ||
        !NxpPhyReadReg(MII_AUTONEG_LINK_PARTNER, &lpanar)  ||
        !NxpPhyReadReg(MII_STATUS,               &status))
    {
        return FALSE;
    }

    /* The link's *negotiated* capability is what both ends advertise. */
    anar &= lpanar;
    if (anar & (MII_ADV_100T_FD | MII_ADV_100T_HD))
        state |= XNET_ETHERNET_LINK_100MBPS;
    else if (anar & (MII_ADV_10T_FD | MII_ADV_10T_HD))
        state |= XNET_ETHERNET_LINK_10MBPS;

    if (anar & (MII_ADV_10T_FD | MII_ADV_100T_FD))
        state |= XNET_ETHERNET_LINK_FULL_DUPLEX;
    else if (anar & (MII_ADV_10T_HD | MII_ADV_100T_HD))
        state |= XNET_ETHERNET_LINK_HALF_DUPLEX;

    if (status & MII_SR_LINK_UP)
        state |= XNET_ETHERNET_LINK_ACTIVE;

    NxpPhyLinkState = state;
    return TRUE;
}

/* --- Exported ordinals --------------------------------------------------- */

/*
 * PhyInitialize(ForceReset, Parameter2).  Called by the title's nvnet
 * driver after it has programmed the MAC + MII clock + adapter-control
 * registers.  Drives the PHY through reset / auto-neg on the first call
 * (or when ForceReset is set); refreshes the cached link state otherwise.
 */
#define NXK_PHY_NETERR_BUSY      ((NTSTATUS)0x800700AA)   /* HRESULT(ERROR_BUSY) */
#define NXK_PHY_NETERR_HARDWARE  ((NTSTATUS)0x801F0001)

NTSTATUS NTAPI
NxPhyInitialize(_In_ BOOLEAN ForceReset, _In_opt_ PVOID Parameter2)
{
    NTSTATUS status = NXK_PHY_NETERR_HARDWARE;
    ULONG    control, mii_status;
    LONG     i;

    UNREFERENCED_PARAMETER(Parameter2);

    /* try-lock: a concurrent PhyInitialize/PhyGetLinkState fails fast. */
    if (InterlockedCompareExchange(&NxpPhyLock, 1, 0) != 0)
        return NXK_PHY_NETERR_BUSY;

    if (ForceReset)
    {
        NxpPhyInitFlag  = 0;
        NxpPhyLinkState = 0;

        /* PHY soft reset: write MII_CR_RESET, then wait for the bit to
         * self-clear.  500 us per step, 1000 steps. */
        if (!NxpPhyWriteReg(MII_CONTROL, MII_CR_RESET))
            goto err;

        control = MII_CR_RESET;
        for (i = 0; i < NXK_PHY_RESET_STEPS; i++)
        {
            if (!NxpPhyReadReg(MII_CONTROL, &control))
                goto err;
            if (!(control & MII_CR_RESET))
                break;
            KeStallExecutionProcessor(NXK_PHY_POLL_STEP_US);
        }
        if (control & MII_CR_RESET)
            goto err;
    }
    else if (NxpPhyInitFlag)
    {
        /* Already initialized and no force-reset; just refresh cache. */
        NxpPhyUpdateLinkState();
        status = STATUS_SUCCESS;
        goto exit;
    }

    /* Wait for auto-negotiation to complete. */
    mii_status = 0;
    for (i = 0; i < NXK_PHY_AUTONEG_STEPS; i++)
    {
        if (!NxpPhyReadReg(MII_STATUS, &mii_status))
            goto err;
        if (mii_status & MII_SR_AUTONEG_COMPLETE)
            break;
        KeStallExecutionProcessor(NXK_PHY_POLL_STEP_US);
    }

    if (!NxpPhyReadReg(MII_CONTROL, &control))
        goto err;

    if (control & MII_CR_AUTONEG_RESTART)
    {
        /*
         * Reset latched "restart auto-neg".  Pick the fastest mode the PHY
         * advertises, force MII control to that speed, then wait for link.
         * HD is set unconditionally; the driver flips to FD if the
         * negotiated state says so.
         */
        if (mii_status & (MII_SR_100T4_CAPABLE | MII_SR_100TX_HD_CAPABLE |
                          MII_SR_100T2_HD_CAPABLE))
        {
            control |= MII_CR_SPEED_SEL_BIT1;
            control &= ~MII_CR_SPEED_SEL_BIT0;
            NxpPhyLinkState |= XNET_ETHERNET_LINK_100MBPS;
        }
        else if (mii_status & MII_SR_10T_HD_CAPABLE)
        {
            control &= ~MII_CR_SPEED_SEL_BIT1;
            control |= MII_CR_SPEED_SEL_BIT0;
            NxpPhyLinkState |= XNET_ETHERNET_LINK_10MBPS;
        }
        else
        {
            goto err;
        }

        NxpPhyLinkState |= XNET_ETHERNET_LINK_HALF_DUPLEX;
        NxpPhyWriteReg(MII_CONTROL, control);
        mii_status = NxpPhyWaitForLinkUp();

        if (mii_status & MII_SR_LINK_UP)
            NxpPhyLinkState |= XNET_ETHERNET_LINK_ACTIVE;
    }
    else
    {
        /* Auto-neg already done; just sample link state. */
        NxpPhyWaitForLinkUp();
        if (!NxpPhyUpdateLinkState())
            goto err;
    }

    NxpPhyInitFlag = 1;
    status = STATUS_SUCCESS;

exit:
    NxpPhyLock = 0;
    return status;

err:
    XbDbg("PhyInitialize failed (MII bus unresponsive?)\n");
    goto exit;
}

/*
 * PhyGetLinkState(Mode).  Mode==FALSE returns the cached value; otherwise
 * re-poll the PHY.  Lock contention silently falls through to the cache
 * (matches retail behavior).
 */
ULONG NTAPI
NxPhyGetLinkState(_In_ BOOLEAN Mode)
{
    if ((NxpPhyLinkState == 0 || Mode) &&
        InterlockedCompareExchange(&NxpPhyLock, 1, 0) == 0)
    {
        NxpPhyUpdateLinkState();
        NxpPhyLock = 0;
    }
    return NxpPhyLinkState;
}

/* EOF */
