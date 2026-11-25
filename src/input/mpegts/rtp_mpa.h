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

#ifndef __TVH_RTP_MPA_H__
#define __TVH_RTP_MPA_H__

#include <stddef.h>
#include <stdint.h>

/*
 * MPEG audio in RTP (RFC 2250; the static payload type 14 of RFC 3551: layer
 * I, II or III, a clock of 90 kHz) as a transport stream with one radio
 * service: the frames of an RTP packet are a PES packet (a frame that comes
 * in several packets when all of them are there), the RTP time stamp its
 * PTS and, a little earlier, the PCR; a PAT, a PMT and an SDT go with it.
 * Plain computation: no tvheadend state, no locks.
 */

#define RTP_MPA_PT 14

/* a UDP payload (n bytes) is such an RTP packet: version 2, payload type 14,
 * the header of RFC 2250 and - where a frame begins, at fragment offset 0 -
 * the header of an audio frame. *off and *len: the audio data; *frag: its
 * offset in the frame it is of (0: the packet begins with a frame).
 * 0: it is none */
int rtp_mpa_payload ( const uint8_t *d, size_t n, size_t *off, size_t *len, int *frag );

typedef void (*rtp_mpa_cb_t) ( void *opaque, const uint8_t *ts, size_t len );

typedef struct rtp_mpa rtp_mpa_t;

/* tsid: of the transport stream; name: of its service (RTP has none) */
rtp_mpa_t *rtp_mpa_create ( uint16_t tsid, const char *name, rtp_mpa_cb_t cb, void *opaque );
void       rtp_mpa_destroy ( rtp_mpa_t *rm );

/* a UDP payload: if it is such an RTP packet, the TS packets of its frames
 * go to cb (188 bytes a call). A frame with a part missing is left out */
void       rtp_mpa_packet ( rtp_mpa_t *rm, const uint8_t *d, size_t n );

#endif /* __TVH_RTP_MPA_H__ */
