/*
 *  Tvheadend - Linux DVB DiseqC Rotor
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

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <math.h>
#include <linux/dvb/frontend.h>

/* **************************************************************************
 * Class definition
 * *************************************************************************/

typedef struct linuxdvb_rotor
{
  linuxdvb_diseqc_t;

  /* USALS */
  double    lr_site_lat;
  double    lr_site_lon;
  double    lr_sat_lon;
  double    lr_zero_lon;
  
  /* GOTOX */
  uint32_t  lr_position;
  uint32_t  lr_rate;

} linuxdvb_rotor_t;

static const char *
linuxdvb_rotor_class_get_title ( idnode_t *o )
{
  static char buf[256];
  linuxdvb_diseqc_t *ld = (linuxdvb_diseqc_t*)o;
  snprintf(buf, sizeof(buf), "Rotor: %s", ld->ld_type);
  return buf;
}

extern const idclass_t linuxdvb_diseqc_class;

const idclass_t linuxdvb_rotor_class = {
  .ic_super       = &linuxdvb_diseqc_class,
  .ic_class       = "linuxdvb_rotor",
  .ic_caption     = "DiseqC Rotor",
  .ic_get_title   = linuxdvb_rotor_class_get_title,
};

const idclass_t linuxdvb_rotor_gotox_class =
{
  .ic_super       = &linuxdvb_rotor_class,
  .ic_class       = "linuxdvb_rotor_gotox",
  .ic_caption     = "GOTOX Rotor",
  .ic_properties  = (const property_t[]) {
    {
      .type   = PT_U16,
      .id     = "position",
      .name   = "GOTOX Position",
      .off    = offsetof(linuxdvb_rotor_t, lr_position),
    },
    {
      .type   = PT_DBL,
      .id     = "sat_lon",
      .name   = "Satellite Longitude",
      .off    = offsetof(linuxdvb_rotor_t, lr_sat_lon),
    },
    {
      .type   = PT_U16,
      .id     = "rate",
      .name   = "Rate (millis/click)",
      .off    = offsetof(linuxdvb_rotor_t, lr_rate),
    },
    {}
  }
};

const idclass_t linuxdvb_rotor_usals_class =
{
  .ic_super       = &linuxdvb_rotor_class,
  .ic_class       = "linuxdvb_rotor_usals",
  .ic_caption     = "USALS Rotor",
  .ic_properties  = (const property_t[]) {
    {
      .type   = PT_DBL,
      .id     = "site_lat",
      .name   = "Site Latitude",
      .off    = offsetof(linuxdvb_rotor_t, lr_site_lat),
    },
    {
      .type   = PT_DBL,
      .id     = "site_lon",
      .name   = "Site Longitude",
      .off    = offsetof(linuxdvb_rotor_t, lr_site_lon),
    },
    {
      .type   = PT_DBL,
      .id     = "sat_lon",
      .name   = "Satellite Longitude",
      .off    = offsetof(linuxdvb_rotor_t, lr_sat_lon),
    },
    {
      .type   = PT_U16,
      .id     = "rate",
      .name   = "Rate (millis/deg)",
      .off    = offsetof(linuxdvb_rotor_t, lr_rate),
    },
 
    {}
  }
};

/* **************************************************************************
 * Class methods
 * *************************************************************************/

static int
linuxdvb_rotor_grace
  ( linuxdvb_diseqc_t *ld, dvb_mux_t *lm )
{
  linuxdvb_rotor_t *lr = (linuxdvb_rotor_t*)ld;
  linuxdvb_satconf_t *ls = ld->ld_satconf->lse_parent;
  int newpos, curpos, delta, tunit;

  if (!ls->ls_orbital_dir || lr->lr_rate == 0)
    return ls->ls_max_rotor_move;

  newpos = (lr->lr_sat_lon + 0.05) * 10;
  if (idnode_is_instance(&lr->ld_id, &linuxdvb_rotor_gotox_class)) {
    tunit  = 1000;  /* GOTOX */
  } else {
    tunit  = 10000; /* USALS */
  }

  curpos = ls->ls_orbital_pos;
  if (ls->ls_orbital_dir == 'W')
    curpos = -(curpos);
  delta = abs(deltaI32(curpos, newpos));

  /* ignore very small movements like 0.8W and 1W */
  if (delta <= 2)
    return 0;

  /* add one extra second, because of the rounding issue */
  return ((lr->lr_rate*delta+(tunit-1))/tunit) + 1;
}

static int
linuxdvb_rotor_check_orbital_pos
  ( dvb_mux_t *lm, linuxdvb_satconf_ele_t *ls )
{
  linuxdvb_satconf_t *lsp;
  int  pos;
  char dir;

  if (dvb_network_get_orbital_pos(lm->mm_network, &pos, &dir) < 0)
    return 0;

  lsp = ls->lse_parent;

  if (dir != lsp->ls_orbital_dir)
    return 0;

  if (abs(pos - lsp->ls_orbital_pos) > 2)
    return 0;

  tvhdebug("diseqc", "rotor already positioned to %i.%i%c",
                     pos / 10, pos % 10, dir);
  return 1;
}

