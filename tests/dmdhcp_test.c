#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmdhcp.h"

static dmdhcp_t g_handle = NULL;

void dmod_test_setup(void)
{
    g_handle = dmdhcp_create();
}

void dmod_test_teardown(void)
{
    dmdhcp_destroy(g_handle);
    g_handle = NULL;
}

DMOD_TEST_STEP(dmdhcp_create)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_handle);
}

DMOD_TEST_STEP(dmdhcp_is_valid)
{
    DMOD_TEST_EXPECT_TRUE(dmdhcp_is_valid(g_handle));
}

DMOD_TEST_STEP(dmdhcp_destroy_null)
{
    /* Destroying NULL must not crash. */
    dmdhcp_destroy(NULL);
}
