/**
 * @file dhcpc.c
 * @brief dhcpc - starts a dmdhcp lease on one interface
 *
 * One instance per interface, the same shape dmnet's networkd uses for its
 * own per-interface unit (see dmnet/services/networkd/src/networkd.c): a
 * `configs/dhcp@.ini` unit template is instantiated once per interface that
 * should acquire an address by DHCP, and dhcpc is the whole reason that
 * template exists.
 *
 * Unlike networkd, dhcpc has nothing left to do once it has made this call:
 * dmdhcp keeps no thread of its own - dmdhcp_start() only registers the
 * lease and arms the first DISCOVER/timer, and everything after that runs
 * on dmdhcp's own dmudp_bind() callback (fed by whichever process is
 * pumping this interface's rx - typically the matching networkd@<iface>
 * instance) or on dmdhcp's dmosi_timer_t callbacks. So dhcpc calls
 * dmdhcp_start() once and exits - see configs/dhcp@.ini's `type=oneshot`.
 *
 * The interface's name arrives as argv[1], from the template's `%i`.
 */
#include "dmod.h"
#include "dmnetif.h"
#include "dmdhcp.h"
#include <errno.h>

/**
 * @brief Main function of the application
 *
 * @param argc Argument count
 * @param argv argv[1] is the interface name to acquire a lease on (the unit
 *             template's %i)
 *
 * @return 0 once dmdhcp_start() has been called successfully, -EINVAL
 *         without an interface name, -ENODEV if no interface by that name
 *         is registered, -EALREADY if the interface already has an active
 *         lease
 */
int main(int argc, char *argv[])
{
    if (argc < 2 || argv[1] == NULL || argv[1][0] == '\0')
    {
        DMOD_LOG_ERROR("dhcpc: no interface name given\n");
        Dmod_Printf("Usage: dhcpc <interface>\n");
        Dmod_Printf("Started per interface from dhcp@.ini - see docs/service.md\n");
        return -EINVAL;
    }

    const char* name = argv[1];

    dmnetif_iface_t iface = dmnetif_find_by_name(name);
    if (iface == NULL)
    {
        DMOD_LOG_ERROR("dhcpc: no interface named '%s' is registered\n", name);
        return -ENODEV;
    }

    dmdhcp_lease_t lease = dmdhcp_start(iface, NULL, NULL, NULL);
    if (lease == NULL)
    {
        DMOD_LOG_ERROR("dhcpc: could not start a lease on '%s' (already active?)\n", name);
        return -EALREADY;
    }

    DMOD_LOG_INFO("dhcpc: acquiring a DHCP lease on '%s'\n", name);
    return 0;
}
