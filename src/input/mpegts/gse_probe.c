/*
 *  Tvheadend - GSE probe: what a GSE stream carries, its DAB ensembles
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

/*
 * A GSE input stream of a multistream transponder (MM_TYPE_GSE) has no TS
 * and no tables: its scan is this probe. The GSE mux runs on the sub-mux
 * input, its stream is the BBFrames of its input stream as the Neumo driver
 * embeds them (DVB_BBFRAMES_PID); while the scan runs the probe reads them
 * (mm_secondary_cb), gse_stats takes the PDUs out of GSE and counts what
 * they are, the IPv4 ones go to the DAB probe as well (gse_dab_probe.c,
 * with libdvbdab). At the end what the stream carried stays with the mux
 * (dvb_mux_set_gse_content()); a stream of 188 byte user packets is no GSE
 * but a TS: the mux becomes a TS sub-mux.
 * The GSE mux of a transponder whose single input stream is GSE is the mux
 * of the transponder itself (mpegts_mux_gse_transponder()): its tuner
 * delivers the BBFrames, all of them are its stream. When the transponder
 * sends a TS or several input streams again, it becomes a TS mux again.
 */

#include "tvheadend.h"
#include "input.h"
#include "mpegts_dvb.h"
#include "bbframe.h"
#include "gse_stats_msg.h"
#include "dab/gse_dab_probe.h"

typedef struct gse_probe {
  mpegts_mux_t    *mm;
  /* the input thread, under mi_output_lock */
  bbframe_split_t  bs;
  gse_stats_t     *gs;
  gse_dab_probe_t *dab;
  int64_t          first, last;     /* mclk of the first and last packet */
} gse_probe_t;

static void
gse_probe_frame ( void *opaque, const uint8_t *frame, size_t len, int isi )
{
  gse_probe_t *p = opaque;
  gse_stats_frame(p->gs, frame, len, isi);
}

static void
gse_probe_pdu ( void *opaque, const gse_pdu_t *pdu )
{
  gse_probe_t *p = opaque;
  gse_dab_probe_pdu(p->dab, pdu);
}

/* the packets of the GSE mux (the input thread, under mi_output_lock) */
static void
gse_probe_packet_cb ( void *aux, const uint8_t *pkt, int len )
{
  gse_probe_t *p = aux;
  const uint8_t *t;
  int n;

  if (len < 188)
    return;
  p->last = mclk();
  if (p->first == 0)
    p->first = p->last;
  for (t = pkt, n = len; n >= 188; t += 188, n -= 188)
    if (t[0] == 0x47 && (((t[1] & 0x1f) << 8) | t[2]) == DVB_BBFRAMES_PID)
      bbframe_split_frames(&p->bs, t, gse_probe_frame, p);
}

static void
gse_probe_set_cb ( mpegts_mux_t *mm, gse_probe_t *p )
{
  mpegts_input_t *mi = mm->mm_active ? mm->mm_active->mmi_input : NULL;

  if (mi)
    tvh_mutex_lock(&mi->mi_output_lock);
  mm->mm_secondary_ctx = p;
  mm->mm_secondary_cb  = p ? gse_probe_packet_cb : NULL;
  if (mi)
    tvh_mutex_unlock(&mi->mi_output_lock);
}

/* the GSE mux of a stream of 188 byte user packets: a TS sub-mux, scanned
 * again (a tasklet: the scan of it ends now); the GSE mux of a transponder
 * that sends a TS or input streams again: a TS mux, as it was */
static void
gse_probe_to_ts_cb ( void *aux, int disarmed )
{
  char *uuid = aux;
  mpegts_mux_t *mm;

  if (!disarmed) {
    tvh_mutex_lock(&global_lock);
    mm = mpegts_mux_find(uuid);
    if (mm && mpegts_mux_gse_transponder(mm)) {
      tvhinfo(LS_MPEGTS, "%s - a TS or several input streams, not a single GSE "
              "stream: a TS mux again", mm->mm_nicename);
      mm->mm_type = MM_TYPE_TS;
      mm->mm_scan_result = MM_SCAN_NONE;
      dvb_mux_set_gse_content((dvb_mux_t *)mm, NULL);
      mpegts_mux_update_nice_name(mm);
      mpegts_network_scan_queue_add(mm, SUBSCRIPTION_PRIO_SCAN_INIT,
                                    SUBSCRIPTION_INITSCAN, 10);
    } else if (mm && mm->mm_type == MM_TYPE_GSE) {
      tvhinfo(LS_MPEGTS, "%s - a TS stream (188 byte user packets), not GSE: "
              "a TS sub-mux now", mm->mm_nicename);
      mm->mm_type = MM_TYPE_ISI;
      mm->mm_scan_result = MM_SCAN_NONE;
      dvb_mux_set_gse_content((dvb_mux_t *)mm, NULL);
      mpegts_mux_update_nice_name(mm);
      mpegts_network_scan_queue_add(mm, SUBSCRIPTION_PRIO_SCAN_INIT,
                                    SUBSCRIPTION_INITSCAN, 10);
    }
    tvh_mutex_unlock(&global_lock);
  }
  free(uuid);
}

