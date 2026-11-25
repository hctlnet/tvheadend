/*
 *  Tvheadend - MPEG audio in RTP as a transport stream
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

#include "rtp_mpa.h"

#include <stdlib.h>
#include <string.h>

#define RTP_MPA_PID_SDT   0x0011
#define RTP_MPA_PID_PMT   0x0100
#define RTP_MPA_PID_AUDIO 0x0101    /* the PCR is in it */
#define RTP_MPA_SID       1
#define RTP_MPA_ONID      0xff00    /* of the range for private temporary use (TS 101 162) */
#define RTP_MPA_NAME_MAX  64
#define RTP_MPA_FRAME_MAX 2048      /* an audio frame has 1729 bytes at most */

/* the PTS is this much after the PCR: a packet brings the frames of a time
 * (ten of 36 ms on 1W 11049V) at once, and comes when its sender has them */
#define RTP_MPA_PCR_AHEAD 45000
/* the tables, every quarter of a second of the stream */
#define RTP_MPA_PSI_STEP  22500

struct rtp_mpa {
  rtp_mpa_cb_t cb;
  void        *opaque;
  uint16_t     tsid;
  char         name[RTP_MPA_NAME_MAX + 1];
  uint8_t      pat[188];        /* the packets of the tables */
  uint8_t      pmt[188];
  uint8_t      sdt[188];
  uint8_t      cc_pat;          /* the continuity counters */
  uint8_t      cc_pmt;
  uint8_t      cc_sdt;
  uint8_t      cc_audio;
  int          lsf;             /* the audio: 0 MPEG-1, 1 MPEG-2 (the lower sampling
                                   frequencies); -1 no frame yet */
  uint8_t      pmt_version;
  int          have_ts;         /* a frame began: */
  uint32_t     rtp_ts;          /* ... the RTP time stamp of the last one */
  uint64_t     pts;             /* ... and that one without the wrap of its 32 bits */
  uint64_t     psi_pts;         /* the PTS the tables went out at last */
  uint32_t     ssrc;            /* the sender (another one has time stamps of its own) */
  int          discontinuity;   /* tell it with the next PCR */
  size_t       flen;            /* a frame that comes in fragments: its length, 0 none */
  size_t       have;            /* ... what is there of it */
  uint8_t      frame[RTP_MPA_FRAME_MAX];
};

/* the header of an MPEG audio frame (4 bytes) */
static int
rtp_mpa_frame ( const uint8_t *h )
{
  return h[0] == 0xff && (h[1] & 0xe0) == 0xe0 &&
         ((h[1] >> 3) & 3) != 1 &&      /* the version: 1 is reserved */
         ((h[1] >> 1) & 3) != 0 &&      /* the layer */
         (h[2] >> 4) != 15 &&           /* the bit rate */
         ((h[2] >> 2) & 3) != 3;        /* the sampling frequency */
}

/* the length of the frame of a header, 0: not told (the free format) */
static size_t
rtp_mpa_frame_len ( const uint8_t *h )
{
  static const uint16_t kbit[2][3][15] = {
    { { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },
      { 0, 32, 48, 56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 384 },
      { 0, 32, 40, 48,  56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320 } },
    { { 0, 32, 48, 56,  64,  80,  96, 112, 128, 144, 160, 176, 192, 224, 256 },
      { 0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160 },
      { 0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160 } }
  };
  static const uint32_t hz[3] = { 44100, 48000, 32000 };
  /* the version: 3 MPEG-1, 2 MPEG-2 (half the sampling frequencies), 0
   * MPEG-2.5 (a quarter) */
  static const uint8_t half[4] = { 2, 0, 1, 0 };
  const int ver = (h[1] >> 3) & 3;
  const int lsf = ver != 3;
  const int layer = 4 - ((h[1] >> 1) & 3);
  const uint32_t rate = hz[(h[2] >> 2) & 3] >> half[ver];
  const uint32_t bits = 1000 * (uint32_t)kbit[lsf][layer - 1][h[2] >> 4];
  const uint32_t pad = (h[2] >> 1) & 1;

  if (bits == 0)
    return 0;
  if (layer == 1)
    return (12 * bits / rate + pad) * 4;
  if (layer == 3 && lsf)
    return 72 * bits / rate + pad;
  return 144 * bits / rate + pad;
}

