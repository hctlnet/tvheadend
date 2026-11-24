/*
 *  Tvheadend - instant channel switching, warm mux manager
 *
 *  Every mux marked "prebuffer" gets a low-weight full-mux subscription so
 *  it stays tuned and its ring buffer filled. Real viewers (higher weight)
 *  take the tuner when they need it; tvh's subscription scheduler brings
 *  the warm mux back as soon as a tuner is free again.
 *
 *  LRU: the DVB muxes last used by viewers (service starts) are
 *  kept warm as well, up to lru_max, with a lower weight (lru_weight). A
 *  used mux gets its warm subscription and ring buffer while it is being
 *  watched, so it stays tuned with a full backlog when the viewer leaves.
 *
 *  A warm subscription never moves a dish (SUBSCRIPTION_NOMOVE): a mux
 *  behind a rotor is kept warm only while the dish stands at its position.
 *  The inputs answer for it as for busy tuners, so it is tried again later.
 *
 *  Copyright (C) 2026 L-S-D
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "dvbbuffer_private.h"
#include "subscriptions.h"
#include "streaming.h"
#include "profile.h"

typedef struct dvbbuffer_warm {
  LIST_ENTRY(dvbbuffer_warm) dw_link;
  mpegts_mux_t       *dw_mux;
  profile_chain_t     dw_prch;
  streaming_target_t  dw_input;
  th_subscription_t  *dw_sub;
  int                 dw_mark;
  int                 dw_lru;       /* kept warm as recently used */
  int                 dw_busy;      /* no free tuner: subscription released */
  int                 dw_failed;    /* ... or it does not start (no signal ...) */
  int64_t             dw_retry;     /* mclk() of the next try while busy */
  int                 dw_backoff;   /* current wait while busy, seconds */
  int64_t             dw_running;   /* mclk() when last seen running (or
                                       subscribed) */
} dvbbuffer_warm_t;

/* no tuner for it now (all busy with scans or viewers, or its dish stands
 * elsewhere): wait before the next try, doubled up to the maximum while it
 * keeps failing */
#define DVBBUFFER_WARM_BUSY_MIN  30
#define DVBBUFFER_WARM_BUSY_MAX  300
/* the tuner of the mux is being set up for someone else (switch, rotor):
 * looked at this often (s) until the mux can be joined */
#define DVBBUFFER_WARM_SETUP     2
/* a warm subscription takes the whole mux and never moves a dish */
#define DVBBUFFER_WARM_FLAGS     (SUBSCRIPTION_MINIMAL | SUBSCRIPTION_NOMOVE)
/* a warm subscription not running for this long (s) failed: tvh retries a
 * failed tuning without a word (no SMT_NOSTART); checked this often (s) */
#define DVBBUFFER_WARM_STUCK     60
#define DVBBUFFER_WARM_CHECK     30

/* mux use by viewers, for the LRU (global_lock) */
typedef struct dvbbuffer_used {
  LIST_ENTRY(dvbbuffer_used) du_link;
  mpegts_mux_t       *du_mux;
  int                 du_users;     /* services now */
  int64_t             du_last;      /* mclk() of the last start or stop */
  int                 du_sel;       /* among the lru_max kept warm */
  int                 du_dead;      /* no input: not kept warm until used again */
} dvbbuffer_used_t;

static LIST_HEAD(, dvbbuffer_warm) dvbbuffer_warm_all;
static LIST_HEAD(, dvbbuffer_used) dvbbuffer_used_all;
static mtimer_t dvbbuffer_warm_timer;
static mtimer_t dvbbuffer_warm_freed_timer;

static void dvbbuffer_warm_lru_dead(mpegts_mux_t *mm);
static void dvbbuffer_warm_reconcile_cb(void *aux);

/*
 * Subscription output: the data itself is taken in mpegts_input_process()
 */
