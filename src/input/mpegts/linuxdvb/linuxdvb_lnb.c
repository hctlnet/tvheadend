/*
 *  Tvheadend - Linux DVB LNB config
 *
 *  Copyright (C) 2013 Adam Sutton
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

#include "tvheadend.h"
#include "linuxdvb_private.h"
#include "settings.h"
#include "spawn.h"

#include <poll.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <linux/dvb/dmx.h>

/* **************************************************************************
 * Class definition
 * *************************************************************************/

typedef struct linuxdvb_lnb_conf
{
  linuxdvb_lnb_t;

  /* Freq control */
  int lnb_low;
  int lnb_high;
  int lnb_switch;

  /* Deviation of the local oscillators from lnb_low/lnb_high (kHz) */
  int lnb_low_corr;
  int lnb_high_corr;

  /* External polarizer control */
  int   lnb_polarizer_enabled;  /* a command sets the polarisation */
  char *lnb_polarizer_cmd;      /* that command */
  int   lnb_static_voltage;     /* 0=13V, 1=18V when polarizer enabled */
} linuxdvb_lnb_conf_t;

static void
linuxdvb_lnb_class_get_title
  ( idnode_t *o, const char *lang, char *dst, size_t dstsize )
{
  linuxdvb_diseqc_t *ld = (linuxdvb_diseqc_t*)o;
  snprintf(dst, dstsize, tvh_gettext_lang(lang, N_("LNB: %s")), ld->ld_type);
}

static htsmsg_t *
linuxdvb_lnb_voltage_list ( void *o, const char *lang )
{
  static const struct strtab tab[] = {
    { N_("13V (Vertical/Right)"),   0 },
    { N_("18V (Horizontal/Left)"),  1 },
  };
  return strtab2htsmsg(tab, 1, lang);
}

extern const idclass_t linuxdvb_diseqc_class;

const idclass_t linuxdvb_lnb_class =
{
  .ic_super       = &linuxdvb_diseqc_class,
  .ic_class       = "linuxdvb_lnb_basic",
  .ic_caption     = N_("LNB"),
  .ic_get_title   = linuxdvb_lnb_class_get_title,
  .ic_properties = (const property_t[]) {
    {
      .type     = PT_INT,
      .id       = "lfo",
      .name     = N_("Low frequency offset"),
      .opts     = PO_RDONLY | PO_NOSAVE,
      .off      = offsetof(linuxdvb_lnb_conf_t, lnb_low),
    },
    {
      .type     = PT_INT,
      .id       = "hfo",
      .name     = N_("High frequency offset"),
      .opts     = PO_RDONLY | PO_NOSAVE,
      .off      = offsetof(linuxdvb_lnb_conf_t, lnb_high),
    },
    {
      .type     = PT_INT,
      .id       = "sfo",
      .name     = N_("Switch frequency offset"),
      .opts     = PO_RDONLY | PO_NOSAVE,
      .off      = offsetof(linuxdvb_lnb_conf_t, lnb_switch),
    },
    {
      .type     = PT_INT,
      .id       = "lfo_corr",
      .name     = N_("Low frequency correction (kHz)"),
      .desc     = N_("Deviation of the low band local oscillator of this "
                     "LNB from the low frequency offset, in kHz. An LNB "
                     "whose oscillator runs 2 MHz low puts every transponder "
                     "2 MHz above its published frequency: enter -2000."),
      .opts     = PO_ADVANCED,
      .off      = offsetof(linuxdvb_lnb_conf_t, lnb_low_corr),
    },
    {
      .type     = PT_INT,
      .id       = "hfo_corr",
      .name     = N_("High frequency correction (kHz)"),
      .desc     = N_("Deviation of the high band local oscillator of this "
                     "LNB from the high frequency offset, in kHz (see the "
                     "low frequency correction)."),
      .opts     = PO_ADVANCED,
      .off      = offsetof(linuxdvb_lnb_conf_t, lnb_high_corr),
    },
    {
      .type     = PT_BOOL,
      .id       = "polarizer_enabled",
      .name     = N_("Use external polarizer"),
      .desc     = N_("The polarisation is set by an external command (a "
                     "mechanical or magnetic polarizer, a positioner with "
                     "one) instead of the LNB voltage, which stays at the "
                     "static LNB voltage."),
      .off      = offsetof(linuxdvb_lnb_conf_t, lnb_polarizer_enabled),
    },
    {
      .type     = PT_STR,
      .id       = "polarizer_cmd",
      .name     = N_("External polarizer command"),
      .desc     = N_("Command that sets the polarisation. It is run before "
                     "a tune with two arguments: the polarisation (H, V, L "
                     "or R) and the frequency of the mux in kHz. A number "
                     "it prints is the time in seconds to wait before the "
                     "tune."),
      .off      = offsetof(linuxdvb_lnb_conf_t, lnb_polarizer_cmd),
    },
    {
      .type     = PT_INT,
      .id       = "static_voltage",
      .name     = N_("Static LNB voltage"),
      .desc     = N_("Voltage to apply to LNB when external polarizer is enabled. "
                     "Some LNBs need 18V for proper signal, others work fine with 13V."),
      .opts     = PO_DOC_NLIST,
      .list     = linuxdvb_lnb_voltage_list,
      .off      = offsetof(linuxdvb_lnb_conf_t, lnb_static_voltage),
    },
    {}
  }
};

