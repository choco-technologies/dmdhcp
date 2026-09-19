/**
 * @file dhcpc_test.c
 * @brief Test steps for dhcpc
 *
 * Drives the actual `dhcpc` module through Dmod_RunModule("dhcpc", argc,
 * argv) (loads it - pulling in dmdhcp as its own linked dependency - runs
 * its main(), unloads it again) rather than depending on dhcpc as a linked
 * module: an Application-type module can't be loaded as another module's
 * dependency (only a Library-type module can), same reasoning
 * dmicmp/tools/ping/tests/ping_test.c documents for itself.
 *
 * A "/dev/null"-backed dmnetif fixture stands in for a real interface:
 * dhcpc only needs to resolve it by name and hand it to dmdhcp_start(),
 * which itself only arms a timer and returns - no real DISCOVER needs to
 * go anywhere for these steps to prove dhcpc's own argument handling and
 * interface lookup.
 */
#include "dmod_test.h"
#include "dmnetif.h"

#define TEST_DEVICE_PATH "/dev/null"

static dmnetif_iface_t g_iface = NULL;

void dmod_test_setup(void)
{
    g_iface = dmnetif_register("test0", TEST_DEVICE_PATH);
}

void dmod_test_teardown(void)
{
    dmnetif_unregister(g_iface);
    g_iface = NULL;
}

DMOD_TEST_STEP(no_args_fails)
{
    char* argv[] = { "dhcpc" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("dhcpc", 1, argv), 0);
}

DMOD_TEST_STEP(unknown_interface_fails)
{
    char* argv[] = { "dhcpc", "no-such-iface" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("dhcpc", 2, argv), 0);
}

DMOD_TEST_STEP(run_module_unloads_dhcpc_afterwards)
{
    char* argv[] = { "dhcpc", "no-such-iface" };
    Dmod_RunModule("dhcpc", 2, argv);
    DMOD_TEST_EXPECT_FALSE(Dmod_IsModuleLoaded("dhcpc"));
}

DMOD_TEST_STEP(valid_interface_starts_a_lease)
{
    char* argv[] = { "dhcpc", "test0" };
    DMOD_TEST_EXPECT_EQ(Dmod_RunModule("dhcpc", 2, argv), 0);
}
