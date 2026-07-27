/**
 * @file dmdhcp_wire.c
 * @brief DHCP/BOOTP fixed header build/parse - see dmdhcp.h
 */
#include "dmod.h"
#include "dmdhcp_internal.h"
#include <string.h>
#include <errno.h>

#define DMDHCP_SNAME_LEN 64u
#define DMDHCP_FILE_LEN  128u

static void write_u16_be(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFu);
}

static uint16_t read_u16_be(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void write_u32_be(uint8_t* p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)(value & 0xFFu);
}

static uint32_t read_u32_be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/**
 * @brief Write one BOOTP address field (ciaddr/yiaddr/siaddr/giaddr) - 4
 *        bytes, dmroute_family_v4's raw bytes or all-zero for
 *        dmroute_family_none
 */
static int write_addr(uint8_t* p, const dmroute_addr_t* addr)
{
    if (addr->family == dmroute_family_v4)
    {
        memcpy(p, addr->addr.v4, DMROUTE_IPV4_ADDR_LEN);
        return 0;
    }
    if (addr->family == dmroute_family_none)
    {
        memset(p, 0, DMROUTE_IPV4_ADDR_LEN);
        return 0;
    }
    return -EINVAL; /* dmroute_family_v6 - DHCP is IPv4-only */
}

static void read_addr(const uint8_t* p, dmroute_addr_t* addr)
{
    /* Always parsed as v4 - the wire format has no way to distinguish
     * "legitimately unset" from "0.0.0.0" once received, unlike a
     * caller-constructed dmdhcp_message_t about to be built. */
    addr->family = dmroute_family_v4;
    memcpy(addr->addr.v4, p, DMROUTE_IPV4_ADDR_LEN);
}

dmod_dmdhcp_api_declaration(1.0, int, _build_message, ( uint8_t* buffer, size_t buffer_len, const dmdhcp_message_t* message ))
{
    if (buffer == NULL || message == NULL || buffer_len < DMDHCP_FIXED_HEADER_LEN + 4u)
        return -EINVAL;

    memset(buffer, 0, DMDHCP_FIXED_HEADER_LEN + 4u);

    buffer[0] = (uint8_t)message->op;
    buffer[1] = message->htype;
    buffer[2] = message->hlen;
    buffer[3] = message->hops;
    write_u32_be(&buffer[4], message->xid);
    write_u16_be(&buffer[8], message->secs);
    write_u16_be(&buffer[10], message->flags);

    int result = write_addr(&buffer[12], &message->ciaddr);
    if (result == 0) result = write_addr(&buffer[16], &message->yiaddr);
    if (result == 0) result = write_addr(&buffer[20], &message->siaddr);
    if (result == 0) result = write_addr(&buffer[24], &message->giaddr);
    if (result != 0)
        return result;

    memcpy(&buffer[28], message->chaddr, DMDHCP_CHADDR_LEN);
    /* buffer[44..235] (sname, file) already zeroed above - option overload
     * (RFC 2132 §9.3) is out of scope, see dmdhcp.h's top comment */

    write_u32_be(&buffer[DMDHCP_FIXED_HEADER_LEN], DMDHCP_MAGIC_COOKIE);
    return 0;
}

dmod_dmdhcp_api_declaration(1.0, int, _parse_message, ( const uint8_t* buffer, size_t length, dmdhcp_message_t* message, size_t* options_offset ))
{
    if (buffer == NULL || message == NULL || options_offset == NULL || length < DMDHCP_FIXED_HEADER_LEN + 4u)
        return -EINVAL;

    if (read_u32_be(&buffer[DMDHCP_FIXED_HEADER_LEN]) != DMDHCP_MAGIC_COOKIE)
        return -EPROTO;

    uint8_t op = buffer[0];
    if (op != (uint8_t)dmdhcp_op_bootrequest && op != (uint8_t)dmdhcp_op_bootreply)
        return -EPROTO;

    message->op   = (dmdhcp_op_t)op;
    message->htype = buffer[1];
    message->hlen  = buffer[2];
    message->hops  = buffer[3];
    message->xid   = read_u32_be(&buffer[4]);
    message->secs  = read_u16_be(&buffer[8]);
    message->flags = read_u16_be(&buffer[10]);

    read_addr(&buffer[12], &message->ciaddr);
    read_addr(&buffer[16], &message->yiaddr);
    read_addr(&buffer[20], &message->siaddr);
    read_addr(&buffer[24], &message->giaddr);

    memcpy(message->chaddr, &buffer[28], DMDHCP_CHADDR_LEN);

    *options_offset = DMDHCP_FIXED_HEADER_LEN + 4u;
    return 0;
}