/* **************************************************************************
 * Control functions
 * *************************************************************************/

/*
 * Standard freq switched LNB
 */

static uint32_t
linuxdvb_lnb_standard_freq 
  ( linuxdvb_lnb_t *l, dvb_mux_t *lm )
{
  linuxdvb_lnb_conf_t *lnb = (linuxdvb_lnb_conf_t*)l;
  int32_t                f = (int32_t)lm->lm_tuning.dmc_fe_freq;
  if (lnb->lnb_switch && f > lnb->lnb_switch)
    f -= lnb->lnb_high + lnb->lnb_high_corr;
  else
    f -= lnb->lnb_low + lnb->lnb_low_corr;
  return (uint32_t)abs(f);
}

static int
linuxdvb_lnb_bandstack_match
  ( linuxdvb_lnb_t *l, dvb_mux_t *lm1, dvb_mux_t *lm2 )
{
  /* everything is in one cable */
  return 1;
}

static int
linuxdvb_lnb_standard_match
  ( linuxdvb_lnb_t *l, dvb_mux_t *lm1, dvb_mux_t *lm2 )
{
  linuxdvb_lnb_conf_t *lnb = (linuxdvb_lnb_conf_t*)l;
  dvb_mux_conf_t      *dmc1 = &lm1->lm_tuning;
  dvb_mux_conf_t      *dmc2 = &lm2->lm_tuning;

  if (lnb->lnb_switch) {
    uint32_t hi1 = dmc1->dmc_fe_freq > lnb->lnb_switch;
    uint32_t hi2 = dmc2->dmc_fe_freq > lnb->lnb_switch;
    if (hi1 != hi2)
      return 0;
  }
  if (dmc1->u.dmc_fe_qpsk.polarisation != dmc2->u.dmc_fe_qpsk.polarisation)
    return 0;
  return 1;
}

static int
linuxdvb_lnb_standard_band
  ( linuxdvb_lnb_t *l, dvb_mux_t *lm )
{
  linuxdvb_lnb_conf_t *lnb = (linuxdvb_lnb_conf_t*)l;
  uint32_t               f = lm->lm_tuning.dmc_fe_freq;
  return (lnb->lnb_switch && f > lnb->lnb_switch);
}

static int
linuxdvb_lnb_standard_pol
  ( linuxdvb_lnb_t *l, dvb_mux_t *lm )
{
  dvb_mux_conf_t      *dmc = &lm->lm_tuning;
  return dmc->u.dmc_fe_qpsk.polarisation == DVB_POLARISATION_HORIZONTAL ||
         dmc->u.dmc_fe_qpsk.polarisation == DVB_POLARISATION_CIRCULAR_LEFT;
}