/* GotoX */
static int
linuxdvb_rotor_gotox_tune
  ( linuxdvb_rotor_t *lr, dvb_mux_t *lm, linuxdvb_satconf_ele_t *ls, int fd )
{
  int i;

  for (i = 0; i <= ls->lse_parent->ls_diseqc_repeats; i++) {
    if (linuxdvb_diseqc_send(fd, 0xE0, 0x31, 0x6B, 1, (int)lr->lr_position)) {
      tvherror("diseqc", "failed to set GOTOX pos %d", lr->lr_position);
      return -1;
    }
    usleep(25000);
  }

  tvhdebug("diseqc", "rotor GOTOX pos %d sent", lr->lr_position);

  return linuxdvb_rotor_grace((linuxdvb_diseqc_t*)lr,lm);
}

static inline
double to_radians( double val )
{
  return ((val * M_PI) / 180.0);
}

static inline
double to_degrees( double val )
{
  return ((val * 180.0) / M_PI);
}

static inline
double to_rev( double val )
{
  return val - floor(val / 360.0) * 360;
}

static
void usals_sat_azimuth_and_elevation
  ( double site_lat, double site_lon, double site_alt, double sat_lon,
    double *azimuth, double *elevation )
{
  const double f     =  1.00 / 298.257;  // Earth flattening factor
  const double r_sat =  42164.57;        // Distance from earth centre to satellite
  const double r_eq  =   6378.14;        // Earth radius

  const double a0    =  0.58804392;
  const double a1    = -0.17941557;
  const double a2    =  0.29906946E-1;
  const double a3    = -0.25187400E-2;
  const double a4    =  0.82622101E-4;

  double sin_site_lat  = sin(to_radians(site_lat));
  double cos_site_lat  = cos(to_radians(site_lat));
  double Rstation      = r_eq / sqrt(1.00 - f*(2.00-f)*sin_site_lat*sin_site_lat);
  double Ra            = (Rstation+site_alt)*cos_site_lat;
  double Rz            = Rstation*(1.00-f)*(1.00-f)*sin_site_lat;
  double alfa_rx       = r_sat*cos(to_radians(sat_lon-site_lon)) - Ra;
  double alfa_ry       = r_sat*sin(to_radians(sat_lon-site_lon));
  double alfa_rz       = -Rz;
  double alfa_r_north  = -alfa_rx*sin_site_lat + alfa_rz*cos_site_lat;

  double alfa_r_zenith = alfa_rx*cos_site_lat + alfa_rz*sin_site_lat;
  double El_geometric  = to_degrees(atan(alfa_r_zenith/sqrt(alfa_r_north*alfa_r_north+alfa_ry*alfa_ry)));
  double x             = fabs(El_geometric+0.589);
  double refraction    = fabs(a0+a1*x+a2*x*x+a3*x*x*x+a4*x*x*x*x);

  *azimuth = 0.00;
  if (alfa_r_north < 0)
    *azimuth = 180+to_degrees(atan(alfa_ry/alfa_r_north));
  else
    *azimuth = to_rev(360+to_degrees(atan(alfa_ry/alfa_r_north)));

  *elevation = 0.00;
  if (El_geometric > 10.2)
    *elevation = El_geometric+0.01617*(cos(to_radians(fabs(El_geometric)))/
                                       sin(to_radians(fabs(El_geometric))));
  else
    *elevation = El_geometric+refraction;
  if (alfa_r_zenith < -3000)
    *elevation = -99;
}

/*
 * Site Latitude
 * Site Longtitude
 * Site Altitude
 * Satellite Longtitute
 */
static double
usals_sat_angle( double site_lat, double site_lon,
                 double site_alt, double sat_lon )
{
  double azimuth, elevation;

  usals_sat_azimuth_and_elevation(site_lat, site_lon, site_alt, sat_lon,
                                   &azimuth, &elevation);

  tvhtrace("diseqc", "rotor angle azimuth %.4f elevation %.4f", azimuth, elevation);

  double rad_azimuth   = to_radians(azimuth);
  double rad_elevation = to_radians(elevation);
  double rad_site_lat  = to_radians(site_lat);
  double cos_elevation = cos(rad_elevation);
  double a, b, value;

  a = -cos_elevation * sin(rad_azimuth);
  b =  sin(rad_elevation) * cos(rad_site_lat) -
       cos_elevation * sin(rad_site_lat) * cos(rad_azimuth);

  value = 180 + to_degrees(atan(a/b));

  if (azimuth > 270) {
    value = value + 180;
    if (value > 360)
      value = 360 - (value-360);
  }
  if (azimuth < 90)
    value = 180 - value;

  return value;
}

