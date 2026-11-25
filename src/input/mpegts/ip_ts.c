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

#include "ip_ts.h"
#include "rtp_mpa.h"

#include <stdio.h>
#include <stdlib.h>

struct ip_ts {
  uint32_t       addr;
  uint16_t       port;
  ip_ts_cb_t     cb;
  void          *opaque;
  int            rtp_seq;       /* the last RTP sequence number, -1: none */
  rtp_mpa_t     *mpa;           /* MPEG audio in RTP came: it makes the TS */
  ip_ts_counts_t c;
};

/* sync bytes every 188 bytes (n: a multiple of 188, not 0) */
static int
ip_ts_sync ( const uint8_t *d, size_t n )
{
  size_t i;

  for (i = 0; i < n; i += 188)
    if (d[i] != 0x47)
      return 0;
  return 1;
}

int
ip_ts_payload ( const uint8_t *d, size_t n, size_t *off, int *rtp_pt )
{
  size_t h;

  *off = 0;
  *rtp_pt = -1;
  if (n >= 188 && n % 188 == 0 && ip_ts_sync(d, n))
    return 1;
  /* RTP (version 2): its header, the CSRCs and the extension */
  if (n >= 12 + 188 && (d[0] >> 6) == 2) {
    h = 12 + 4 * (size_t)(d[0] & 0x0f);
    if ((d[0] & 0x10) && h + 4 <= n)
      h += 4 + 4 * (((size_t)d[h + 2] << 8) | d[h + 3]);
    if (h < n && (n - h) % 188 == 0 && ip_ts_sync(d + h, n - h)) {
      *off = h;
      *rtp_pt = d[1] & 0x7f;
      return 1;
    }
  }
  /* a header of the sender: two packets after it at least, a single sync
   * byte tells nothing */
  for (h = 1; h <= IP_TS_HEADER_MAX && h + 2 * 188 <= n; h++)
    if ((n - h) % 188 == 0 && ip_ts_sync(d + h, n - h)) {
      *off = h;
      return 1;
    }
  return 0;
}

ip_ts_t *
ip_ts_create ( uint32_t addr, uint16_t port, ip_ts_cb_t cb, void *opaque )
{
  ip_ts_t *it = calloc(1, sizeof(*it));

  if (it == NULL)
    return NULL;
  it->addr = addr;
  it->port = port;
  it->cb = cb;
  it->opaque = opaque;
  it->rtp_seq = -1;
  return it;
}

void
ip_ts_destroy ( ip_ts_t *it )
{
  if (it == NULL)
    return;
  rtp_mpa_destroy(it->mpa);
  free(it);
}

/* RTP packets missing before this one, by its sequence number */
static void
ip_ts_rtp_seq ( ip_ts_t *it, const uint8_t *rtp )
{
  const int seq = (rtp[2] << 8) | rtp[3];

  if (it->rtp_seq >= 0 && seq != ((it->rtp_seq + 1) & 0xffff))
    it->c.rtp_lost += (uint16_t)(seq - it->rtp_seq - 1);
  it->rtp_seq = seq;
}

static void
ip_ts_mpa_cb ( void *opaque, const uint8_t *ts, size_t len )
{
  ip_ts_t *it = opaque;

  it->c.packets += len / 188;
  it->cb(it->opaque, ts, len);
}

/* a UDP payload of the group that is no TS: MPEG audio in RTP is made one */
static int
ip_ts_mpa ( ip_ts_t *it, const uint8_t *u, size_t ulen )
{
  size_t off;
  size_t len;
  int frag;
  char name[32];

  if (!rtp_mpa_payload(u, ulen, &off, &len, &frag))
    return 0;
  if (it->mpa == NULL) {
    snprintf(name, sizeof(name), "%u.%u.%u.%u:%u", (it->addr >> 24) & 0xff,
             (it->addr >> 16) & 0xff, (it->addr >> 8) & 0xff, it->addr & 0xff, it->port);
    it->mpa = rtp_mpa_create(it->port, name, ip_ts_mpa_cb, it);
    if (it->mpa == NULL)
      return 0;
  }
  it->c.audio++;
  ip_ts_rtp_seq(it, u);
  rtp_mpa_packet(it->mpa, u, ulen);
  return 1;
}

/* the UDP payload (*ulen bytes) of an IPv4 packet (n bytes) that is a whole
 * datagram for the group and port; NULL else - it is counted when it is one
 * for them: a fragment, or one cut short */
static const uint8_t *
ip_ts_udp ( ip_ts_t *it, const uint8_t *d, size_t n, size_t *ulen )
{
  const uint8_t *u;
  size_t ihl;
  size_t tot;

  if (n < 28 || (d[0] >> 4) != 4 || d[9] != 17)
    return NULL;
  ihl = 4 * (size_t)(d[0] & 0x0f);
  if (ihl < 20 || ihl + 8 > n)
    return NULL;
  if ((((uint32_t)d[16] << 24) | ((uint32_t)d[17] << 16) |
       ((uint32_t)d[18] << 8) | d[19]) != it->addr)
    return NULL;
  /* a later fragment has no UDP header: not known whose it is */
  if ((d[6] & 0x1f) || d[7])
    return NULL;
  u = d + ihl;
  if ((((uint16_t)u[2] << 8) | u[3]) != it->port)
    return NULL;
  it->c.datagrams++;
  if (d[6] & 0x20) {            /* the first fragment of a datagram */
    it->c.fragments++;
    return NULL;
  }
  tot = ((size_t)d[2] << 8) | d[3];
  if (tot < ihl + 8 || tot > n)
    tot = n;
  *ulen = ((size_t)u[4] << 8) | u[5];
  if (*ulen < 8 || ihl + *ulen > tot) {
    it->c.other++;
    return NULL;
  }
  *ulen -= 8;
  return u + 8;
}

void
ip_ts_ipv4 ( ip_ts_t *it, const uint8_t *d, size_t n )
{
  size_t ulen;
  size_t off;
  int rtp_pt;
  const uint8_t *u = ip_ts_udp(it, d, n, &ulen);

  if (u == NULL)
    return;
  if (!ip_ts_payload(u, ulen, &off, &rtp_pt)) {
    if (!ip_ts_mpa(it, u, ulen))
      it->c.other++;
    return;
  }
  if (rtp_pt >= 0)
    ip_ts_rtp_seq(it, u);
  it->c.packets += (ulen - off) / 188;
  it->cb(it->opaque, u + off, ulen - off);
}

const ip_ts_counts_t *
ip_ts_counts ( const ip_ts_t *it )
{
  return &it->c;
}