static int
linuxdvb_lnb_inverted_pol
  ( linuxdvb_lnb_t *l, dvb_mux_t *lm )
{
  return !linuxdvb_lnb_standard_pol(l, lm);
}

/*
 * Bandstacked polarity switch LNB
 */

static uint32_t
linuxdvb_lnb_bandstack_freq
  ( linuxdvb_lnb_t *l, dvb_mux_t *lm )
{
  linuxdvb_lnb_conf_t *lnb = (linuxdvb_lnb_conf_t*)l;
  int32_t                f = (int32_t)lm->lm_tuning.dmc_fe_freq;
  dvb_mux_conf_t      *dmc = &lm->lm_tuning;
  int pol = dmc->u.dmc_fe_qpsk.polarisation == DVB_POLARISATION_HORIZONTAL ||
            dmc->u.dmc_fe_qpsk.polarisation == DVB_POLARISATION_CIRCULAR_LEFT;
  if (pol)
    f -= lnb->lnb_high + lnb->lnb_high_corr;
  else
    f -= lnb->lnb_low + lnb->lnb_low_corr;
  return (uint32_t)abs(f);
}

static int
linuxdvb_lnb_bandstack_band
  ( linuxdvb_lnb_t *l, dvb_mux_t *lm )
{
  dvb_mux_conf_t      *dmc = &lm->lm_tuning;
  int pol = dmc->u.dmc_fe_qpsk.polarisation == DVB_POLARISATION_HORIZONTAL ||
            dmc->u.dmc_fe_qpsk.polarisation == DVB_POLARISATION_CIRCULAR_LEFT;
  return pol;
}

static int
linuxdvb_lnb_bandstack_pol
  ( linuxdvb_lnb_t *l, dvb_mux_t *lm )
{
  return 0;
}

/*
 * External polarizer: a command sets the polarisation, as the external
 * rotor command moves the dish. Its arguments are the polarisation (H, V,
 * L, R) and the frequency of the mux in kHz. A number it prints is the
 * time in seconds to wait before the tune (the polarizer moves); nothing,
 * or no answer in time: none. The tune goes on when the command fails.
 */
#define LNB_POLARIZER_ANSWER_MS 5000
#define LNB_POLARIZER_WAIT_MAX  60

static int
linuxdvb_lnb_polarizer_cmd
  ( linuxdvb_lnb_conf_t *lnb, dvb_mux_t *lm )
{
  char pol[2] = "", freq[16], out[32];
  char *argv[] = { NULL, pol, freq, NULL };
  struct pollfd pfd;
  int rd = -1, n, wait = 0;

  if (lnb->lnb_polarizer_cmd == NULL || *lnb->lnb_polarizer_cmd == '\0')
    return 0;

  switch (lm->lm_tuning.u.dmc_fe_qpsk.polarisation) {
  case DVB_POLARISATION_HORIZONTAL:     pol[0] = 'H'; break;
  case DVB_POLARISATION_VERTICAL:       pol[0] = 'V'; break;
  case DVB_POLARISATION_CIRCULAR_LEFT:  pol[0] = 'L'; break;
  case DVB_POLARISATION_CIRCULAR_RIGHT: pol[0] = 'R'; break;
  default:
    tvhwarn(LS_DISEQC, "polarizer: unknown polarisation %d",
            lm->lm_tuning.u.dmc_fe_qpsk.polarisation);
    return 0;
  }
  snprintf(freq, sizeof(freq), "%u", lm->lm_tuning.dmc_fe_freq);

  if (spawn_and_give_stdout(lnb->lnb_polarizer_cmd, argv, NULL, &rd, NULL, 1) < 0) {
    tvherror(LS_DISEQC, "polarizer: cannot spawn external command");
    return 0;
  }
  /* its answer, for a while at most: the tune waits here */
  pfd.fd = rd;
  pfd.events = POLLIN;
  if (poll(&pfd, 1, LNB_POLARIZER_ANSWER_MS) <= 0) {
    tvhwarn(LS_DISEQC, "polarizer: no answer from external command");
  } else if ((n = read(rd, out, sizeof(out) - 1)) > 0) {
    out[n] = '\0';
    if (out[0] >= '0' && out[0] <= '9')
      wait = MIN(atoi(out), LNB_POLARIZER_WAIT_MAX);
  }
  close(rd);
  tvhdebug(LS_DISEQC, "polarizer: external command for %s %s, waiting %d s",
           pol, freq, wait);
  return wait;
}

