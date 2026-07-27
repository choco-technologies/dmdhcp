/**
 * @file dmdhcp_test.c
 * @brief Test steps for dmdhcp
 *
 * Two kinds of steps:
 *
 *  - Codec-only (fixed header, options TLV) - no network at all.
 *  - State-machine steps: same infrastructure as dmtcp_test.c/dmudp_test.c
 *    - a "/dev/null"-backed dmnetif fixture interface, and feed_frame()
 *    driving a hand-built, correctly-checksummed IPv4/UDP/DHCP packet
 *    straight into dmnetbridge's real packet_received DIF implementation
 *    via Dmod_GetNextDifModule()/Dmod_GetDifFunction() - the same
 *    discovery dmnetbridge_handle_netif_rx() itself uses - so a message
 *    flows through the real dmnetbridge -> dmip -> dmudp -> dmdhcp
 *    dispatch chain, not a mock. DHCP payload bytes are built with
 *    dmdhcp's own dmdhcp_build_message()/_options_write() rather than
 *    hand-assembled byte-by-byte, so these tests also exercise the codec's
 *    build side together with dmdhcp's own parse side.
 *
 *    feed_frame() never touches dmarp (it bypasses
 *    dmnetbridge_handle_netif_rx() itself, which is the only thing that
 *    calls dmarp_note_frame()) - so the RFC 2131 §4.4.1 conflict probe is
 *    driven by seeding/not seeding dmarp's cache directly
 *    (dmarp_cache_insert()) rather than by feeding a real ARP reply frame,
 *    the same "hand-seeded ARP cache hit" approach dmudp_test.c documents
 *    for its own send-path tests.
 *
 *    dmdhcp_get_xid() (a public accessor, not a test-only hook) is what
 *    makes any of this possible without predicting dmosi_rand32()'s
 *    output: read it right after dmdhcp_start()/each restart, and use it
 *    to address the test's own OFFER/ACK/NAK frames at whichever lease is
 *    under test.
 *
 * Constants mirroring src/dmdhcp_internal.h's private ones (not linked
 * directly - see dmtcp_test.c's own note about why: avoiding this test
 * target needing dmlist/dmosi link just for a couple of constants, when it
 * already needs dmosi for dmosi_thread_sleep()... here we already link
 * dmosi anyway (see CMakeLists.txt), but dmdhcp_internal.h also declares
 * the private struct dmdhcp_lease which pulls in dmlist.h - simpler to
 * just mirror the few values this file needs by hand).
 */
#define DMOD_ENABLE_REGISTRATION ON
#define ENABLE_DIF_REGISTRATIONS ON
#include "dmod_test.h"
#include "dmdhcp.h"
#include "dmudp.h"
#include "dmip.h"
#include "dmroute.h"
#include "dmarp.h"
#include "dmnetbridge.h"
#include "dmosi.h"
#include <string.h>
#include <errno.h>

/* Mirrors src/dmdhcp_internal.h - see this file's top comment */
#define TEST_PROBE_HANDOFF_MS 1u
#define TEST_ARP_PROBE_TIMEOUT_MS 1000u

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

/* ============================================================================
 *                      State-machine fixture
 * ========================================================================== */

static dmroute_addr_t make_v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    dmroute_addr_t addr = { 0 };
    addr.family = dmroute_family_v4;
    addr.addr.v4[0] = a;
    addr.addr.v4[1] = b;
    addr.addr.v4[2] = c;
    addr.addr.v4[3] = d;
    return addr;
}

static void write_u16_be(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFu);
}

static void write_u32_be(uint8_t* p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)(value & 0xFFu);
}

#define TEST_ETH_HEADER_LEN 14u
#define TEST_ETHERTYPE_IPV4 0x0800u
#define TEST_V4_PSEUDO_HEADER_LEN 12u
#define TEST_MAX_DHCP_LEN 320u
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