static void
dvbbuffer_warm_input(void *opaque, streaming_message_t *sm)
{
  dvbbuffer_warm_t *dw = opaque;

  /* SMT_START comes from the input thread: the reconciler sees the
   * subscription running (dvbbuffer_warm_check()) */
  switch (sm->sm_type) {
  case SMT_STOP:
  case SMT_NOSTART:
    tvhdebug(LS_DVBBUFFER, "%s: warm subscription %s (%s)",
             dw->dw_mux->mm_nicename,
             sm->sm_type == SMT_STOP ? "stopped" : "not started",
             streaming_code2txt(sm->sm_code));
    /* tuned, but no data (transponder gone): a recently used mux is
     * dropped instead of being retried every 2 s */
    if (sm->sm_type == SMT_NOSTART && sm->sm_code == SM_CODE_NO_INPUT && dw->dw_lru)
      dvbbuffer_warm_lru_dead(dw->dw_mux);
    /* all tuners taken by higher weights (scan, viewers), or the dish of
     * the mux stands elsewhere: tvh would retry every 2 s (log line each
     * time) and grab every tuner freed between two scan steps only to lose
     * it again - the reconciler releases the subscription and tries again
     * later, not this callback */
    else if (sm->sm_type == SMT_NOSTART && sm->sm_code == SM_CODE_NO_FREE_ADAPTER && !dw->dw_busy) {
      dw->dw_busy = 1;
      dw->dw_failed = 0;
      dvbbuffer_warm_reconcile();
    }
    /* does not start for another reason (tuning failed, no signal, no
     * input): tvh would retry every 2 s and take a tuner each time - the
     * same backoff as for busy tuners, but no retry when a tuner is freed */
    else if (sm->sm_type == SMT_NOSTART && !dw->dw_busy) {
      dw->dw_busy = 1;
      dw->dw_failed = 1;
      dvbbuffer_warm_reconcile();
    }
    break;
  default:
    break;
  }
  streaming_msg_free(sm);
}

static htsmsg_t *
dvbbuffer_warm_input_info(void *opaque, htsmsg_t *list)
{
  htsmsg_add_str(list, NULL, "dvbbuffer warm mux input");
  return list;
}

static streaming_ops_t dvbbuffer_warm_input_ops = {
  .st_cb   = dvbbuffer_warm_input,
  .st_info = dvbbuffer_warm_input_info
};

static inline uint32_t
dvbbuffer_warm_weight(int lru)
{
  return lru ? dvbbuffer_conf.lru_weight : dvbbuffer_conf.warm_weight;
}

static inline const char *
dvbbuffer_warm_title(int lru)
{
  return lru ? "dvbbuffer LRU" : "dvbbuffer";
}

static void
dvbbuffer_warm_destroy(dvbbuffer_warm_t *dw)
{
  mpegts_mux_t *mm = dw->dw_mux;

  /* the recently used muxes come and go with every zap: debug */
  if (dw->dw_lru)
    tvhdebug(LS_DVBBUFFER, "%s: recently used mux released", mm->mm_nicename);
  else
    tvhinfo(LS_DVBBUFFER, "%s: warm mux released", mm->mm_nicename);
  LIST_REMOVE(dw, dw_link);
  if (dw->dw_sub)
    subscription_unsubscribe(dw->dw_sub, UNSUBSCRIBE_FINAL);
  free(dw);
  /* still tuned for others (viewers) - the ring buffer may go */
  if (!dvbbuffer_mux_wanted(mm))
    dvbbuffer_mux_detach(mm);
}

