/*
 *  Tvheadend - MPEG-TS in the UDP datagrams of an IP multicast group
 *
 *  Copyright (C) 2026 L-S-D
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __TVH_IP_TS_H__
#define __TVH_IP_TS_H__

#include <stddef.h>
#include <stdint.h>

/*
 * The TS packets of a UDP payload: all of it, or what follows an RTP header
 * or a short header of the sender (sync bytes every 188 bytes up to the end
 * tell it). And the TS of one IPv4 group and port, from the IPv4 packets of
 * a GSE stream or of an MPE PID: the one its datagrams carry, or the one
 * made of the MPEG audio they carry in RTP (rtp_mpa.h; a service named by
 * the group and port, the port is the ID of its TS).
 * Plain computation: no tvheadend state, no locks.
 */

#define IP_TS_HEADER_MAX 32     /* the longest header of a sender */

/* the TS packets of a UDP payload (n bytes) start at *off and go to its
 * end: 0, or after the RTP header (*rtp_pt its payload type, else -1), or
 * after a header of the sender. 0: no TS */
int ip_ts_payload ( const uint8_t *d, size_t n, size_t *off, int *rtp_pt );

typedef void (*ip_ts_cb_t) ( void *opaque, const uint8_t *ts, size_t len );

typedef struct ip_ts_counts {
  uint64_t datagrams;           /* of the group and port */
  uint64_t packets;             /* TS packets given out */
  uint64_t audio;               /* datagrams with MPEG audio in RTP, made TS packets */
  uint64_t other;               /* datagrams that were neither */
  uint64_t fragments;           /* IP fragments (not put together) */
  uint64_t rtp_lost;            /* RTP packets missing by their sequence numbers */
} ip_ts_counts_t;

typedef struct ip_ts ip_ts_t;

/* addr: the IPv4 group, in host byte order */
ip_ts_t *ip_ts_create ( uint32_t addr, uint16_t port, ip_ts_cb_t cb, void *opaque );
void     ip_ts_destroy ( ip_ts_t *it );

/* an IPv4 packet (n bytes): the TS packets of a datagram of the group go
 * to cb, whole ones, and those made of its MPEG audio */
void     ip_ts_ipv4 ( ip_ts_t *it, const uint8_t *d, size_t n );

const ip_ts_counts_t *ip_ts_counts ( const ip_ts_t *it );

#endif /* __TVH_IP_TS_H__ */