int
rtp_mpa_payload ( const uint8_t *d, size_t n, size_t *off, size_t *len, int *frag )
{
  size_t h;
  size_t end = n;

  if (n < 12 + 4 + 4 || (d[0] >> 6) != 2 || (d[1] & 0x7f) != RTP_MPA_PT)
    return 0;
  /* the RTP header: its CSRCs, the extension, the padding at the end */
  h = 12 + 4 * (size_t)(d[0] & 0x0f);
  if (d[0] & 0x10) {
    if (h + 4 > n)
      return 0;
    h += 4 + 4 * (((size_t)d[h + 2] << 8) | d[h + 3]);
  }
  if (d[0] & 0x20) {
    if (d[n - 1] == 0 || d[n - 1] > n)
      return 0;
    end = n - d[n - 1];
  }
  /* the header of RFC 2250: 16 bits that are zero, the fragment offset */
  if (h + 4 >= end || d[h] || d[h + 1])
    return 0;
  *frag = (d[h + 2] << 8) | d[h + 3];
  h += 4;
  if (*frag == 0 && (end - h < 4 || !rtp_mpa_frame(d + h)))
    return 0;
  *off = h;
  *len = end - h;
  return 1;
}

static uint32_t
rtp_mpa_crc ( const uint8_t *d, size_t n )
{
  uint32_t crc = 0xffffffff;
  int i;

  while (n--) {
    crc ^= (uint32_t)*d++ << 24;
    for (i = 0; i < 8; i++)
      crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04c11db7 : crc << 1;
  }
  return crc;
}

/* a section without its CRC-32 (n bytes) as the TS packet p */
static void
rtp_mpa_section ( uint8_t *p, int pid, uint8_t *s, size_t n )
{
  uint32_t crc;

  s[1] = (s[1] & 0xf0) | (((n + 4 - 3) >> 8) & 0x0f);
  s[2] = (n + 4 - 3) & 0xff;
  crc = rtp_mpa_crc(s, n);
  memset(p, 0xff, 188);
  p[0] = 0x47;
  p[1] = 0x40 | (pid >> 8);
  p[2] = pid & 0xff;
  p[3] = 0x10;
  p[4] = 0;                     /* the section begins here */
  memcpy(p + 5, s, n);
  p[5 + n] = crc >> 24;
  p[6 + n] = crc >> 16;
  p[7 + n] = crc >> 8;
  p[8 + n] = crc;
}

static void
rtp_mpa_build_pmt ( rtp_mpa_t *rm )
{
  uint8_t s[32];
  size_t n = 0;

  s[n++] = 0x02;
  s[n++] = 0xb0;
  s[n++] = 0;
  s[n++] = RTP_MPA_SID >> 8;
  s[n++] = RTP_MPA_SID & 0xff;
  s[n++] = 0xc1 | ((rm->pmt_version & 0x1f) << 1);
  s[n++] = 0;
  s[n++] = 0;
  s[n++] = 0xe0 | (RTP_MPA_PID_AUDIO >> 8);     /* the PCR */
  s[n++] = RTP_MPA_PID_AUDIO & 0xff;
  s[n++] = 0xf0;
  s[n++] = 0;
  s[n++] = rm->lsf > 0 ? 0x04 : 0x03;           /* ISO 13818-3 or 11172-3 audio */
  s[n++] = 0xe0 | (RTP_MPA_PID_AUDIO >> 8);
  s[n++] = RTP_MPA_PID_AUDIO & 0xff;
  s[n++] = 0xf0;
  s[n++] = 0;
  rtp_mpa_section(rm->pmt, RTP_MPA_PID_PMT, s, n);
}