/* the full-mux subscription of an entry; 0 = ok */
static int
dvbbuffer_warm_subscribe(dvbbuffer_warm_t *dw)
{
  mpegts_mux_t *mm = dw->dw_mux;
  mpegts_service_t *ms;
  mpegts_apids_t pids;

  dw->dw_sub = subscription_create_from_mux(&dw->dw_prch, NULL,
                                            dvbbuffer_warm_weight(dw->dw_lru),
                                            dvbbuffer_warm_title(dw->dw_lru),
                                            DVBBUFFER_WARM_FLAGS,
                                            NULL, NULL, NULL, NULL);
  if (dw->dw_sub == NULL) {
    tvherror(LS_DVBBUFFER, "%s: unable to create warm subscription",
             mm->mm_nicename);
    return -1;
  }

  /* the whole transport stream */
  ms = (mpegts_service_t *)dw->dw_sub->ths_service;
  /* the data itself is taken in mpegts_input_process(): no copy of the
   * whole stream for the subscription (ts_recv_raw() remuxes only when a
   * target takes it) */
  tvh_mutex_lock(&ms->s_stream_mutex);
  streaming_target_set_reject_filter(&dw->dw_sub->ths_input,
                                     SMT_TO_MASK(SMT_MPEGTS) | SMT_TO_MASK(SMT_PACKET));
  tvh_mutex_unlock(&ms->s_stream_mutex);
  mpegts_pid_init(&pids);
  pids.all = 1;
  ms->s_update_pids(ms, &pids);
  mpegts_pid_done(&pids);
  /* a running mux (in use) gets its ring buffer now */
  dvbbuffer_mux_attach(mm);
  dw->dw_running = mclk();
  return 0;
}

/*
 * The inputs of the mux for a warm subscription now: 1 = one takes it (or
 * none is known yet), 0 = none - the tuner the mux runs on is still being
 * set up for someone else (switch, rotor: *setup = 1), or the dish of the
 * mux stands elsewhere. Asked before another try: a subscription would
 * only fail, with a log line
 */
static int
dvbbuffer_warm_input_ready(dvbbuffer_warm_t *dw, int *setup)
{
  mpegts_mux_t *mm = dw->dw_mux;
  mpegts_mux_instance_t *mmi;
  int n = 0;

  *setup = 0;
  LIST_FOREACH(mmi, &mm->mm_instances, mmi_mux_link) {
    n++;
    if (mmi->mmi_input->mi_is_enabled(mmi->mmi_input, mm, DVBBUFFER_WARM_FLAGS,
                                      dvbbuffer_warm_weight(dw->dw_lru)) == MI_IS_ENABLED_OK)
      return 1;
  }
  *setup = mm->mm_active != NULL;
  return n == 0;
}

/* no input for the mux now: the next look, without a subscription */
static void
dvbbuffer_warm_wait(dvbbuffer_warm_t *dw, int setup, int64_t now)
{
  dw->dw_busy = 1;
  dw->dw_failed = 0;
  if (setup) {
    dw->dw_retry = now + sec2mono(DVBBUFFER_WARM_SETUP);
    return;
  }
  dw->dw_backoff = MIN(MAX(dw->dw_backoff * 2, DVBBUFFER_WARM_BUSY_MIN),
                       DVBBUFFER_WARM_BUSY_MAX);
  dw->dw_retry = now + sec2mono(dw->dw_backoff);
  tvhdebug(LS_DVBBUFFER, "%s: no input for it now, %s mux tried again in %d s",
           dw->dw_mux->mm_nicename, dw->dw_lru ? "recently used" : "warm", dw->dw_backoff);
}

static void
dvbbuffer_warm_create(mpegts_mux_t *mm, int lru)
{
  dvbbuffer_warm_t *dw;

  if ((dw = calloc(1, sizeof(*dw))) == NULL)
    return;
  dw->dw_mux = mm;
  dw->dw_mark = 1;
  dw->dw_lru = lru;
  streaming_target_init(&dw->dw_input, &dvbbuffer_warm_input_ops, dw, 0);
  dw->dw_prch.prch_id = mm;
  dw->dw_prch.prch_st = &dw->dw_input;
  LIST_INSERT_HEAD(&dvbbuffer_warm_all, dw, dw_link);

  if (dvbbuffer_warm_subscribe(dw)) {
    LIST_REMOVE(dw, dw_link);
    free(dw);
    return;
  }
  if (lru)
    tvhdebug(LS_DVBBUFFER, "%s: recently used mux (weight %u)", mm->mm_nicename,
             dvbbuffer_warm_weight(lru));
  else
    tvhinfo(LS_DVBBUFFER, "%s: warm mux (weight %u)", mm->mm_nicename,
            dvbbuffer_warm_weight(lru));
}