/* USALS */
static int
linuxdvb_rotor_usals_tune
  ( linuxdvb_rotor_t *lr, dvb_mux_t *lm, linuxdvb_satconf_ele_t *ls, int fd )
{
  static const uint8_t xtable[10] =
         { 0x00, 0x02, 0x03, 0x05, 0x06, 0x08, 0x0A, 0x0B, 0x0D, 0x0E };

  double site_lat = lr->lr_site_lat;
  double site_lon = lr->lr_site_lon;
  double sat_lon  = lr->lr_sat_lon;
  double motor_angle;
  uint32_t tmp, cmd;
  int i;

  if (ls->lse_parent->ls_site_lat_south)
    site_lat = -site_lat;
  if (ls->lse_parent->ls_site_lon_west)
    site_lon = 360 - site_lon;
  if (sat_lon < 0)
    sat_lon = 360 - sat_lon;

  motor_angle = usals_sat_angle(lr->lr_site_lat, lr->lr_site_lon,
                                ls->lse_parent->ls_site_altitude,
                                lr->lr_sat_lon);

  if (site_lat >= 0) {
    tmp = round(fabs(180 - motor_angle) * 10.0);
    cmd = ((tmp / 10) * 0x10 + xtable[tmp % 10]) |
          (motor_angle < 180 ? 0xE000 : 0xD000);
  } else if (motor_angle < 180) {
    tmp = round(fabs(motor_angle) * 10.0);
    cmd = ((tmp / 10) * 0x10 + xtable[tmp % 10]) | 0xD000;
  } else {
    tmp = round(fabs(360 - motor_angle) * 10.0);
    cmd = ((tmp / 10) * 0x10 + xtable[tmp % 10]) | 0xE000;
  }

  tvhtrace("diseqc", "rotor USALS goto %0.1f%c (motor %0.1f %sclockwise)",
           fabs(lr->lr_sat_lon), (lr->lr_sat_lon > 0.0) ? 'E' : 'W',
           ((double)tmp / 10.0), (cmd & 0xF000) == 0xD000 ? "counter-" : "");

  for (i = 0; i <= ls->lse_parent->ls_diseqc_repeats; i++) {
    if (linuxdvb_diseqc_send(fd, 0xE0, 0x31, 0x6E, 2,
                             (cmd >> 8) & 0xff, cmd & 0xff)) {
      tvherror("diseqc", "failed to send USALS command");
      return -1;
    }
    usleep(25000);
  }

  return linuxdvb_rotor_grace((linuxdvb_diseqc_t*)lr,lm);

#undef TO_RAD
#undef TO_DEG
}

static int
linuxdvb_rotor_tune
  ( linuxdvb_diseqc_t *ld, dvb_mux_t *lm, linuxdvb_satconf_ele_t *ls, int fd )
{
  linuxdvb_rotor_t *lr = (linuxdvb_rotor_t*)ld;

  if (linuxdvb_rotor_check_orbital_pos(lm, ls))
    return 0;

  /* Force to 18v (quicker movement) */
  if (ioctl(fd, FE_SET_VOLTAGE, SEC_VOLTAGE_18)) {
    tvherror("diseqc", "failed to set 18v for rotor movement");
    return -1;
  }
  usleep(15000);
  ls->lse_parent->ls_last_pol = 2;

  /* GotoX */
  if (idnode_is_instance(&lr->ld_id, &linuxdvb_rotor_gotox_class))
    return linuxdvb_rotor_gotox_tune(lr, lm, ls, fd);

  /* USALS */
  return linuxdvb_rotor_usals_tune(lr, lm, ls, fd);
}

/* **************************************************************************
 * Create / Config
 * *************************************************************************/

struct {
  const char      *name;
  const idclass_t *idc;
} linuxdvb_rotor_all[] = {
  {
    .name = "GOTOX",
    .idc  = &linuxdvb_rotor_gotox_class
  },
  {
    .name = "USALS",
    .idc  = &linuxdvb_rotor_usals_class
  }
};

htsmsg_t *
linuxdvb_rotor_list ( void *o )
{
  int i;
  htsmsg_t *m = htsmsg_create_list(); 
  htsmsg_add_str(m, NULL, "None");
  for (i = 0; i < ARRAY_SIZE(linuxdvb_rotor_all); i++)
    htsmsg_add_str(m, NULL, linuxdvb_rotor_all[i].name);
  return m;
}

linuxdvb_diseqc_t *
linuxdvb_rotor_create0
  ( const char *name, htsmsg_t *conf, linuxdvb_satconf_ele_t *ls )
{
  int i;
  linuxdvb_diseqc_t *ld = NULL;

  for (i = 0; i < ARRAY_SIZE(linuxdvb_rotor_all); i++) {
    if (!strcmp(name ?: "", linuxdvb_rotor_all[i].name)) {
      ld = linuxdvb_diseqc_create0(calloc(1, sizeof(linuxdvb_rotor_t)),
                                   NULL, linuxdvb_rotor_all[i].idc, conf,
                                   linuxdvb_rotor_all[i].name, ls);
      if (ld) {
        ld->ld_tune  = linuxdvb_rotor_tune;
        ld->ld_grace = linuxdvb_rotor_grace;
      }
    }
  }
                                 
  return ld;
}

void
linuxdvb_rotor_destroy ( linuxdvb_diseqc_t *lr )
{
  linuxdvb_diseqc_destroy(lr);
  free(lr);
}

/******************************************************************************
 * Editor Configuration
 *
 * vim:sts=2:ts=2:sw=2:et
 *****************************************************************************/
