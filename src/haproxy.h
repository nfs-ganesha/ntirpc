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
 */

/* proxy protocol v2 definitions */
#define PP2_SIGNATURE        "\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A"
#define PP2_SIG_UINT32       0x0d0a0d0a
#define PP2_SIG_UINT32_2     0x000d0a51
#define PP2_SIG_UINT32_3     0x5549540a
#define PP2_SIGNATURE_LEN    12
#define PP2_HEADER_LEN       16

/* ver_cmd byte */
#define PP2_CMD_LOCAL        0x00
#define PP2_CMD_PROXY        0x01
#define PP2_CMD_MASK         0x0F

#define PP2_VERSION          0x20
#define PP2_VERSION_MASK     0xF0

#define PP2_VERSION2_CMD_LOCAL (PP2_CMD_LOCAL | PP2_VERSION)
#define PP2_VERSIOB2_CMD_PROXY (PP2_CMD_PROXY | PP2_VERSION)

/* fam byte */
#define PP2_TRANS_UNSPEC     0x00
#define PP2_TRANS_STREAM     0x01
#define PP2_TRANS_DGRAM      0x02
#define PP2_TRANS_MASK       0x0F

#define PP2_FAM_UNSPEC       0x00
#define PP2_FAM_INET         0x10
#define PP2_FAM_INET6        0x20
#define PP2_FAM_UNIX         0x30
#define PP2_FAM_MASK         0xF0

#define PP2_TRANS_STREAM_FAM_INET (PP2_TRANS_STREAM | PP2_FAM_INET)
#define PP2_TRANS_STREAM_FAM_INET6 (PP2_TRANS_STREAM | PP2_FAM_INET6)

#define PP2_ADDR_LEN_UNSPEC  (0)
#define PP2_ADDR_LEN_INET    (4 + 4 + 2 + 2)
#define PP2_ADDR_LEN_INET6   (16 + 16 + 2 + 2)
#define PP2_ADDR_LEN_UNIX    (108 + 108)

#define PP2_HDR_LEN_UNSPEC   (PP2_HEADER_LEN + PP2_ADDR_LEN_UNSPEC)
#define PP2_HDR_LEN_INET     (PP2_HEADER_LEN + PP2_ADDR_LEN_INET)
#define PP2_HDR_LEN_INET6    (PP2_HEADER_LEN + PP2_ADDR_LEN_INET6)
#define PP2_HDR_LEN_UNIX     (PP2_HEADER_LEN + PP2_ADDR_LEN_UNIX)

#define PP2_TYPE_ALPN           0x01
#define PP2_TYPE_AUTHORITY      0x02
#define PP2_TYPE_CRC32C         0x03
#define PP2_TYPE_NOOP           0x04
#define PP2_TYPE_UNIQUE_ID      0x05
#define PP2_TYPE_SSL            0x20
#define PP2_SUBTYPE_SSL_VERSION 0x21
#define PP2_SUBTYPE_SSL_CN      0x22
#define PP2_SUBTYPE_SSL_CIPHER  0x23
#define PP2_SUBTYPE_SSL_SIG_ALG 0x24
#define PP2_SUBTYPE_SSL_KEY_ALG 0x25
#define PP2_TYPE_NETNS          0x30

#define PP2_CLIENT_SSL           0x01
#define PP2_CLIENT_CERT_CONN     0x02
#define PP2_CLIENT_CERT_SESS     0x04

/* Max length of the authority TLV */
#define PP2_AUTHORITY_MAX 255

#define TLV_HEADER_SIZE      3

struct proxy_header_part {
	uint8_t ver_cmd;   /* protocol version and command */
	uint8_t fam;       /* protocol family and transport */
	uint16_t len;      /* number of following bytes part of the header */
};

union proxy_addr { 
	struct {   /* for TCP/UDP over IPv4, len = 12 */
		uint32_t src_addr;
		uint32_t dst_addr;
		uint16_t src_port;
		uint16_t dst_port;
	} ip4;
	struct {   /* for TCP/UDP over IPv6, len = 36 */
		uint8_t  src_addr[16];
		uint8_t  dst_addr[16];
		uint16_t src_port;
		uint16_t dst_port;
	} ip6;
	struct {   /* for AF_UNIX sockets, len = 216 */
		uint8_t src_addr[108];
		uint8_t dst_addr[108];
	} unx;
};

struct proxy_hdr_v2 {
	uint8_t sig[12];   /* hex 0D 0A 0D 0A 00 0D 0A 51 55 49 54 0A */
	struct proxy_header_part header_part;
	union proxy_addr addr;
};

enum xprt_stat recv_haproxy_header(SVCXPRT *xprt);