/*
 * A busy entry (reconciler, global_lock): the subscription of a newly busy
 * one is released, and when its wait is over it subscribes again. It is
 * not busy any more after that, or it has the time of its next try.
 */
static void
dvbbuffer_warm_busy_entry(dvbbuffer_warm_t *dw, int64_t now)
{
  int setup;

  if (dw->dw_sub) {
    subscription_unsubscribe(dw->dw_sub, UNSUBSCRIBE_FINAL);
    dw->dw_sub = NULL;
    /* the mux is being tuned for a viewer and its tuner is not set up
     * yet (a rotor on its way): joined when that is done */
    if (!dw->dw_failed && !dvbbuffer_warm_input_ready(dw, &setup) && setup) {
      dvbbuffer_warm_wait(dw, 1, now);
      tvhdebug(LS_DVBBUFFER, "%s: its tuner is being set up, %s mux joins it then",
               dw->dw_mux->mm_nicename, dw->dw_lru ? "recently used" : "warm");
      return;
    }
    dw->dw_backoff = dw->dw_backoff ?
      MIN(dw->dw_backoff * 2, DVBBUFFER_WARM_BUSY_MAX) : DVBBUFFER_WARM_BUSY_MIN;
    dw->dw_retry = now + sec2mono(dw->dw_backoff);
    /* the first wait of a busy period: info, the longer ones: debug */
    tvhlog(dw->dw_backoff == DVBBUFFER_WARM_BUSY_MIN ? LOG_INFO : LOG_DEBUG,
           LS_DVBBUFFER, "%s: %s, %s mux tried again in %d s",
           dw->dw_mux->mm_nicename, dw->dw_failed ? "does not start" : "no tuner for it now",
           dw->dw_lru ? "recently used" : "warm", dw->dw_backoff);
    return;
  }
  if (dw->dw_retry > now)
    return;
  if (!dvbbuffer_warm_input_ready(dw, &setup)) {
    dvbbuffer_warm_wait(dw, setup, now);
    return;
  }
  dw->dw_busy = 0;
  tvhdebug(LS_DVBBUFFER, "%s: trying again to keep the mux warm",
           dw->dw_mux->mm_nicename);
  if (dvbbuffer_warm_subscribe(dw) == 0)
    return;
  /* no subscription possible: like busy, next wait */
  dw->dw_busy = 1;
  dw->dw_backoff = MIN(MAX(dw->dw_backoff * 2, DVBBUFFER_WARM_BUSY_MIN),
                       DVBBUFFER_WARM_BUSY_MAX);
  dw->dw_retry = now + sec2mono(dw->dw_backoff);
}

/* the busy entries; returns the mclk() of the next retry, 0 = none */
static int64_t
dvbbuffer_warm_busy_update(void)
{
  dvbbuffer_warm_t *dw;
  const int64_t now = mclk();
  int64_t next = 0;

  LIST_FOREACH(dw, &dvbbuffer_warm_all, dw_link) {
    if (!dw->dw_busy)
      continue;
    dvbbuffer_warm_busy_entry(dw, now);
    if (dw->dw_busy && (next == 0 || dw->dw_retry < next))
      next = dw->dw_retry;
  }
  return next;
}

/* a network scan is queued or running: tuners freed now go to the scan */
static int
dvbbuffer_warm_scanning(void)
{
  mpegts_network_t *mn;

  LIST_FOREACH(mn, &mpegts_network_all, mn_global_link)
    if (TAILQ_FIRST(&mn->mn_scan_pend) || TAILQ_FIRST(&mn->mn_scan_active))
      return 1;
  return 0;
}

/* checked shortly after a mux stop: the last scan mux is still queued
 * while it stops */