/**
 * @brief Wrap a complete IP packet in a minimal Ethernet frame and
 *        broadcast it to every packet_received DIF implementor (dmip's
 *        own, in practice) - the same discovery
 *        dmnetbridge_handle_netif_rx() itself uses. Does NOT go through
 *        dmarp_note_frame() (only the real pump function does that) - see
 *        this file's top comment.
 */
static void feed_frame(dmnetif_iface_t iface, uint16_t ethertype, const uint8_t* packet, size_t packet_len)
{
    size_t frame_len = TEST_ETH_HEADER_LEN + packet_len;
    uint8_t* frame = Dmod_Malloc(frame_len);
    memset(frame, 0, TEST_ETH_HEADER_LEN);
    write_u16_be(&frame[12], ethertype);
    memcpy(frame + TEST_ETH_HEADER_LEN, packet, packet_len);

    Dmod_Context_t* implementor = NULL;
    while ((implementor = Dmod_GetNextDifModule(dmod_dmnetbridge_packet_received_sig, implementor)) != NULL)
    {
        dmod_dmnetbridge_packet_received_t fn =
            (dmod_dmnetbridge_packet_received_t)Dmod_GetDifFunction(implementor, dmod_dmnetbridge_packet_received_sig);
        if (fn != NULL)
        {
            fn(iface, frame, frame_len);
        }
    }

    Dmod_Free(frame);
}

/**
 * @brief Build a DHCP message using dmdhcp's own codec (dmdhcp_build_message()/
 *        _options_write()) - any of `server_id`/`netmask`/`router`/`dns`
 *        may be NULL to omit that option, `lease_time_sec` == 0 to omit
 *        options 51/58/59
 */
static size_t build_dhcp_payload(uint8_t* out, size_t out_cap, dmdhcp_msg_type_t msg_type, uint32_t xid,
                                  const dmroute_addr_t* yiaddr, const dmroute_addr_t* server_id,
                                  const dmroute_addr_t* netmask, const dmroute_addr_t* router,
                                  const dmroute_addr_t* dns, uint32_t lease_time_sec)
{
    dmdhcp_message_t header = { 0 };
    header.op = dmdhcp_op_bootreply;
    header.htype = 1;
    header.hlen = 6;
    header.xid = xid;
    header.flags = DMDHCP_FLAG_BROADCAST;
    if (yiaddr != NULL)
    {
        header.yiaddr = *yiaddr;
    }

    dmdhcp_build_message(out, out_cap, &header);
    size_t options_offset = DMDHCP_FIXED_HEADER_LEN + 4u;

    uint8_t msg_type_byte = (uint8_t)msg_type;
    uint8_t lease_bytes[4];
    write_u32_be(lease_bytes, lease_time_sec);

    dmdhcp_option_t options[6];
    size_t n = 0;
    options[n++] = (dmdhcp_option_t){ .code = DMDHCP_OPT_MSG_TYPE, .length = 1, .data = &msg_type_byte };
    if (server_id != NULL) options[n++] = (dmdhcp_option_t){ .code = DMDHCP_OPT_SERVER_ID, .length = 4, .data = server_id->addr.v4 };
    if (netmask != NULL) options[n++] = (dmdhcp_option_t){ .code = DMDHCP_OPT_SUBNET_MASK, .length = 4, .data = netmask->addr.v4 };
    if (router != NULL) options[n++] = (dmdhcp_option_t){ .code = DMDHCP_OPT_ROUTER, .length = 4, .data = router->addr.v4 };
    if (dns != NULL) options[n++] = (dmdhcp_option_t){ .code = DMDHCP_OPT_DNS_SERVER, .length = 4, .data = dns->addr.v4 };
    if (lease_time_sec > 0) options[n++] = (dmdhcp_option_t){ .code = DMDHCP_OPT_LEASE_TIME, .length = 4, .data = lease_bytes };

    size_t options_len = 0;
    dmdhcp_options_write(&out[options_offset], out_cap - options_offset, options, n, &options_len);
    return options_offset + options_len;
}

