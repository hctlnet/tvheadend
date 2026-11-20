/*
 *  Tvheadend - instant channel switching, per mux ring buffer
 *  Copyright (C) 2026 L-S-D
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "dvbbuffer_private.h"

static mpegts_listener_t dvbbuffer_mux_listener;
static int dvbbuffer_mux_rings;     /* atomic */

/* kept warm (prebuffer) */
int
dvbbuffer_mux_prebuffer(mpegts_mux_t *mm)
{
  return dvbbuffer_running && dvbbuffer_conf.enabled && mm->mm_prebuffer &&
         mm->mm_is_enabled(mm);
}

/* ring buffer wanted: kept warm */
int
dvbbuffer_mux_wanted(mpegts_mux_t *mm)
{
  return dvbbuffer_mux_prebuffer(mm);
}

/*
 * Upper bound of the TS bitrate of a mux from its tuning parameters, kbit/s;
 * 0 = unknown (AUTO, other delivery systems): the configured maximum then
 */
static uint32_t
dvbbuffer_fec(int fec, double *rate)
{
  int num = fec / 100, den = fec % 100;

  if (fec <= DVB_FEC_AUTO || num <= 0 || den <= 0 || num >= den)
    return 0;
  *rate = (double)num / den;
  return 1;
}

static int
dvbbuffer_bits_per_symbol(int mod)
{
  switch (mod) {
  case DVB_MOD_QPSK:     return 2;
  case DVB_MOD_PSK_8:    return 3;
  case DVB_MOD_APSK_16:  return 4;
  case DVB_MOD_APSK_32:  return 5;
  case DVB_MOD_QAM_16:   return 4;
  case DVB_MOD_QAM_32:   return 5;
  case DVB_MOD_QAM_64:   return 6;
  case DVB_MOD_QAM_128:  return 7;
  case DVB_MOD_QAM_256:  return 8;
  default:               return 0;
  }
}

/* DVB-S2 normal frame: BCH information bits per code rate */
static int
dvbbuffer_s2_kbch(int fec)
{
  switch (fec) {
  case DVB_FEC_1_4:  return 16008;
  case DVB_FEC_1_3:  return 21408;
  case DVB_FEC_2_5:  return 25728;
  case DVB_FEC_1_2:  return 32208;
  case DVB_FEC_3_5:  return 38688;
  case DVB_FEC_2_3:  return 43040;
  case DVB_FEC_3_4:  return 48408;
  case DVB_FEC_4_5:  return 51648;
  case DVB_FEC_5_6:  return 53840;
  case DVB_FEC_8_9:  return 57472;
  case DVB_FEC_9_10: return 58192;
  default:           return 0;
  }
}

uint32_t
dvbbuffer_mux_bitrate(mpegts_mux_t *mm)
{
  const dvb_mux_conf_t *dmc;
  double fec, bps;
  int bits, kbch, guard, carriers;
  double tu;

  if (!idnode_is_instance(&mm->mm_id, &dvb_mux_dvbs_class) &&
      !idnode_is_instance(&mm->mm_id, &dvb_mux_dvbc_class) &&
      !idnode_is_instance(&mm->mm_id, &dvb_mux_dvbt_class))
    return 0;
  dmc = &((dvb_mux_t *)mm)->lm_tuning;
  switch (dmc->dmc_fe_delsys) {
  case DVB_SYS_DVBS:
    if (!dvbbuffer_fec(dmc->u.dmc_fe_qpsk.fec_inner, &fec))
      return 0;
    bps = (double)dmc->u.dmc_fe_qpsk.symbol_rate * 2 * fec * 188 / 204;
    break;
  case DVB_SYS_DVBS2:
    /* useful rate of normal frames without pilots (an upper bound): BCH bits
     * less the base band header per frame of 64800 / bits + 90 symbols;
     * the roll-off does not change it */
    bits = dvbbuffer_bits_per_symbol(dmc->dmc_fe_modulation);
    kbch = dvbbuffer_s2_kbch(dmc->u.dmc_fe_qpsk.fec_inner);
    if (!bits || !kbch)
      return 0;
    bps = (double)dmc->u.dmc_fe_qpsk.symbol_rate * (kbch - 80) / (64800 / bits + 90);
    break;
  case DVB_SYS_DVBC_ANNEX_A:
  case DVB_SYS_DVBC_ANNEX_C:
    bits = dvbbuffer_bits_per_symbol(dmc->dmc_fe_modulation);
    if (!bits)
      return 0;
    bps = (double)dmc->u.dmc_fe_qam.symbol_rate * bits * 188 / 204;
    break;
  case DVB_SYS_DVBT:
    /* data carriers x bits x code rate x RS / symbol time incl. guard */
    bits = dvbbuffer_bits_per_symbol(dmc->dmc_fe_modulation);
    if (!bits || !dvbbuffer_fec(dmc->u.dmc_fe_ofdm.code_rate_HP, &fec) ||
        dmc->u.dmc_fe_ofdm.bandwidth < 1000)
      return 0;
    switch (dmc->u.dmc_fe_ofdm.transmission_mode) {
    case DVB_TRANSMISSION_MODE_2K: carriers = 1512; tu = 224e-6; break;
    case DVB_TRANSMISSION_MODE_8K: carriers = 6048; tu = 896e-6; break;
    default: return 0;
    }
    guard = dmc->u.dmc_fe_ofdm.guard_interval;
    if (guard < DVB_GUARD_INTERVAL_1_4 || guard > DVB_GUARD_INTERVAL_1_32)
      return 0;
    tu *= 8000.0 / dmc->u.dmc_fe_ofdm.bandwidth;
    bps = carriers * bits * fec * 188 / 204 / (tu * (1 + 1.0 / (guard - 1000)));
    break;
  default:
    return 0;
  }
  return (uint32_t)(bps / 1000) + 1;
}