static void
rtp_mpa_build_tables ( rtp_mpa_t *rm )
{
  uint8_t s[32 + RTP_MPA_NAME_MAX];
  const size_t l = strlen(rm->name);
  size_t n = 0;

  s[n++] = 0x00;
  s[n++] = 0xb0;
  s[n++] = 0;
  s[n++] = rm->tsid >> 8;
  s[n++] = rm->tsid & 0xff;
  s[n++] = 0xc1;
  s[n++] = 0;
  s[n++] = 0;
  s[n++] = RTP_MPA_SID >> 8;
  s[n++] = RTP_MPA_SID & 0xff;
  s[n++] = 0xe0 | (RTP_MPA_PID_PMT >> 8);
  s[n++] = RTP_MPA_PID_PMT & 0xff;
  rtp_mpa_section(rm->pat, 0, s, n);

  /* the SDT of this TS: a radio service that runs, with its name */
  n = 0;
  s[n++] = 0x42;
  s[n++] = 0xf0;
  s[n++] = 0;
  s[n++] = rm->tsid >> 8;
  s[n++] = rm->tsid & 0xff;
  s[n++] = 0xc1;
  s[n++] = 0;
  s[n++] = 0;
  s[n++] = RTP_MPA_ONID >> 8;
  s[n++] = RTP_MPA_ONID & 0xff;
  s[n++] = 0xff;
  s[n++] = RTP_MPA_SID >> 8;
  s[n++] = RTP_MPA_SID & 0xff;
  s[n++] = 0xfc;                                /* no EIT */
  s[n++] = 0x80;                                /* running, not scrambled */
  s[n++] = 5 + l;
  s[n++] = 0x48;                                /* the service descriptor */
  s[n++] = 3 + l;
  s[n++] = 0x02;                                /* digital radio sound service */
  s[n++] = 0;                                   /* no provider */
  s[n++] = l;
  memcpy(s + n, rm->name, l);
  n += l;
  rtp_mpa_section(rm->sdt, RTP_MPA_PID_SDT, s, n);
}

rtp_mpa_t *
rtp_mpa_create ( uint16_t tsid, const char *name, rtp_mpa_cb_t cb, void *opaque )
{
  rtp_mpa_t *rm = calloc(1, sizeof(*rm));

  if (rm == NULL)
    return NULL;
  rm->cb = cb;
  rm->opaque = opaque;
  rm->tsid = tsid;
  strncpy(rm->name, name, RTP_MPA_NAME_MAX);
  rm->lsf = -1;
  rtp_mpa_build_tables(rm);
  return rm;
}

void
rtp_mpa_destroy ( rtp_mpa_t *rm )
{
  free(rm);
}

static void
rtp_mpa_table ( rtp_mpa_t *rm, uint8_t *p, uint8_t *cc )
{
  p[3] = 0x10 | (*cc & 0x0f);
  (*cc)++;
  rm->cb(rm->opaque, p, 188);
}

/* the header of the PES packet of alen bytes of audio data, with the PTS, to
 * hdr */
#define RTP_MPA_PES_HEADER 14
static void
rtp_mpa_pes_header ( const rtp_mpa_t *rm, uint8_t *hdr, size_t alen )
{
  const uint64_t pts = rm->pts & 0x1ffffffffULL;

  hdr[0] = 0;
  hdr[1] = 0;
  hdr[2] = 1;
  hdr[3] = 0xc0;
  hdr[4] = (RTP_MPA_PES_HEADER - 6 + alen) >> 8;
  hdr[5] = (RTP_MPA_PES_HEADER - 6 + alen) & 0xff;
  hdr[6] = 0x84;                        /* a frame begins the data */
  hdr[7] = 0x80;
  hdr[8] = 5;
  hdr[9]  = 0x21 | ((pts >> 29) & 0x0e);
  hdr[10] = pts >> 22;
  hdr[11] = 0x01 | ((pts >> 14) & 0xfe);
  hdr[12] = pts >> 7;
  hdr[13] = 0x01 | ((pts << 1) & 0xfe);
}

/* the adaptation field of a TS packet (af bytes with the one of its length)
 * to p: with the PCR (8 bytes at least then), the rest is stuffing */
static void
rtp_mpa_adaptation ( rtp_mpa_t *rm, uint8_t *p, size_t af, int with_pcr )
{
  const uint64_t pcr = (rm->pts - RTP_MPA_PCR_AHEAD) & 0x1ffffffffULL;
  size_t o = 2;

  p[0] = af - 1;
  if (af == 1)
    return;
  p[1] = 0x00;
  if (with_pcr) {
    p[1] = 0x50 | (rm->discontinuity ? 0x80 : 0x00);
    rm->discontinuity = 0;
    p[o++] = pcr >> 25;
    p[o++] = pcr >> 17;
    p[o++] = pcr >> 9;
    p[o++] = pcr >> 1;
    p[o++] = ((pcr & 1) << 7) | 0x7e;
    p[o++] = 0;
  }
  memset(p + o, 0xff, af - o);
}

/* whole audio frames as a PES packet in the audio PID: it has their PTS,
 * its first TS packet the PCR */