static void
dvbbuffer_warm_freed_cb(void *aux)
{
  dvbbuffer_warm_t *dw;
  int any = 0;

  if (dvbbuffer_warm_scanning())
    return;
  LIST_FOREACH(dw, &dvbbuffer_warm_all, dw_link)
    if (dw->dw_busy && !dw->dw_failed && dw->dw_sub == NULL) {
      dw->dw_retry = mclk();
      any = 1;
    }
  if (any)
    dvbbuffer_warm_reconcile();
}

/*
 * A mux stopped, its tuner is free (global_lock): busy entries try again
 * soon - not during a scan, where the next scan mux takes the tuner
 */
void
dvbbuffer_warm_tuner_freed(void)
{
  dvbbuffer_warm_t *dw;

  lock_assert(&global_lock);
  LIST_FOREACH(dw, &dvbbuffer_warm_all, dw_link)
    if (dw->dw_busy && !dw->dw_failed && dw->dw_sub == NULL) {
      mtimer_arm_rel(&dvbbuffer_warm_freed_timer, dvbbuffer_warm_freed_cb, NULL, sec2mono(2));
      return;
    }
}

static dvbbuffer_warm_t *
dvbbuffer_warm_find(mpegts_mux_t *mm)
{
  dvbbuffer_warm_t *dw;

  LIST_FOREACH(dw, &dvbbuffer_warm_all, dw_link)
    if (dw->dw_mux == mm)
      break;
  return dw;
}

static void
dvbbuffer_warm_set_lru(dvbbuffer_warm_t *dw, int lru)
{
  uint32_t weight = dvbbuffer_warm_weight(lru);

  /* also a changed configuration */
  if (dw->dw_sub && dw->dw_sub->ths_weight != weight)
    subscription_change_weight(dw->dw_sub, weight);
  if (dw->dw_lru == lru)
    return;
  dw->dw_lru = lru;
  if (dw->dw_sub) {
    /* the title follows (status page): the subscription itself stays, so
     * the mux stays tuned; ths_title is read under global_lock only */
    free(dw->dw_sub->ths_title);
    dw->dw_sub->ths_title = strdup(dvbbuffer_warm_title(lru));
  }
  tvhdebug(LS_DVBBUFFER, "%s: now kept warm as %s mux (weight %u)",
           dw->dw_mux->mm_nicename, lru ? "recently used" : "prebuffer",
           dvbbuffer_warm_weight(lru));
  /* a prebuffer mux in use that lost its flag lost its ring with it
   * (dvbbuffer_mux_prebuffer_notify()): kept as a recently used mux, it
   * gets it back while it runs */
  if (lru)
    dvbbuffer_mux_attach(dw->dw_mux);
}

static int
dvbbuffer_used_cmp(const void *a, const void *b)
{
  const dvbbuffer_used_t *x = *(dvbbuffer_used_t * const *)a;
  const dvbbuffer_used_t *y = *(dvbbuffer_used_t * const *)b;

  if ((x->du_users > 0) != (y->du_users > 0))
    return x->du_users > 0 ? -1 : 1;           /* in use first */
  if (x->du_last != y->du_last)
    return x->du_last > y->du_last ? -1 : 1;   /* then the latest */
  return 0;
}

/*
 * Choose the recently used muxes kept warm
 */
static int
dvbbuffer_used_eligible(const dvbbuffer_used_t *du)
{
  /* prebuffer muxes are warm anyway: they do not take LRU places */
  return dvbbuffer_running && dvbbuffer_conf.enabled && !du->du_dead &&
         !dvbbuffer_mux_prebuffer(du->du_mux) && du->du_mux->mm_is_enabled(du->du_mux);
}