static void
dvbbuffer_mux_free(dvbbuffer_mux_t *dm)
{
  memoryinfo_free(&dvbbuffer_memoryinfo, dvbbuffer_ring_memory(dm->dm_ring));
  dvbbuffer_ring_destroy(dm->dm_ring);
  free(dm->dm_name);
  free(dm);
}

/*
 * Create the ring buffer of a running mux and publish it to the input thread
 */
void
dvbbuffer_mux_attach(mpegts_mux_t *mm)
{
  mpegts_mux_instance_t *mmi = mm->mm_active;
  mpegts_input_t *mi;
  dvbbuffer_mux_t *dm;
  uint32_t kbps;

  lock_assert(&global_lock);

  if (mm->mm_dvbbuffer || mmi == NULL || !dvbbuffer_mux_wanted(mm))
    return;
  mi = mmi->mmi_input;

  dm = calloc(1, sizeof(*dm));
  if (dm == NULL)
    return;
  dm->dm_name = strdup(mm->mm_nicename ?: "?");

  kbps = dvbbuffer_mux_bitrate(mm);
  if (kbps == 0 || kbps > dvbbuffer_conf.max_bitrate_kbps)
    kbps = dvbbuffer_conf.max_bitrate_kbps;
  dm->dm_ring = dvbbuffer_ring_create(dvbbuffer_conf.buffer_sec * 1000, kbps, dm->dm_name);
  if (dm->dm_ring == NULL) {
    tvherror(LS_DVBBUFFER, "%s: unable to create ring buffer (out of memory)", dm->dm_name);
    free(dm->dm_name);
    free(dm);
    return;
  }
  memoryinfo_alloc(&dvbbuffer_memoryinfo, dvbbuffer_ring_memory(dm->dm_ring));

  tvh_mutex_lock(&mi->mi_output_lock);
  mm->mm_dvbbuffer = dm;
  tvh_mutex_unlock(&mi->mi_output_lock);
  atomic_add(&dvbbuffer_mux_rings, 1);

  tvhinfo(LS_DVBBUFFER, "%s: ring buffer started (%u s, %u kbit/s%s)",
          dm->dm_name, dvbbuffer_conf.buffer_sec, kbps,
          kbps == dvbbuffer_conf.max_bitrate_kbps ? ", configured maximum" : " from tuning");
}

int
dvbbuffer_mux_count(void)
{
  return atomic_get(&dvbbuffer_mux_rings);
}

/*
 * Stop feeding and release the ring buffer
 */
void
dvbbuffer_mux_detach(mpegts_mux_t *mm)
{
  mpegts_mux_instance_t *mmi = mm->mm_active;
  dvbbuffer_mux_t *dm = mm->mm_dvbbuffer;
  mpegts_input_t *mi;

  lock_assert(&global_lock);

  if (dm == NULL)
    return;
  if (mmi) {
    mi = mmi->mmi_input;
    tvh_mutex_lock(&mi->mi_output_lock);
    mm->mm_dvbbuffer = NULL;
    tvh_mutex_unlock(&mi->mi_output_lock);
  } else {
    /* mpegts_mux_stop() cleared mm_active and the PIDs under
     * mi_output_lock before the stop event, the input thread
     * does not touch this mux any more */
    mm->mm_dvbbuffer = NULL;
  }
  atomic_dec(&dvbbuffer_mux_rings, 1);
  tvhinfo(LS_DVBBUFFER, "%s: ring buffer stopped", dm->dm_name);
  dvbbuffer_mux_free(dm);
}