static void
rtp_mpa_pes ( rtp_mpa_t *rm, const uint8_t *a, size_t alen )
{
  uint8_t hdr[RTP_MPA_PES_HEADER];
  uint8_t p[188];
  size_t hpos = 0;
  size_t left = RTP_MPA_PES_HEADER + alen;
  size_t n;
  size_t k;
  size_t af;
  int first = 1;

  rtp_mpa_pes_header(rm, hdr, alen);
  while (left > 0) {
    /* the adaptation field: the PCR, and what the last packet has too much */
    af = first ? 8 : 0;
    n = left < 184 - af ? left : 184 - af;
    af = 184 - n;
    p[0] = 0x47;
    p[1] = (first ? 0x40 : 0x00) | (RTP_MPA_PID_AUDIO >> 8);
    p[2] = RTP_MPA_PID_AUDIO & 0xff;
    p[3] = (af ? 0x30 : 0x10) | (rm->cc_audio++ & 0x0f);
    if (af)
      rtp_mpa_adaptation(rm, p + 4, af, first);
    /* of the PES header first */
    k = RTP_MPA_PES_HEADER - hpos < n ? RTP_MPA_PES_HEADER - hpos : n;
    memcpy(p + 4 + af, hdr + hpos, k);
    hpos += k;
    memcpy(p + 4 + af + k, a, n - k);
    a += n - k;
    left -= n;
    first = 0;
    rm->cb(rm->opaque, p, 188);
  }
}

/* a later part of the frame that began (the same time stamp, the offset
 * where the last one ended), else that frame is lost; the frame goes out
 * when it is whole */
static void
rtp_mpa_fragment ( rtp_mpa_t *rm, const uint8_t *a, size_t len, size_t frag, uint32_t ts )
{
  if (rm->flen == 0 || ts != rm->rtp_ts || frag != rm->have || len > rm->flen - rm->have) {
    rm->flen = 0;
    return;
  }
  memcpy(rm->frame + rm->have, a, len);
  rm->have += len;
  if (rm->have < rm->flen)
    return;
  rm->flen = 0;
  rtp_mpa_pes(rm, rm->frame, rm->have);
}

void
rtp_mpa_packet ( rtp_mpa_t *rm, const uint8_t *d, size_t n )
{
  size_t off;
  size_t len;
  size_t flen;
  uint32_t ts;
  uint32_t ssrc;
  int frag;
  int lsf;

  if (!rtp_mpa_payload(d, n, &off, &len, &frag))
    return;
  ts   = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7];
  ssrc = ((uint32_t)d[8] << 24) | ((uint32_t)d[9] << 16) | ((uint32_t)d[10] << 8) | d[11];
  if (rm->have_ts && ssrc != rm->ssrc) {
    rm->discontinuity = 1;
    rm->flen = 0;
  }
  rm->ssrc = ssrc;
  if (frag) {
    rtp_mpa_fragment(rm, d + off, len, frag, ts);
    return;
  }

  /* the time stamp wraps after 13 hours, the PTS after 26 */
  if (rm->have_ts)
    rm->pts += (int32_t)(ts - rm->rtp_ts);
  else
    rm->pts = ts;
  rm->rtp_ts = ts;
  lsf = ((d[off + 1] >> 3) & 3) != 3;
  if (lsf != rm->lsf) {
    if (rm->lsf >= 0)
      rm->pmt_version++;
    rm->lsf = lsf;
    rtp_mpa_build_pmt(rm);
    rm->have_ts = 0;            /* the tables now */
  }
  if (!rm->have_ts || rm->pts - rm->psi_pts >= RTP_MPA_PSI_STEP) {
    rtp_mpa_table(rm, rm->pat, &rm->cc_pat);
    rtp_mpa_table(rm, rm->pmt, &rm->cc_pmt);
    rtp_mpa_table(rm, rm->sdt, &rm->cc_sdt);
    rm->psi_pts = rm->pts;
  }
  rm->have_ts = 1;
  /* whole frames, or the first part of one that is larger than a packet
   * (of a frame in the free format the length is not known: its later parts
   * are left out) */
  flen = rtp_mpa_frame_len(d + off);
  rm->flen = 0;
  if (flen > len && flen <= RTP_MPA_FRAME_MAX) {
    memcpy(rm->frame, d + off, len);
    rm->have = len;
    rm->flen = flen;
    return;
  }
  rtp_mpa_pes(rm, d + off, len);
}