/* with the external polarizer the LNB gets a fixed voltage: the satconf asks
 * for it (lnb_vol) before it sets any voltage, so vol below is already it */
static int
linuxdvb_lnb_static_vol ( linuxdvb_lnb_t *l, dvb_mux_t *lm )
{
  linuxdvb_lnb_conf_t *lnb = (linuxdvb_lnb_conf_t*)l;
  return lnb->lnb_polarizer_enabled ? (lnb->lnb_static_voltage ? 1 : 0) : -1;
}

static int
linuxdvb_lnb_polarizer_tune
  ( linuxdvb_diseqc_t *ld, dvb_mux_t *lm,
    linuxdvb_satconf_t *lsp, linuxdvb_satconf_ele_t *ls,
    int vol, int pol, int band, int freq )
{
  linuxdvb_lnb_conf_t *lnb = (linuxdvb_lnb_conf_t*)ld;
  int wait = 0, r;

  /* the polarizer is set by its command (vol is the static voltage); the
   * time it asks for is waited before the tune goes on */
  if (lnb->lnb_polarizer_enabled)
    wait = linuxdvb_lnb_polarizer_cmd(lnb, lm);
  r = linuxdvb_diseqc_set_volt(ls->lse_parent, vol);
  return r ? r : wait;
}

/* **************************************************************************
 * Static list
 * *************************************************************************/

