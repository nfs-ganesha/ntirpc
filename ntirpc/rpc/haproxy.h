/*
 * include/haproxy/connection-t.h
 * This file describes the connection struct and associated constants.
 *
 * Copyright (C) 2000-2014 Willy Tarreau - w@1wt.eu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* NOTE: This file is extracted from the haproxy project at:
 *       https://github.com/haproxy/haproxy.git
 *
 * Specifically, parts of this file: include/haproxy/connection-t.h
 *
 * NOTE: the definitions in this file are based on the haproxy spec in
 * https://www.haproxy.org/download/1.8/doc/proxy-protocol.txt
 */

#include "config.h"

#ifndef _NTIRPC_RPC_HAPROXY_H
#define _NTIRPC_RPC_HAPROXY_H

/* proxy protocol v2 definitions */
#define PP2_SIGNATURE "\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A"
#define PP2_SIG_UINT32 0x0d0a0d0a
#define PP2_SIG_UINT32_2 0x000d0a51
#define PP2_SIG_UINT32_3 0x5549540a
#define PP2_SIGNATURE_LEN 12
#define PP2_HEADER_LEN 16

/* ver_cmd byte */
#define PP2_CMD_LOCAL 0x00
#define PP2_CMD_PROXY 0x01
#define PP2_CMD_MASK 0x0F

#define PP2_VERSION 0x20
#define PP2_VERSION_MASK 0xF0

#define PP2_VERSION2_CMD_LOCAL (PP2_CMD_LOCAL | PP2_VERSION)
#define PP2_VERSION2_CMD_PROXY (PP2_CMD_PROXY | PP2_VERSION)

/* fam byte */
#define PP2_TRANS_UNSPEC 0x00
#define PP2_TRANS_STREAM 0x01
#define PP2_TRANS_DGRAM 0x02
#define PP2_TRANS_MASK 0x0F

#define PP2_FAM_UNSPEC 0x00
#define PP2_FAM_INET 0x10
#define PP2_FAM_INET6 0x20
#define PP2_FAM_UNIX 0x30
#define PP2_FAM_MASK 0xF0

#define PP2_TRANS_STREAM_FAM_INET (PP2_TRANS_STREAM | PP2_FAM_INET)
#define PP2_TRANS_STREAM_FAM_INET6 (PP2_TRANS_STREAM | PP2_FAM_INET6)

#define PP2_ADDR_LEN_UNSPEC (0)
#define PP2_ADDR_LEN_INET (4 + 4 + 2 + 2)
#define PP2_ADDR_LEN_INET6 (16 + 16 + 2 + 2)
#define PP2_ADDR_LEN_UNIX (108 + 108)

#define PP2_HDR_LEN_UNSPEC (PP2_HEADER_LEN + PP2_ADDR_LEN_UNSPEC)
#define PP2_HDR_LEN_INET (PP2_HEADER_LEN + PP2_ADDR_LEN_INET)
#define PP2_HDR_LEN_INET6 (PP2_HEADER_LEN + PP2_ADDR_LEN_INET6)
#define PP2_HDR_LEN_UNIX (PP2_HEADER_LEN + PP2_ADDR_LEN_UNIX)

#define PP2_TYPE_ALPN 0x01
#define PP2_TYPE_AUTHORITY 0x02
#define PP2_TYPE_CRC32C 0x03
#define PP2_TYPE_NOOP 0x04
#define PP2_TYPE_UNIQUE_ID 0x05
#define PP2_TYPE_SSL 0x20
#define PP2_SUBTYPE_SSL_VERSION 0x21
#define PP2_SUBTYPE_SSL_CN 0x22
#define PP2_SUBTYPE_SSL_CIPHER 0x23
#define PP2_SUBTYPE_SSL_SIG_ALG 0x24
#define PP2_SUBTYPE_SSL_KEY_ALG 0x25
#define PP2_TYPE_NETNS 0x30

#define PP2_TYPE_UNSET 0x00

/* Define custom PP2_TYPE by common cloud provides */
/* https://cloud.google.com/vpc/docs/about-vpc-hosted-services#proxy-protocol */
#define PP2_TYPE_GCP 0xE0
#define PP2_TYPE_GCP_EXPECTED_LENGTH 8
/* https://docs.aws.amazon.com/elasticloadbalancing/latest/network/edit-target-group-attributes.html#proxy-protocol */
#define PP2_TYPE_AWS 0xEA
/* https://learn.microsoft.com/en-us/azure/private-link/private-link-service-overview#getting-connection-information-using-tcp-proxy-v2 */
#define PP2_TYPE_AZURE 0xEE

#define PP2_CLIENT_SSL 0x01
#define PP2_CLIENT_CERT_CONN 0x02
#define PP2_CLIENT_CERT_SESS 0x04

/* Max length of the authority TLV */
#define PP2_AUTHORITY_MAX 255

#define TLV_HEADER_SIZE 3

struct proxy_header_part {
	uint8_t ver_cmd; /* protocol version and command */
	uint8_t fam; /* protocol family and transport */
	uint16_t len; /* number of following bytes part of the header */
};

struct proxy_header_addr_ip4_part { /* for TCP/UDP over IPv4, len = 12 */
	uint32_t src_addr;
	uint32_t dst_addr;
	uint16_t src_port;
	uint16_t dst_port;
};
_Static_assert(
	sizeof(struct proxy_header_addr_ip4_part) == PP2_ADDR_LEN_INET,
	"proxy_header_addr_ip4_part size is not equal to PP2_ADDR_LEN_INET");

struct proxy_header_addr_ip6_part { /* for TCP/UDP over IPv6, len = 36 */
	uint8_t src_addr[16];
	uint8_t dst_addr[16];
	uint16_t src_port;
	uint16_t dst_port;
};
_Static_assert(
	sizeof(struct proxy_header_addr_ip6_part) == PP2_ADDR_LEN_INET6,
	"proxy_header_addr_ip6_part size is not equal to PP2_ADDR_LEN_INET6");

typedef uint8_t proxy_protocol_tlv_type;
typedef uint16_t proxy_protocol_tlv_length;
struct proxy_protocol_tlv_header {
	proxy_protocol_tlv_type type;
	proxy_protocol_tlv_length length;
	/* value is stored in big-endian as received from socket */
	void *value;
};

struct proxy_protocol_tlv_headers {
	uint16_t tlv_count;
	struct proxy_protocol_tlv_header *tlvs;
};

#endif /* _NTIRPC_RPC_HAPROXY_H */