/* frames of input streams (multistream) among the frames */
static int
gse_probe_has_isis ( const gse_stats_result_t *r )
{
  size_t i;

  for (i = 0; i < sizeof(r->isis); i++)
    if (r->isis[i])
      return 1;
  return 0;
}

/* the scan of a GSE mux starts */
void
mpegts_gse_probe_start ( mpegts_mux_t *mm )
{
  gse_probe_t *p;

  if (mm->mm_type != MM_TYPE_GSE || mm->mm_gse_probe_ctx ||
      mm->mm_active == NULL || mm->mm_secondary_cb ||
      !idnode_is_instance(&mm->mm_id, &dvb_mux_class))
    return;
  if ((p = calloc(1, sizeof(*p))) == NULL)
    return;
  if ((p->gs = gse_stats_create()) == NULL) {
    free(p);
    return;
  }
  bbframe_split_init(&p->bs, ((dvb_mux_t *)mm)->lm_tuning.dmc_fe_stream_id);
  p->mm = mm;
  if ((p->dab = gse_dab_probe_create(mm)) != NULL)
    gse_stats_tap(p->gs, gse_probe_pdu, p);
  mm->mm_gse_probe_ctx = p;
  gse_probe_set_cb(mm, p);
  /* the mux of the transponder itself: its tuner opens the PID of the
   * BBFrames for the probe (a sub-mux gets them from the sub-mux input) */
  if (mpegts_mux_gse_transponder(mm)) {
    mm->mm_update_pids_flag = 1;
    mpegts_mux_update_pids(mm);
  }
  tvhdebug(LS_MPEGTS, "%s - GSE probe started", mm->mm_nicename);
}

/* what the frames of the stream say: the mux is no GSE mux (any more), it is
 * made a TS mux then (gse_probe_to_ts_cb()) */
static int
gse_probe_not_gse ( mpegts_mux_t *mm, const gse_stats_result_t *r )
{
  const uint64_t frames = r->frames[0] + r->frames[1] + r->frames[2] + r->frames[3];
  char ubuf[UUID_HEX_SIZE];
  int ts;

  if (frames == 0)
    return 0;
  /* not GSE: a generic stream of 188 byte user packets is a TS one (16E
   * 12677.964V: generic continuous, UPL 1504, PAT, PMT, SDT). The mux of
   * a transponder stays GSE with such a stream (as it is tuned it had no
   * tables, that made it one); it is a TS mux again when most frames are
   * TS ones, or frames are of input streams (a multistream transponder
   * now: the ISI probe of its scan as a TS mux finds them) */
  if (mpegts_mux_gse_transponder(mm))
    ts = r->frames[3] * 2 > frames || gse_probe_has_isis(r);
  else
    ts = r->frames_upl * 2 > frames && r->upl == 188 * 8;
  if (!ts)
    return 0;
  tasklet_arm_alloc(gse_probe_to_ts_cb, strdup(idnode_uuid_as_str(&mm->mm_id, ubuf)));
  return 1;
}

/* what the stream carried (m: the message of it, NULL none) in the log, and
 * with the mux when keep */
static void
gse_probe_content ( mpegts_mux_t *mm, htsmsg_t *m, int keep )
{
  char buf[256];

  if (m) {
    gse_stats_msg_text(m, buf, sizeof(buf));
    tvhinfo(LS_MPEGTS, "%s - %s", mm->mm_nicename, buf);
    if (keep)
      dvb_mux_set_gse_content((dvb_mux_t *)mm, m);
    else
      htsmsg_destroy(m);
  } else if (keep) {
    tvhinfo(LS_MPEGTS, "%s - no BBFrames of the stream", mm->mm_nicename);
    dvb_mux_set_gse_content((dvb_mux_t *)mm, NULL);
  }
}

/* the scan of the GSE mux ended: what the stream carried (when evaluate,
 * not for a cancelled scan) stays with the mux; the DAB ensembles found,
 * *content: the stream carried GSE PDUs */
int
mpegts_gse_probe_complete ( mpegts_mux_t *mm, int evaluate, int *content )
{
  gse_probe_t *p = mm->mm_gse_probe_ctx;
  gse_stats_result_t *r;
  htsmsg_t *m = NULL;
  int found;

  *content = 0;
  if (p == NULL)
    return 0;
  mm->mm_gse_probe_ctx = NULL;

  /* the input thread is not inside the probe after this */
  if (mm->mm_secondary_ctx == p)
    gse_probe_set_cb(mm, NULL);
  if (mpegts_mux_gse_transponder(mm) && mm->mm_active) {
    mm->mm_update_pids_flag = 1;
    mpegts_mux_update_pids(mm);
  }

  r = evaluate ? malloc(sizeof(*r)) : NULL;
  if (r != NULL) {
    gse_stats_get(p->gs, r);
    m = gse_stats_msg(r);
    if (m != NULL)
      htsmsg_add_s64(m, "us", p->last - p->first);
    if (gse_probe_not_gse(mm, r))
      evaluate = 0;
    *content = evaluate && r->gse.pdus > 0;
    free(r);
  }
  gse_probe_content(mm, m, evaluate);

  found = gse_dab_probe_finish(p->dab, evaluate);
  gse_stats_destroy(p->gs);
  free(p);
  return found;
}