linuxdvb_lnb_conf_t linuxdvb_lnb_all[] = {
  {
    { {
      .ld_type    = "Universal",
      .ld_tune    = linuxdvb_lnb_polarizer_tune,
      },
      .lnb_freq   = linuxdvb_lnb_standard_freq,
      .lnb_match  = linuxdvb_lnb_standard_match,
      .lnb_band   = linuxdvb_lnb_standard_band,
      .lnb_pol    = linuxdvb_lnb_standard_pol,
    },
    .lnb_low    =  9750000,
    .lnb_high   = 10600000,
    .lnb_switch = 11700000,
  },
  {
    { {
      .ld_type    = "Standard",
      .ld_tune    = linuxdvb_lnb_polarizer_tune,
      },
      .lnb_freq   = linuxdvb_lnb_standard_freq,
      .lnb_match  = linuxdvb_lnb_standard_match,
      .lnb_band   = linuxdvb_lnb_standard_band,
      .lnb_pol    = linuxdvb_lnb_standard_pol,
    },
    .lnb_low    = 10000000,
    .lnb_high   = 0,
    .lnb_switch = 0,
  },
  {
    { {
      .ld_type    = "Enhanced",
      .ld_tune    = linuxdvb_lnb_polarizer_tune,
      },
      .lnb_freq   = linuxdvb_lnb_standard_freq,
      .lnb_match  = linuxdvb_lnb_standard_match,
      .lnb_band   = linuxdvb_lnb_standard_band,
      .lnb_pol    = linuxdvb_lnb_standard_pol,
    },
    .lnb_low    =  9750000,
    .lnb_high   = 0,
    .lnb_switch = 0,
  },
  {
    { {
      .ld_type    = "C-Band",
      .ld_tune    = linuxdvb_lnb_polarizer_tune,
      },
      .lnb_freq   = linuxdvb_lnb_standard_freq,
      .lnb_match  = linuxdvb_lnb_standard_match,
      .lnb_band   = linuxdvb_lnb_standard_band,
      .lnb_pol    = linuxdvb_lnb_standard_pol,
    },
    .lnb_low    =  5150000,
    .lnb_high   = 0,
    .lnb_switch = 0,
  },
  {
    { {
      .ld_type    = "C-Band (bandstack)",
      .ld_tune    = linuxdvb_lnb_polarizer_tune,
      },
      .lnb_freq   = linuxdvb_lnb_bandstack_freq,
      .lnb_match  = linuxdvb_lnb_bandstack_match,
      .lnb_band   = linuxdvb_lnb_bandstack_band,
      .lnb_pol    = linuxdvb_lnb_bandstack_pol,
    },
    .lnb_low    =  5150000,
    .lnb_high   =  5750000,
    .lnb_switch =  0,
  },
  {
    { {
      .ld_type    = "Ku 10750",
      .ld_tune    = linuxdvb_lnb_polarizer_tune,
      },
      .lnb_freq   = linuxdvb_lnb_standard_freq,
      .lnb_match  = linuxdvb_lnb_standard_match,
      .lnb_band   = linuxdvb_lnb_standard_band,
      .lnb_pol    = linuxdvb_lnb_standard_pol,
    },
    .lnb_low    = 10750000,
    .lnb_high   = 0,
    .lnb_switch = 0,
  },
  {
    { {
      .ld_type    = "Ku 10750 (Hi-Band)",
      .ld_tune    = linuxdvb_lnb_polarizer_tune,
      },
      .lnb_freq   = linuxdvb_lnb_standard_freq,
      .lnb_match  = linuxdvb_lnb_standard_match,
      .lnb_band   = linuxdvb_lnb_standard_band,
      .lnb_pol    = linuxdvb_lnb_standard_pol,
    },
    .lnb_low    = 10750000,
    .lnb_high   = 10750000,
    .lnb_switch = 10750000,
  },
  {
    { {
      .ld_type    = "Ku 10750 (Hi-Band, Inverted-Polar.)",
      .ld_tune    = linuxdvb_lnb_polarizer_tune,
      },
      .lnb_freq   = linuxdvb_lnb_standard_freq,
      .lnb_match  = linuxdvb_lnb_standard_match,
      .lnb_band   = linuxdvb_lnb_standard_band,
      .lnb_pol    = linuxdvb_lnb_inverted_pol,
    },
    .lnb_low    = 10750000,
    .lnb_high   = 10750000,
    .lnb_switch = 10750000,
  },
  {
    { {
      .ld_type    = "Ku 11300",
      .ld_tune    = linuxdvb_lnb_polarizer_tune,
      },
      .lnb_freq   = linuxdvb_lnb_standard_freq,
      .lnb_match  = linuxdvb_lnb_standard_match,
      .lnb_band   = linuxdvb_lnb_standard_band,
      .lnb_pol    = linuxdvb_lnb_standard_pol,
    },
    .lnb_low    = 11300000,
    .lnb_high   = 0,
    .lnb_switch = 0,
  },
  {
    { {
      .ld_type    = "Ku 11300 (Hi-Band)",
      .ld_tune    = linuxdvb_lnb_polarizer_tune,
      },
      .lnb_freq   = linuxdvb_lnb_standard_freq,
      .lnb_match  = linuxdvb_lnb_standard_match,
      .lnb_band   = linuxdvb_lnb_standard_band,
      .lnb_pol    = linuxdvb_lnb_standard_pol,
    },
    .lnb_low    = 11300000,
    .lnb_high   = 11300000,
    .lnb_switch = 11300000,
  },
  {
     { {
       .ld_type    = "C 5150/Ku 10750 (22kHz switch)",
       .ld_tune    = linuxdvb_lnb_polarizer_tune,
       },
       .lnb_freq   = linuxdvb_lnb_standard_freq,
       .lnb_band   = linuxdvb_lnb_standard_band,
       .lnb_pol    = linuxdvb_lnb_standard_pol,
     },
     .lnb_low    =  5150000,
     .lnb_high   = 10750000,
     .lnb_switch = 11700000, /* 10750 + 950 (bottom of the receiver range 950Mhz-2150Mhz) */
  },
  {
    { {
      .ld_type    = "DBS",
      .ld_tune    = linuxdvb_lnb_polarizer_tune,
      },
      .lnb_freq   = linuxdvb_lnb_standard_freq,
      .lnb_match  = linuxdvb_lnb_standard_match,
      .lnb_band   = linuxdvb_lnb_standard_band,
      .lnb_pol    = linuxdvb_lnb_standard_pol,
    },
    .lnb_low    = 11250000,
    .lnb_high   = 0,
    .lnb_switch = 0,
  },
  {
    { {
      .ld_type    = "DBS Bandstack",
      .ld_tune    = linuxdvb_lnb_polarizer_tune,
      },
      .lnb_freq   = linuxdvb_lnb_bandstack_freq,
      .lnb_match  = linuxdvb_lnb_bandstack_match,
      .lnb_band   = linuxdvb_lnb_bandstack_band,
      .lnb_pol    = linuxdvb_lnb_bandstack_pol,
    },
    .lnb_low    = 11250000,
    .lnb_high   = 14350000,
    .lnb_switch = 0,
  },
  {
    { {
      .ld_type    = "Ku 10700 (Australia)",
      .ld_tune    = linuxdvb_lnb_polarizer_tune,
      },
      .lnb_freq   = linuxdvb_lnb_standard_freq,
      .lnb_match  = linuxdvb_lnb_standard_match,
      .lnb_band   = linuxdvb_lnb_standard_band,
      .lnb_pol    = linuxdvb_lnb_standard_pol,
    },
    .lnb_low    = 10700000,
    .lnb_high   = 10700000,
    .lnb_switch = 11800000,
  },
};