/**
 * @brief Wrap a DHCP payload in a correctly-checksummed UDP-over-IPv4
 *        datagram (server port 67 -> client port 68) and feed it in
 */
static void feed_dhcp_message(dmnetif_iface_t iface, dmdhcp_msg_type_t msg_type, uint32_t xid, const dmroute_addr_t* server_ip,
                               const dmroute_addr_t* yiaddr, const dmroute_addr_t* server_id, const dmroute_addr_t* netmask,
                               const dmroute_addr_t* router, const dmroute_addr_t* dns, uint32_t lease_time_sec)
{
    uint8_t dhcp[TEST_MAX_DHCP_LEN];
    size_t dhcp_len = build_dhcp_payload(dhcp, sizeof(dhcp), msg_type, xid, yiaddr, server_id, netmask, router, dns, lease_time_sec);

    uint8_t udp[DMUDP_HEADER_LEN + TEST_MAX_DHCP_LEN];
    size_t udp_len = DMUDP_HEADER_LEN + dhcp_len;
    dmudp_header_t udp_header = { .src_port = DMDHCP_SERVER_PORT, .dst_port = DMDHCP_CLIENT_PORT, .length = (uint16_t)udp_len };
    dmudp_build_header(udp, udp_len, &udp_header);
    memcpy(udp + DMUDP_HEADER_LEN, dhcp, dhcp_len);

    dmroute_addr_t broadcast = make_v4(255, 255, 255, 255);
    const dmroute_addr_t* dst = &broadcast; /* client has no address yet - server broadcasts too, doesn't matter for dispatch */

    uint8_t pseudo_and_segment[TEST_V4_PSEUDO_HEADER_LEN + DMUDP_HEADER_LEN + TEST_MAX_DHCP_LEN];
    memcpy(&pseudo_and_segment[0], server_ip->addr.v4, DMROUTE_IPV4_ADDR_LEN);
    memcpy(&pseudo_and_segment[4], dst->addr.v4, DMROUTE_IPV4_ADDR_LEN);
    pseudo_and_segment[8] = 0;
    pseudo_and_segment[9] = DMIP_PROTO_UDP;
    write_u16_be(&pseudo_and_segment[10], (uint16_t)udp_len);
    memcpy(&pseudo_and_segment[TEST_V4_PSEUDO_HEADER_LEN], udp, udp_len);
    write_u16_be(&udp[6], dmip_checksum(pseudo_and_segment, TEST_V4_PSEUDO_HEADER_LEN + udp_len));

    dmip_v4_header_t ip_header = { 0 };
    ip_header.total_length = (uint16_t)(DMIP_V4_HEADER_LEN + udp_len);
    ip_header.ttl = DMIP_DEFAULT_TTL;
    ip_header.protocol = DMIP_PROTO_UDP;
    ip_header.src = *server_ip;
    ip_header.dst = *dst;

    uint8_t packet[DMIP_V4_HEADER_LEN + DMUDP_HEADER_LEN + TEST_MAX_DHCP_LEN];
    dmip_v4_build_header(packet, sizeof(packet), &ip_header);
    memcpy(packet + DMIP_V4_HEADER_LEN, udp, udp_len);

    feed_frame(iface, TEST_ETHERTYPE_IPV4, packet, DMIP_V4_HEADER_LEN + udp_len);
}

/**
 * @brief Poll dmdhcp_get_state() until it's no longer `from_state`, or
 *        `max_ms` elapses
 *
 * Used to wait out the RFC 2131 §4.4.1 conflict probe hand-off (a
 * dmosi_timer_t firing on a different thread, see
 * src/dmdhcp_internal.h) without hard-coding exactly how long dmarp_resolve()
 * takes in this fixture.
 */
static void wait_while_state(dmdhcp_lease_t lease, dmdhcp_state_t from_state, uint32_t max_ms)
{
    uint32_t waited = 0;
    while (dmdhcp_get_state(lease) == from_state && waited < max_ms)
    {
        dmosi_thread_sleep(20);
        waited += 20;
    }
}

