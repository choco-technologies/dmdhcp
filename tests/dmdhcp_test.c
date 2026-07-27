/**
 * @file dmdhcp_test.c
 * @brief Test steps for dmdhcp
 *
 * Codec-only for now (dmdhcp_build_message()/_parse_message() and the
 * options TLV codec) - these need no network at all. State-machine tests
 * (feeding a hand-built OFFER/ACK/NAK frame through the real dmnetbridge
 * -> dmip -> dmudp -> dmdhcp dispatch chain, the same "/dev/null"-backed
 * dmnetif fixture dmtcp_test.c/dmudp_test.c use) are deferred until
 * dmudp_send_on_iface()/dmip_v4_send_on_iface() exist upstream - dmdhcp's
 * broadcast sends (dmdhcp_start()'s own DISCOVER included) depend on them,
 * see include/dmdhcp.h's top comment.
 */
#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmdhcp.h"
#include <string.h>
#include <errno.h>

/* ============================================================================
 *                      Fixed header build/parse
 * ========================================================================== */

DMOD_TEST_STEP(build_message_rejects_bad_arguments)
{
    uint8_t buffer[DMDHCP_FIXED_HEADER_LEN + 4];
    dmdhcp_message_t message = { 0 };

    DMOD_TEST_EXPECT_EQ(dmdhcp_build_message(NULL, sizeof(buffer), &message), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdhcp_build_message(buffer, sizeof(buffer), NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdhcp_build_message(buffer, DMDHCP_FIXED_HEADER_LEN + 3, &message), -EINVAL);
}

DMOD_TEST_STEP(parse_message_rejects_bad_arguments)
{
    uint8_t buffer[DMDHCP_FIXED_HEADER_LEN + 4] = { 0 };
    dmdhcp_message_t message = { 0 };
    size_t options_offset = 0;

    DMOD_TEST_EXPECT_EQ(dmdhcp_parse_message(NULL, sizeof(buffer), &message, &options_offset), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdhcp_parse_message(buffer, sizeof(buffer), NULL, &options_offset), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdhcp_parse_message(buffer, sizeof(buffer), &message, NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdhcp_parse_message(buffer, DMDHCP_FIXED_HEADER_LEN + 3, &message, &options_offset), -EINVAL);
}

DMOD_TEST_STEP(parse_message_rejects_bad_magic_cookie)
{
    uint8_t buffer[DMDHCP_FIXED_HEADER_LEN + 4] = { 0 };
    dmdhcp_message_t message = { 0 };
    size_t options_offset = 0;

    /* Cookie left as all-zero, not DMDHCP_MAGIC_COOKIE */
    DMOD_TEST_EXPECT_EQ(dmdhcp_parse_message(buffer, sizeof(buffer), &message, &options_offset), -EPROTO);
}

DMOD_TEST_STEP(build_and_parse_message_round_trip)
{
    dmdhcp_message_t message = { 0 };
    message.op = dmdhcp_op_bootrequest;
    message.htype = 1;
    message.hlen = 6;
    message.hops = 0;
    message.xid = 0xAABBCCDDu;
    message.secs = 12;
    message.flags = DMDHCP_FLAG_BROADCAST;
    message.ciaddr = (dmroute_addr_t){ .family = dmroute_family_none };
    message.yiaddr = (dmroute_addr_t){ .family = dmroute_family_v4, .addr.v4 = { 192, 168, 1, 42 } };
    message.siaddr = (dmroute_addr_t){ .family = dmroute_family_none };
    message.giaddr = (dmroute_addr_t){ .family = dmroute_family_none };
    memcpy(message.chaddr, (uint8_t[DMDHCP_CHADDR_LEN]){ 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }, DMDHCP_CHADDR_LEN);

    uint8_t buffer[DMDHCP_FIXED_HEADER_LEN + 4];
    DMOD_TEST_EXPECT_EQ(dmdhcp_build_message(buffer, sizeof(buffer), &message), 0);

    dmdhcp_message_t parsed = { 0 };
    size_t options_offset = 0;
    DMOD_TEST_EXPECT_EQ(dmdhcp_parse_message(buffer, sizeof(buffer), &parsed, &options_offset), 0);
    DMOD_TEST_EXPECT_EQ(options_offset, (size_t)(DMDHCP_FIXED_HEADER_LEN + 4));
    DMOD_TEST_EXPECT_EQ((int)parsed.op, (int)dmdhcp_op_bootrequest);
    DMOD_TEST_EXPECT_EQ(parsed.xid, (uint32_t)0xAABBCCDDu);
    DMOD_TEST_EXPECT_EQ(parsed.secs, (uint16_t)12);
    DMOD_TEST_EXPECT_EQ(parsed.flags, (uint16_t)DMDHCP_FLAG_BROADCAST);
    DMOD_TEST_EXPECT_EQ((int)parsed.yiaddr.family, (int)dmroute_family_v4);
    DMOD_TEST_EXPECT_EQ(parsed.yiaddr.addr.v4[0], (uint8_t)192);
    DMOD_TEST_EXPECT_EQ(parsed.yiaddr.addr.v4[3], (uint8_t)42);
    /* ciaddr was built as dmroute_family_none - parsing always reports v4
     * (an all-zero address on the wire), see dmdhcp_wire.c's read_addr() */
    DMOD_TEST_EXPECT_EQ((int)parsed.ciaddr.family, (int)dmroute_family_v4);
    DMOD_TEST_EXPECT_EQ(parsed.ciaddr.addr.v4[0], (uint8_t)0);
}

DMOD_TEST_STEP(build_message_rejects_ipv6_address_field)
{
    dmdhcp_message_t message = { 0 };
    message.ciaddr = (dmroute_addr_t){ .family = dmroute_family_v6 };

    uint8_t buffer[DMDHCP_FIXED_HEADER_LEN + 4];
    DMOD_TEST_EXPECT_EQ(dmdhcp_build_message(buffer, sizeof(buffer), &message), -EINVAL);
}

/* ============================================================================
 *                      Options codec
 * ========================================================================== */

DMOD_TEST_STEP(options_write_and_find_round_trip)
{
    uint8_t msg_type = (uint8_t)dmdhcp_msg_offer;
    uint8_t requested_ip[4] = { 10, 0, 0, 5 };
    dmdhcp_option_t options[2] = {
        { .code = DMDHCP_OPT_MSG_TYPE, .length = 1, .data = &msg_type },
        { .code = DMDHCP_OPT_REQUESTED_IP, .length = 4, .data = requested_ip },
    };

    uint8_t buffer[32];
    size_t out_len = 0;
    DMOD_TEST_EXPECT_EQ(dmdhcp_options_write(buffer, sizeof(buffer), options, 2, &out_len), 0);
    DMOD_TEST_EXPECT_EQ(out_len, (size_t)(3 + 6 + 1)); /* [code+len+1 data byte] + [code+len+4 data bytes] + END */
    DMOD_TEST_EXPECT_EQ(buffer[out_len - 1], (uint8_t)DMDHCP_OPT_END);

    dmdhcp_option_t found = { 0 };
    DMOD_TEST_EXPECT_EQ(dmdhcp_options_find(buffer, out_len, DMDHCP_OPT_REQUESTED_IP, &found), 0);
    DMOD_TEST_EXPECT_EQ(found.length, (uint8_t)4);
    DMOD_TEST_EXPECT_EQ(found.data[0], (uint8_t)10);
    DMOD_TEST_EXPECT_EQ(found.data[3], (uint8_t)5);

    DMOD_TEST_EXPECT_EQ(dmdhcp_options_find(buffer, out_len, DMDHCP_OPT_SERVER_ID, &found), -ENOENT);
}

DMOD_TEST_STEP(options_write_rejects_buffer_too_small)
{
    uint8_t msg_type = (uint8_t)dmdhcp_msg_discover;
    dmdhcp_option_t options[1] = { { .code = DMDHCP_OPT_MSG_TYPE, .length = 1, .data = &msg_type } };

    uint8_t buffer[2]; /* not even enough for [code][len][data] */
    size_t out_len = 0;
    DMOD_TEST_EXPECT_EQ(dmdhcp_options_write(buffer, sizeof(buffer), options, 1, &out_len), -ENOSPC);
}

DMOD_TEST_STEP(options_find_skips_pad_and_honors_end)
{
    uint8_t buffer[] = { DMDHCP_OPT_PAD, DMDHCP_OPT_PAD, 53, 1, 5, DMDHCP_OPT_END, 99, 1, 1 };
    dmdhcp_option_t found = { 0 };

    DMOD_TEST_EXPECT_EQ(dmdhcp_options_find(buffer, sizeof(buffer), DMDHCP_OPT_MSG_TYPE, &found), 0);
    DMOD_TEST_EXPECT_EQ(found.data[0], (uint8_t)5);

    /* code 99 sits after DMDHCP_OPT_END - must not be found */
    DMOD_TEST_EXPECT_EQ(dmdhcp_options_find(buffer, sizeof(buffer), 99, &found), -ENOENT);
}

DMOD_TEST_STEP(options_find_rejects_truncated_option)
{
    uint8_t buffer[] = { DMDHCP_OPT_ROUTER, 4, 1, 2 }; /* claims length 4, only 2 bytes of data follow */
    dmdhcp_option_t found = { 0 };

    DMOD_TEST_EXPECT_EQ(dmdhcp_options_find(buffer, sizeof(buffer), DMDHCP_OPT_ROUTER, &found), -EPROTO);
}

DMOD_TEST_STEP(option_get_u32_round_trip)
{
    uint8_t data[4] = { 0x00, 0x01, 0x51, 0x80 }; /* 86400 (1 day), big-endian */
    dmdhcp_option_t opt = { .code = DMDHCP_OPT_LEASE_TIME, .length = 4, .data = data };

    uint32_t value = 0;
    DMOD_TEST_EXPECT_EQ(dmdhcp_option_get_u32(&opt, &value), 0);
    DMOD_TEST_EXPECT_EQ(value, (uint32_t)86400);

    opt.length = 3; /* wrong size */
    DMOD_TEST_EXPECT_EQ(dmdhcp_option_get_u32(&opt, &value), -EINVAL);
}

DMOD_TEST_STEP(option_get_addr_round_trip)
{
    uint8_t data[4] = { 255, 255, 255, 0 };
    dmdhcp_option_t opt = { .code = DMDHCP_OPT_SUBNET_MASK, .length = 4, .data = data };

    dmroute_addr_t addr = { 0 };
    DMOD_TEST_EXPECT_EQ(dmdhcp_option_get_addr(&opt, &addr), 0);
    DMOD_TEST_EXPECT_EQ((int)addr.family, (int)dmroute_family_v4);
    DMOD_TEST_EXPECT_EQ(addr.addr.v4[2], (uint8_t)255);
    DMOD_TEST_EXPECT_EQ(addr.addr.v4[3], (uint8_t)0);
}

DMOD_TEST_STEP(option_multi_address_accessors)
{
    uint8_t data[8] = { 8, 8, 8, 8, 1, 1, 1, 1 }; /* two DNS servers */
    dmdhcp_option_t opt = { .code = DMDHCP_OPT_DNS_SERVER, .length = 8, .data = data };

    DMOD_TEST_EXPECT_EQ(dmdhcp_option_addr_count(&opt), (size_t)2);

    dmroute_addr_t addr = { 0 };
    DMOD_TEST_EXPECT_EQ(dmdhcp_option_get_addr_at(&opt, 0, &addr), 0);
    DMOD_TEST_EXPECT_EQ(addr.addr.v4[0], (uint8_t)8);

    DMOD_TEST_EXPECT_EQ(dmdhcp_option_get_addr_at(&opt, 1, &addr), 0);
    DMOD_TEST_EXPECT_EQ(addr.addr.v4[0], (uint8_t)1);

    DMOD_TEST_EXPECT_EQ(dmdhcp_option_get_addr_at(&opt, 2, &addr), -EINVAL);
}

/* ============================================================================
 *                      dmdhcp_start() argument validation
 * ========================================================================== */

DMOD_TEST_STEP(start_rejects_null_iface)
{
    DMOD_TEST_EXPECT_NULL(dmdhcp_start(NULL, NULL, NULL, NULL));
}