/* **************************************************************************
 * Create / Config
 * *************************************************************************/

htsmsg_t *
linuxdvb_lnb_list ( void *o, const char *lang )
{
  int i;
  htsmsg_t *m = htsmsg_create_list();
  for (i = 0; i < ARRAY_SIZE(linuxdvb_lnb_all); i++)
    htsmsg_add_str(m, NULL, linuxdvb_lnb_all[i].ld_type);
  return m;
}

linuxdvb_lnb_t *
linuxdvb_lnb_create0
  ( const char *name, htsmsg_t *conf, linuxdvb_satconf_ele_t *ls )
{
  linuxdvb_lnb_conf_t *lsc = &linuxdvb_lnb_all[0], *lsc2;
  int i;

  /* Find */
  if (name) {
    for (i = 0; i < ARRAY_SIZE(linuxdvb_lnb_all); i++)
      if (!strcmp(linuxdvb_lnb_all[i].ld_type, name)) {
        lsc = &linuxdvb_lnb_all[i];
        break;
      }
  }

  lsc2 = malloc(sizeof(linuxdvb_lnb_conf_t));
  *lsc2 = *lsc;
  lsc2->lnb_vol = linuxdvb_lnb_static_vol;
  return (linuxdvb_lnb_t *)
            linuxdvb_diseqc_create0((linuxdvb_diseqc_t *)lsc2,
                                    NULL, &linuxdvb_lnb_class,
                                    conf, lsc->ld_type, ls);
}

void
linuxdvb_lnb_destroy ( linuxdvb_lnb_t *lnb )
{
  linuxdvb_diseqc_destroy((linuxdvb_diseqc_t *)lnb);
  free(((linuxdvb_lnb_conf_t *)lnb)->lnb_polarizer_cmd);
  free(lnb);
}

/******************************************************************************
 * Editor Configuration
 *
 * vim:sts=2:ts=2:sw=2:et
 *****************************************************************************/