static void
dvbbuffer_used_select(void)
{
  dvbbuffer_used_t *du, **v;
  uint32_t n = 0, i, sel;

  LIST_FOREACH(du, &dvbbuffer_used_all, du_link)
    n++;
  if (n == 0)
    return;
  v = malloc(n * sizeof(*v));
  if (v == NULL)
    return;
  n = 0;
  LIST_FOREACH(du, &dvbbuffer_used_all, du_link) {
    du->du_sel = 0;
    v[n++] = du;
  }
  qsort(v, n, sizeof(*v), dvbbuffer_used_cmp);
  for (i = 0, sel = 0; i < n && sel < dvbbuffer_conf.lru_max; i++)
    if (dvbbuffer_used_eligible(v[i])) {
      v[i]->du_sel = 1;
      sel++;
    }
  free(v);
}

/* forget the muxes neither in use nor kept warm */
static void
dvbbuffer_used_forget(void)
{
  dvbbuffer_used_t *du, *du_next;

  for (du = LIST_FIRST(&dvbbuffer_used_all); du; du = du_next) {
    du_next = LIST_NEXT(du, du_link);
    if (!du->du_sel && du->du_users <= 0) {
      LIST_REMOVE(du, du_link);
      free(du);
    }
  }
}

/* the existing entries that stay (up to warm_max prebuffer muxes, then the
 * selected recently used ones) are marked; the number of prebuffer ones */
static uint32_t
dvbbuffer_warm_mark(void)
{
  dvbbuffer_warm_t *dw;
  dvbbuffer_used_t *du;
  uint32_t count = 0;

  LIST_FOREACH(dw, &dvbbuffer_warm_all, dw_link)
    dw->dw_mark = 0;
  LIST_FOREACH(dw, &dvbbuffer_warm_all, dw_link)
    if (dvbbuffer_mux_prebuffer(dw->dw_mux) && count < dvbbuffer_conf.warm_max) {
      dw->dw_mark = 1;
      dvbbuffer_warm_set_lru(dw, 0);
      count++;
    }
  LIST_FOREACH(du, &dvbbuffer_used_all, du_link)
    if (du->du_sel && (dw = dvbbuffer_warm_find(du->du_mux)) != NULL && !dw->dw_mark) {
      dw->dw_mark = 1;
      dvbbuffer_warm_set_lru(dw, 1);
    }
  return count;
}

/* every reconcile passes here: once an hour is enough */
static void
dvbbuffer_warm_over_max(mpegts_mux_t *mm)
{
  static tvhlog_limit_t limit_warm_max;

  if (tvhlog_limit(&limit_warm_max, 3600))
    tvhwarn(LS_DVBBUFFER, "%s: not kept warm, limit of %u warm muxes reached "
            "(more muxes are over it)", mm->mm_nicename, dvbbuffer_conf.warm_max);
}

/* prebuffer muxes without an entry get one, up to warm_max */
static void
dvbbuffer_warm_add_prebuffer(uint32_t count)
{
  mpegts_network_t *mn;
  mpegts_mux_t *mm;

  LIST_FOREACH(mn, &mpegts_network_all, mn_global_link)
    LIST_FOREACH(mm, &mn->mn_muxes, mm_network_link) {
      if (!dvbbuffer_mux_prebuffer(mm) || dvbbuffer_warm_find(mm))
        continue;
      if (count >= dvbbuffer_conf.warm_max) {
        dvbbuffer_warm_over_max(mm);
        continue;
      }
      dvbbuffer_warm_create(mm, 0);
      count++;
    }
}

/*
 * The subscriptions of the entries: running - the next busy period starts
 * with the shortest wait; not running for DVBBUFFER_WARM_STUCK (tuning
 * failed) - failed, released and tried again later. 1 = one is to be
 * checked again
 */
static int
dvbbuffer_warm_check(int64_t now)
{
  dvbbuffer_warm_t *dw;
  int again = 0;

  LIST_FOREACH(dw, &dvbbuffer_warm_all, dw_link) {
    if (dw->dw_sub == NULL || dw->dw_busy)
      continue;
    if (atomic_get(&dw->dw_sub->ths_state) == SUBSCRIPTION_GOT_SERVICE) {
      dw->dw_running = now;
      dw->dw_backoff = 0;
      dw->dw_failed = 0;
    } else if (now - dw->dw_running >= sec2mono(DVBBUFFER_WARM_STUCK)) {
      tvhdebug(LS_DVBBUFFER, "%s: warm subscription not running for %d s",
               dw->dw_mux->mm_nicename, DVBBUFFER_WARM_STUCK);
      dw->dw_busy = 1;
      dw->dw_failed = 1;
      continue;
    }
    again = 1;
  }
  return again;
}

