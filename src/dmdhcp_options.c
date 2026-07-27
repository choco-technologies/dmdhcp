/**
 * @file dmdhcp_options.c
 * @brief RFC 2132 TLV options codec - see dmdhcp.h
 *
 * Wire shape: a sequence of [code(1)][length(1)][data(length bytes)]
 * entries, except DMDHCP_OPT_PAD (0) and DMDHCP_OPT_END (255), which carry
 * no length/data byte at all (RFC 2132 §2).
 */
#include "dmod.h"
#include "dmdhcp_internal.h"
#include <string.h>
#include <errno.h>

static uint32_t read_u32_be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

dmod_dmdhcp_api_declaration(1.0, int, _options_write, ( uint8_t* buffer, size_t buffer_len, const dmdhcp_option_t* options, size_t option_count, size_t* out_len ))
{
    if (buffer == NULL || out_len == NULL || (options == NULL && option_count > 0))
        return -EINVAL;

    size_t pos = 0;
    for (size_t i = 0; i < option_count; i++)
    {
        const dmdhcp_option_t* opt = &options[i];
        if (opt->data == NULL && opt->length > 0)
            return -EINVAL;
        if (pos + 2u + (size_t)opt->length > buffer_len)
            return -ENOSPC;

        buffer[pos++] = opt->code;
        buffer[pos++] = opt->length;
        if (opt->length > 0)
        {
            memcpy(&buffer[pos], opt->data, opt->length);
            pos += opt->length;
        }
    }

    if (pos + 1u > buffer_len)
        return -ENOSPC;
    buffer[pos++] = DMDHCP_OPT_END;

    *out_len = pos;
    return 0;
}

dmod_dmdhcp_api_declaration(1.0, int, _options_find, ( const uint8_t* options, size_t options_len, uint8_t code, dmdhcp_option_t* out ))
{
    if (options == NULL || out == NULL)
        return -EINVAL;

    size_t pos = 0;
    while (pos < options_len)
    {
        uint8_t entry_code = options[pos];
        if (entry_code == DMDHCP_OPT_PAD)
        {
            pos++;
            continue;
        }
        if (entry_code == DMDHCP_OPT_END)
        {
            break;
        }
        if (pos + 1u >= options_len)
            return -EPROTO; /* truncated length byte */

        uint8_t entry_len = options[pos + 1u];
        if (pos + 2u + (size_t)entry_len > options_len)
            return -EPROTO; /* truncated data */

        if (entry_code == code)
        {
            out->code   = entry_code;
            out->length = entry_len;
            out->data   = &options[pos + 2u];
            return 0;
        }

        pos += 2u + (size_t)entry_len;
    }

    return -ENOENT;
}

dmod_dmdhcp_api_declaration(1.0, int, _option_get_u32, ( const dmdhcp_option_t* opt, uint32_t* out ))
{
    if (opt == NULL || out == NULL || opt->length != 4u)
        return -EINVAL;

    *out = read_u32_be(opt->data);
    return 0;
}

dmod_dmdhcp_api_declaration(1.0, int, _option_get_addr, ( const dmdhcp_option_t* opt, dmroute_addr_t* out ))
{
    if (opt == NULL || out == NULL || opt->length != 4u)
        return -EINVAL;

    out->family = dmroute_family_v4;
    memcpy(out->addr.v4, opt->data, DMROUTE_IPV4_ADDR_LEN);
    return 0;
}

dmod_dmdhcp_api_declaration(1.0, size_t, _option_addr_count, ( const dmdhcp_option_t* opt ))
{
    if (opt == NULL)
        return 0;
    return (size_t)opt->length / DMROUTE_IPV4_ADDR_LEN;
}

dmod_dmdhcp_api_declaration(1.0, int, _option_get_addr_at, ( const dmdhcp_option_t* opt, size_t index, dmroute_addr_t* out ))
{
    if (opt == NULL || out == NULL)
        return -EINVAL;

    size_t count = (size_t)opt->length / DMROUTE_IPV4_ADDR_LEN;
    if (index >= count)
        return -EINVAL;

    out->family = dmroute_family_v4;
    memcpy(out->addr.v4, opt->data + (index * DMROUTE_IPV4_ADDR_LEN), DMROUTE_IPV4_ADDR_LEN);
    return 0;
}