/*
 * The configuration changed (global_lock): rings no longer wanted go at
 * once. Instant zapping disabled: also those without a warm entry (a
 * prebuffer mux over the maximum, tuned for a viewer) - the reconciler
 * takes down only the warm entries, such a ring would stay until the mux
 * stops, its backlog going out without the live start.
 */
void
dvbbuffer_mux_conf_changed(void)
{
  mpegts_network_t *mn;
  mpegts_mux_t *mm;

  lock_assert(&global_lock);
  LIST_FOREACH(mn, &mpegts_network_all, mn_global_link)
    LIST_FOREACH(mm, &mn->mn_muxes, mm_network_link)
      if (mm->mm_dvbbuffer && !dvbbuffer_mux_wanted(mm))
        dvbbuffer_mux_detach(mm);
}

/*
 * Ring before the input processing (mpegts_input_process(), input thread,
 * mi_output_lock held): the chunk goes into the ring; 1 = tvh processes
 * it from the ring (dvbbuffer_mux_span()), 0 = from the chunk itself (it
 * is too large for the ring to keep it whole until it is processed)
 */
int
dvbbuffer_mux_feed0(mpegts_mux_t *mm, const uint8_t *tsb, int len,
                    int cc_restart, dvbbuffer_span_t *sp)
{
  dvbbuffer_ring_t *r = mm->mm_dvbbuffer->dm_ring;

  if (cc_restart)
    dvbbuffer_ring_mark_gap(r);
  sp->seq = dvbbuffer_ring_head(r);
  dvbbuffer_ring_feed(r, tsb, len, getfastmonoclock());
  sp->end = dvbbuffer_ring_head(r);
  return sp->end - sp->seq <= dvbbuffer_ring_capacity(r) / 2;
}

/* the next piece of the stored chunk in one piece of ring memory: its
   length in bytes, 0 = done */
int
dvbbuffer_mux_span(mpegts_mux_t *mm, dvbbuffer_span_t *sp, const uint8_t **data)
{
  uint32_t n = dvbbuffer_ring_span(mm->mm_dvbbuffer->dm_ring, sp->seq, sp->end, data);

  sp->seq += n;
  return (int)n * 188;  /* n <= ring capacity <= INT_MAX / 188 (ring.c) */
}

/*
 * The mux property "prebuffer" changed
 */
void
dvbbuffer_mux_prebuffer_notify(void *p, const char *lang)
{
  mpegts_mux_t *mm = p;

  if (!dvbbuffer_running)
    return;
  if (dvbbuffer_mux_wanted(mm))
    dvbbuffer_mux_attach(mm);
  else
    dvbbuffer_mux_detach(mm);
}

/*
 * Mux events (global_lock)
 */
static void
dvbbuffer_mux_start_cb(mpegts_mux_t *mm, void *p)
{
  dvbbuffer_mux_attach(mm);
}

static void
dvbbuffer_mux_stop_cb(mpegts_mux_t *mm, void *p, int reason)
{
  dvbbuffer_mux_detach(mm);
}

static void
dvbbuffer_mux_delete_cb(mpegts_mux_t *mm, void *p)
{
  dvbbuffer_mux_detach(mm);
}

void
dvbbuffer_mux_init(void)
{
  dvbbuffer_mux_listener.ml_mux_start  = dvbbuffer_mux_start_cb;
  dvbbuffer_mux_listener.ml_mux_stop   = dvbbuffer_mux_stop_cb;
  dvbbuffer_mux_listener.ml_mux_delete = dvbbuffer_mux_delete_cb;
  mpegts_add_listener(&dvbbuffer_mux_listener);
}

/*
 * Shutdown (global_lock): stop all ring buffers
 */
void
dvbbuffer_mux_done(void)
{
  mpegts_network_t *mn;
  mpegts_mux_t *mm;

  lock_assert(&global_lock);

  mpegts_rem_listener(&dvbbuffer_mux_listener);
  LIST_FOREACH(mn, &mpegts_network_all, mn_global_link)
    LIST_FOREACH(mm, &mn->mn_muxes, mm_network_link)
      dvbbuffer_mux_detach(mm);
}