/*
 * Bring the warm subscriptions in line with the mux flags and the
 * configuration (global_lock)
 */
static void
dvbbuffer_warm_reconcile_cb(void *aux)
{
  dvbbuffer_warm_t *dw, *dw_next;
  dvbbuffer_used_t *du;
  uint32_t count;
  int64_t next;
  int check;

  lock_assert(&global_lock);

  dvbbuffer_used_select();
  count = dvbbuffer_warm_mark();
  for (dw = LIST_FIRST(&dvbbuffer_warm_all); dw; dw = dw_next) {
    dw_next = LIST_NEXT(dw, dw_link);
    if (!dw->dw_mark)
      dvbbuffer_warm_destroy(dw);
  }
  dvbbuffer_warm_add_prebuffer(count);
  LIST_FOREACH(du, &dvbbuffer_used_all, du_link)
    if (du->du_sel && dvbbuffer_warm_find(du->du_mux) == NULL)
      dvbbuffer_warm_create(du->du_mux, 1);
  dvbbuffer_used_forget();

  /* busy entries keep their place; next try when the wait is over, the
   * others are checked now and then */
  check = dvbbuffer_warm_check(mclk());
  next = dvbbuffer_warm_busy_update();
  if (check && (next == 0 || next > mclk() + sec2mono(DVBBUFFER_WARM_CHECK)))
    next = mclk() + sec2mono(DVBBUFFER_WARM_CHECK);
  if (next)
    mtimer_arm_abs(&dvbbuffer_warm_timer, dvbbuffer_warm_reconcile_cb, NULL, next);
}

void
dvbbuffer_warm_reconcile(void)
{
  mtimer_arm_rel(&dvbbuffer_warm_timer, dvbbuffer_warm_reconcile_cb, NULL, 0);
}

/*
 * A viewer starts (+1) or stops (-1) using a mux (global_lock)
 */
void
dvbbuffer_warm_used(mpegts_mux_t *mm, int delta)
{
  dvbbuffer_used_t *du;
  dvbbuffer_warm_t *dw;

  lock_assert(&global_lock);
  /* only DVB muxes: an IPTV mux has no tuner to keep, a warm subscription
   * would only hold the stream open (or retry a dead one forever); not a
   * sub-mux (no transponder found for it): it has no ring of its own */
  if (!dvbbuffer_mux_is_dvb(mm) || mpegts_mux_is_submux(mm))
    return;
  LIST_FOREACH(du, &dvbbuffer_used_all, du_link)
    if (du->du_mux == mm)
      break;
  if (du == NULL) {
    if (delta < 0)
      return;
    if ((du = calloc(1, sizeof(*du))) == NULL)
      return;
    du->du_mux = mm;
    LIST_INSERT_HEAD(&dvbbuffer_used_all, du, du_link);
  }
  du->du_users += delta;
  if (du->du_users < 0)
    du->du_users = 0;
  du->du_last = mclk();
  if (delta > 0)
    du->du_dead = 0;    /* used again: another try */
  /* ... and one that waits (no tuner, dish elsewhere) looks again now: the
   * mux is being tuned for the viewer */
  dw = delta > 0 ? dvbbuffer_warm_find(mm) : NULL;
  if (dw && dw->dw_busy && dw->dw_sub == NULL) {
    dw->dw_retry = mclk();
    dw->dw_backoff = 0;
  }
  if (dvbbuffer_conf.lru_max || dw)
    dvbbuffer_warm_reconcile();
}

