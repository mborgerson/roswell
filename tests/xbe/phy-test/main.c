/*
 * Drive the Ethernet PHY ordinals end-to-end.
 *
 * Calls the kernel's PhyInitialize / PhyGetLinkState and prints the
 * resulting link-state flags.  A successful run reports
 * XNET_ETHERNET_LINK_ACTIVE | XNET_ETHERNET_LINK_100MBPS | _FULL_DUPLEX
 * (xemu's emulated PHY auto-negotiates to 100 Mbps FD with link up).
 *
 * Note on the NIC prelude: nxdk's nvnetdrv sets up NvRegAdapterControl +
 * NvRegMIISpeed in title code right before calling PhyInitialize.  That
 * works on retail Xbox because everything runs ring 0; under nxkrnl the
 * title is still in ring 3 and the NV MMIO PDE is supervisor-only, so we
 * skip the prelude.  We rely on early boot having left the controller in
 * a usable state -- xemu's MII engine answers reads/writes regardless of
 * AdapterControl.RUNNING (see hw/xbox/mcpx/nvnet/nvnet.c phy_reg_read).
 */
#include <xboxkrnl/xboxkrnl.h>
#include <stdint.h>

int main(void)
{
    NTSTATUS status;
    DWORD link;

    DbgPrint("phy-test: calling PhyInitialize(FALSE, NULL)\n");
    status = PhyInitialize(FALSE, NULL);
    DbgPrint("phy-test: PhyInitialize -> 0x%08x\n", (unsigned)status);

    link = PhyGetLinkState(TRUE);
    DbgPrint("phy-test: PhyGetLinkState(TRUE) -> 0x%08x\n", (unsigned)link);
    if (link & 0x01) DbgPrint("phy-test:   XNET_ETHERNET_LINK_ACTIVE\n");
    if (link & 0x02) DbgPrint("phy-test:   XNET_ETHERNET_LINK_100MBPS\n");
    if (link & 0x04) DbgPrint("phy-test:   XNET_ETHERNET_LINK_10MBPS\n");
    if (link & 0x08) DbgPrint("phy-test:   XNET_ETHERNET_LINK_FULL_DUPLEX\n");
    if (link & 0x10) DbgPrint("phy-test:   XNET_ETHERNET_LINK_HALF_DUPLEX\n");

    /* A second PhyInitialize without forceReset should hit the cached fast
     * path and return STATUS_SUCCESS immediately. */
    DbgPrint("phy-test: calling PhyInitialize(FALSE, NULL) again (cached)\n");
    status = PhyInitialize(FALSE, NULL);
    DbgPrint("phy-test: PhyInitialize(2nd) -> 0x%08x\n", (unsigned)status);

    /* Cached PhyGetLinkState (Mode=FALSE) -- should not re-poll. */
    link = PhyGetLinkState(FALSE);
    DbgPrint("phy-test: PhyGetLinkState(FALSE) -> 0x%08x (cached)\n",
             (unsigned)link);

    DbgPrint("phy-test: done\n");
    return 0;
}