/* ============================================================================
 *                      State machine: initial acquisition
 * ========================================================================== */

DMOD_TEST_STEP(start_sends_discover_and_enters_selecting)
{
    dmdhcp_lease_t lease = dmdhcp_start(g_iface, NULL, NULL, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(lease);
    DMOD_TEST_EXPECT_EQ((int)dmdhcp_get_state(lease), (int)dmdhcp_state_selecting);
    DMOD_TEST_EXPECT_NE(dmdhcp_get_xid(lease), (uint32_t)0);
    DMOD_TEST_EXPECT_EQ(dmdhcp_get_iface(lease), g_iface);

    dmdhcp_stop(lease);
}

DMOD_TEST_STEP(offer_with_wrong_xid_is_ignored)
{
    dmdhcp_lease_t lease = dmdhcp_start(g_iface, NULL, NULL, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(lease);

    dmroute_addr_t server = make_v4(10, 10, 0, 1);
    dmroute_addr_t offered = make_v4(10, 10, 0, 50);
    feed_dhcp_message(g_iface, dmdhcp_msg_offer, dmdhcp_get_xid(lease) ^ 0xFFFFFFFFu, &server, &offered, &server, NULL, NULL, NULL, 3600);

    DMOD_TEST_EXPECT_EQ((int)dmdhcp_get_state(lease), (int)dmdhcp_state_selecting);

    dmdhcp_stop(lease);
}

DMOD_TEST_STEP(offer_moves_to_checking_offer)
{
    dmdhcp_lease_t lease = dmdhcp_start(g_iface, NULL, NULL, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(lease);

    dmroute_addr_t server = make_v4(10, 20, 0, 1);
    dmroute_addr_t offered = make_v4(10, 20, 0, 50);
    dmroute_addr_t netmask = make_v4(255, 255, 255, 0);
    feed_dhcp_message(g_iface, dmdhcp_msg_offer, dmdhcp_get_xid(lease), &server, &offered, &server, &netmask, &server, NULL, 3600);

    DMOD_TEST_EXPECT_EQ((int)dmdhcp_get_state(lease), (int)dmdhcp_state_checking_offer);

    dmdhcp_stop(lease);
}

/**
 * @brief on_bound tracking shared by the state-machine steps below -
 *        module-scope, not per-step, since DMOD_TEST_STEP bodies can't
 *        form closures; dmod_test steps run sequentially, never
 *        concurrently, so this is safe to reset at the top of each step
 *        that uses it.
 */
static int  g_on_bound_call_count = 0;
static bool g_on_bound_last_is_renewal = false;

static void test_on_bound(dmdhcp_lease_t lease, bool is_renewal, void* user_data)
{
    (void)lease;
    (void)user_data;
    g_on_bound_call_count++;
    g_on_bound_last_is_renewal = is_renewal;
}

static int g_on_expired_call_count = 0;

static void test_on_expired(dmdhcp_lease_t lease, void* user_data)
{
    (void)lease;
    (void)user_data;
    g_on_expired_call_count++;
}

/**
 * @brief Full happy path, DISCOVER through BOUND: no ARP reply is ever
 *        seeded, so both conflict probes (post-OFFER, post-ACK) come back
 *        clean - see this file's top comment on how the probe is driven
 *        here instead of via a real ARP frame.
 */
DMOD_TEST_STEP(happy_path_reaches_bound_and_configures_interface)
{
    g_on_bound_call_count = 0;
    dmdhcp_callbacks_t callbacks = { .on_bound = test_on_bound };

    dmdhcp_lease_t lease = dmdhcp_start(g_iface, &callbacks, NULL, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(lease);

    dmroute_addr_t server = make_v4(10, 30, 0, 1);
    dmroute_addr_t offered = make_v4(10, 30, 0, 50);
    dmroute_addr_t netmask = make_v4(255, 255, 255, 0);
    dmroute_addr_t dns = make_v4(8, 8, 8, 8);

    feed_dhcp_message(g_iface, dmdhcp_msg_offer, dmdhcp_get_xid(lease), &server, &offered, &server, &netmask, &server, &dns, 3600);
    DMOD_TEST_EXPECT_EQ((int)dmdhcp_get_state(lease), (int)dmdhcp_state_checking_offer);

    wait_while_state(lease, dmdhcp_state_checking_offer, TEST_ARP_PROBE_TIMEOUT_MS + 500u);
    DMOD_TEST_EXPECT_EQ((int)dmdhcp_get_state(lease), (int)dmdhcp_state_requesting);

    feed_dhcp_message(g_iface, dmdhcp_msg_ack, dmdhcp_get_xid(lease), &server, &offered, &server, &netmask, &server, &dns, 3600);
    DMOD_TEST_EXPECT_EQ((int)dmdhcp_get_state(lease), (int)dmdhcp_state_checking_ack);

    wait_while_state(lease, dmdhcp_state_checking_ack, TEST_ARP_PROBE_TIMEOUT_MS + 500u);
    DMOD_TEST_EXPECT_EQ((int)dmdhcp_get_state(lease), (int)dmdhcp_state_bound);

    DMOD_TEST_EXPECT_EQ(g_on_bound_call_count, 1);
    DMOD_TEST_EXPECT_FALSE(g_on_bound_last_is_renewal);

    dmdhcp_lease_info_t info = { 0 };
    DMOD_TEST_EXPECT_EQ(dmdhcp_get_lease_info(lease, &info), 0);
    DMOD_TEST_EXPECT_EQ((int)info.ip_address.family, (int)dmroute_family_v4);
    DMOD_TEST_EXPECT_EQ(info.ip_address.addr.v4[3], (uint8_t)50);
    DMOD_TEST_EXPECT_EQ(info.lease_time_sec, (uint32_t)3600);

    DMOD_TEST_EXPECT_EQ(dmdhcp_get_dns_server_count(lease), (size_t)1);
    dmroute_addr_t got_dns = { 0 };
    DMOD_TEST_EXPECT_EQ(dmdhcp_get_dns_server(lease, 0, &got_dns), 0);
    DMOD_TEST_EXPECT_EQ(got_dns.addr.v4[0], (uint8_t)8);

    /* Applied to the interface (dmdhcp_lifecycle_apply()) */
    dmroute_addr_t got_ip = { 0 };
    dmnetif_get_ip_address(g_iface, &got_ip);
    DMOD_TEST_EXPECT_EQ((int)got_ip.family, (int)dmroute_family_v4);
    DMOD_TEST_EXPECT_EQ(got_ip.addr.v4[3], (uint8_t)50);

    dmroute_addr_t got_netmask = { 0 };
    dmnetif_get_netmask(g_iface, &got_netmask);
    DMOD_TEST_EXPECT_EQ(got_netmask.addr.v4[3], (uint8_t)0);
    DMOD_TEST_EXPECT_EQ(got_netmask.addr.v4[2], (uint8_t)255);

    dmdhcp_stop(lease);
}

DMOD_TEST_STEP(nak_during_requesting_restarts_discovery_with_new_xid)
{
    dmdhcp_lease_t lease = dmdhcp_start(g_iface, NULL, NULL, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(lease);

    dmroute_addr_t server = make_v4(10, 40, 0, 1);
    dmroute_addr_t offered = make_v4(10, 40, 0, 50);
    dmroute_addr_t netmask = make_v4(255, 255, 255, 0);

    uint32_t first_xid = dmdhcp_get_xid(lease);
    feed_dhcp_message(g_iface, dmdhcp_msg_offer, first_xid, &server, &offered, &server, &netmask, &server, NULL, 3600);
    wait_while_state(lease, dmdhcp_state_checking_offer, TEST_ARP_PROBE_TIMEOUT_MS + 500u);
    DMOD_TEST_EXPECT_EQ((int)dmdhcp_get_state(lease), (int)dmdhcp_state_requesting);

    feed_dhcp_message(g_iface, dmdhcp_msg_nak, dmdhcp_get_xid(lease), &server, NULL, &server, NULL, NULL, NULL, 0);

    DMOD_TEST_EXPECT_EQ((int)dmdhcp_get_state(lease), (int)dmdhcp_state_selecting);
    DMOD_TEST_EXPECT_NE(dmdhcp_get_xid(lease), first_xid);

    dmdhcp_stop(lease);
}

/* ============================================================================
 *                      State machine: address conflict (DHCPDECLINE)
 * ========================================================================== */

/**
 * @brief Seeding dmarp's cache for the offered candidate makes the
 * deferred conflict probe (dmarp_resolve()) resolve as a hit immediately -
 * simulating "someone answered the ARP probe" without a real frame, see
 * this file's top comment.
 */
DMOD_TEST_STEP(conflicting_offer_triggers_decline_and_restart)
{
    dmdhcp_lease_t lease = dmdhcp_start(g_iface, NULL, NULL, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(lease);

    dmroute_addr_t server = make_v4(10, 50, 0, 1);
    dmroute_addr_t offered = make_v4(10, 50, 0, 77);
    dmnetif_mac_addr_t conflicting_mac = { .addr = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x99 } };

    uint32_t first_xid = dmdhcp_get_xid(lease);
    feed_dhcp_message(g_iface, dmdhcp_msg_offer, first_xid, &server, &offered, &server, NULL, NULL, NULL, 3600);
    DMOD_TEST_EXPECT_EQ((int)dmdhcp_get_state(lease), (int)dmdhcp_state_checking_offer);

    dmarp_cache_insert(g_iface, &offered, &conflicting_mac);

    /* The probe should now resolve almost immediately (cache hit, no wire
     * ARP wait) - still poll rather than assume a fixed delay. */
    wait_while_state(lease, dmdhcp_state_checking_offer, TEST_ARP_PROBE_TIMEOUT_MS + 500u);

    DMOD_TEST_EXPECT_EQ((int)dmdhcp_get_state(lease), (int)dmdhcp_state_declining);
    /* A fresh DISCOVER (and a new xid) only goes out once the >=10s RFC
     * 2131 §4.4.1 pause elapses - not waited out here, just confirm the
     * xid is still the declined transaction's. */
    DMOD_TEST_EXPECT_EQ(dmdhcp_get_xid(lease), first_xid);

    dmarp_cache_remove(g_iface, &offered);
    dmdhcp_stop(lease);
}

/* ============================================================================
 *                      State machine: renewal / lease loss
 * ========================================================================== */

/**
 * @brief Drives a lease to BOUND (see happy_path_reaches_bound_and_configures_interface),
 *        then exercises dmdhcp_renew() -> ACK -> BOUND again
 */
DMOD_TEST_STEP(renew_forces_renewing_and_ack_returns_to_bound)
{
    g_on_bound_call_count = 0;
    dmdhcp_callbacks_t callbacks = { .on_bound = test_on_bound };

    dmdhcp_lease_t lease = dmdhcp_start(g_iface, &callbacks, NULL, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(lease);

    dmroute_addr_t server = make_v4(10, 60, 0, 1);
    dmroute_addr_t offered = make_v4(10, 60, 0, 50);
    dmroute_addr_t netmask = make_v4(255, 255, 255, 0);

    feed_dhcp_message(g_iface, dmdhcp_msg_offer, dmdhcp_get_xid(lease), &server, &offered, &server, &netmask, NULL, NULL, 3600);
    wait_while_state(lease, dmdhcp_state_checking_offer, TEST_ARP_PROBE_TIMEOUT_MS + 500u);
    feed_dhcp_message(g_iface, dmdhcp_msg_ack, dmdhcp_get_xid(lease), &server, &offered, &server, &netmask, NULL, NULL, 3600);
    wait_while_state(lease, dmdhcp_state_checking_ack, TEST_ARP_PROBE_TIMEOUT_MS + 500u);
    DMOD_TEST_EXPECT_EQ((int)dmdhcp_get_state(lease), (int)dmdhcp_state_bound);
    DMOD_TEST_EXPECT_EQ(g_on_bound_call_count, 1);
    DMOD_TEST_EXPECT_FALSE(g_on_bound_last_is_renewal);

    DMOD_TEST_EXPECT_EQ(dmdhcp_renew(lease), 0);
    DMOD_TEST_EXPECT_EQ((int)dmdhcp_get_state(lease), (int)dmdhcp_state_renewing);

    /* RENEWING never re-probes for a conflict (see src/dmdhcp_input.c) -
     * the ACK is applied directly, so no wait_while_state() is needed
     * here. */
    feed_dhcp_message(g_iface, dmdhcp_msg_ack, dmdhcp_get_xid(lease), &server, &offered, &server, &netmask, NULL, NULL, 7200);
    DMOD_TEST_EXPECT_EQ((int)dmdhcp_get_state(lease), (int)dmdhcp_state_bound);
    DMOD_TEST_EXPECT_EQ(g_on_bound_call_count, 2);
    DMOD_TEST_EXPECT_TRUE(g_on_bound_last_is_renewal);

    dmdhcp_lease_info_t info = { 0 };
    DMOD_TEST_EXPECT_EQ(dmdhcp_get_lease_info(lease, &info), 0);
    DMOD_TEST_EXPECT_EQ(info.lease_time_sec, (uint32_t)7200);

    dmdhcp_stop(lease);
}

DMOD_TEST_STEP(nak_during_renewing_loses_lease_and_deconfigures_interface)
{
    g_on_expired_call_count = 0;
    dmdhcp_callbacks_t callbacks = { .on_expired_or_lost = test_on_expired };

    dmdhcp_lease_t lease = dmdhcp_start(g_iface, &callbacks, NULL, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(lease);

    dmroute_addr_t server = make_v4(10, 70, 0, 1);
    dmroute_addr_t offered = make_v4(10, 70, 0, 50);
    dmroute_addr_t netmask = make_v4(255, 255, 255, 0);

    feed_dhcp_message(g_iface, dmdhcp_msg_offer, dmdhcp_get_xid(lease), &server, &offered, &server, &netmask, NULL, NULL, 3600);
    wait_while_state(lease, dmdhcp_state_checking_offer, TEST_ARP_PROBE_TIMEOUT_MS + 500u);
    feed_dhcp_message(g_iface, dmdhcp_msg_ack, dmdhcp_get_xid(lease), &server, &offered, &server, &netmask, NULL, NULL, 3600);
    wait_while_state(lease, dmdhcp_state_checking_ack, TEST_ARP_PROBE_TIMEOUT_MS + 500u);
    DMOD_TEST_EXPECT_EQ((int)dmdhcp_get_state(lease), (int)dmdhcp_state_bound);

    dmdhcp_renew(lease);
    DMOD_TEST_EXPECT_EQ((int)dmdhcp_get_state(lease), (int)dmdhcp_state_renewing);

    feed_dhcp_message(g_iface, dmdhcp_msg_nak, dmdhcp_get_xid(lease), &server, NULL, &server, NULL, NULL, NULL, 0);

    /* dmdhcp_lifecycle_handle_lost() unapplies, fires on_expired_or_lost,
     * then restarts discovery - all synchronous within the rx-thread call
     * that fed the NAK (see src/dmdhcp_lifecycle.c), so no wait is needed. */
    DMOD_TEST_EXPECT_EQ((int)dmdhcp_get_state(lease), (int)dmdhcp_state_selecting);
    DMOD_TEST_EXPECT_EQ(g_on_expired_call_count, 1);

    dmroute_addr_t got_ip = { 0 };
    dmnetif_get_ip_address(g_iface, &got_ip);
    DMOD_TEST_EXPECT_EQ((int)got_ip.family, (int)dmroute_family_none);

    dmdhcp_stop(lease);
}