/*
 * A service starts (+1) or stops (-1) - a viewer or recording, not the
 * service mapper and other internal subscriptions below the user weights
 * (global_lock)
 */
void
dvbbuffer_service_used(mpegts_service_t *s, int delta)
{
  mpegts_mux_t *mm;
  dvb_mux_t *p;

  lock_assert(&global_lock);
  if (!dvbbuffer_running || s->s_dvb_mux == NULL)
    return;
  if (delta > 0) {
    if (s->s_type != STYPE_STD || s->s_dvbbuffer_used ||
        s->s_dvb_subscription_weight < SUBSCRIPTION_PRIO_MIN)
      return;
    s->s_dvbbuffer_used = 1;
  } else {
    if (!s->s_dvbbuffer_used)
      return;
    s->s_dvbbuffer_used = 0;
    /* the mux counted at the start: the link of a sub-mux to its
     * transponder may have changed since (deleted: forgotten with it) */
    if ((mm = mpegts_mux_find0(&s->s_dvbbuffer_used_mux)) != NULL)
      dvbbuffer_warm_used(mm, delta);
    return;
  }
  /* a sub-mux: its transponder is kept warm (its ring has the sub-mux; a
   * sub-mux in a sub-mux: the transponder at the root) */
  mm = s->s_dvb_mux;
  while (mpegts_mux_is_submux(mm) && (p = dvb_mux_submux_parent((dvb_mux_t *)mm)) != NULL)
    mm = (mpegts_mux_t *)p;
  uuid_duplicate(&s->s_dvbbuffer_used_mux, &mm->mm_id.in_uuid);
  dvbbuffer_warm_used(mm, delta);
}

/*
 * The warm subscription of a recently used mux did not start: no input
 * (called from its subscription output, global_lock) - the reconciler
 * releases it, not this callback
 */
static void
dvbbuffer_warm_lru_dead(mpegts_mux_t *mm)
{
  dvbbuffer_used_t *du;

  LIST_FOREACH(du, &dvbbuffer_used_all, du_link)
    if (du->du_mux == mm)
      break;
  if (du == NULL || du->du_dead)
    return;
  du->du_dead = 1;
  tvhinfo(LS_DVBBUFFER, "%s: no input, no longer kept warm as recently used mux",
          mm->mm_nicename);
  dvbbuffer_warm_reconcile();
}

/* kept warm as a recently used mux */
int
dvbbuffer_warm_lru(mpegts_mux_t *mm)
{
  dvbbuffer_warm_t *dw = dvbbuffer_warm_find(mm);
  return dw != NULL && dw->dw_lru;
}

void
dvbbuffer_warm_mux_delete(mpegts_mux_t *mm)
{
  dvbbuffer_warm_t *dw;
  dvbbuffer_used_t *du;

  LIST_FOREACH(du, &dvbbuffer_used_all, du_link)
    if (du->du_mux == mm) {
      LIST_REMOVE(du, du_link);
      free(du);
      break;
    }

  LIST_FOREACH(dw, &dvbbuffer_warm_all, dw_link)
    if (dw->dw_mux == mm) {
      dvbbuffer_warm_destroy(dw);
      break;
    }
}

void
dvbbuffer_warm_init(void)
{
  /* give the inputs some time to settle after startup */
  mtimer_arm_rel(&dvbbuffer_warm_timer, dvbbuffer_warm_reconcile_cb, NULL,
                 sec2mono(5));
}

void
dvbbuffer_warm_done(void)
{
  dvbbuffer_warm_t *dw;
  dvbbuffer_used_t *du;

  mtimer_disarm(&dvbbuffer_warm_timer);
  mtimer_disarm(&dvbbuffer_warm_freed_timer);
  while ((dw = LIST_FIRST(&dvbbuffer_warm_all)) != NULL)
    dvbbuffer_warm_destroy(dw);
  while ((du = LIST_FIRST(&dvbbuffer_used_all)) != NULL) {
    LIST_REMOVE(du, du_link);
    free(du);
  }
}
