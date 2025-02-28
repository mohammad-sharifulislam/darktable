/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/imagebuf.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(2, dt_iop_velvia_params_t)

typedef struct dt_iop_velvia_params_t
{
  float strength; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 25.0
  float bias;     // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 1.0 $DESCRIPTION: "mid-tones bias"
} dt_iop_velvia_params_t;

/* legacy version 1 params */
typedef struct dt_iop_velvia_params1_t
{
  float saturation;
  float vibrance;
  float luminance;
  float clarity;
} dt_iop_velvia_params1_t;

typedef struct dt_iop_velvia_gui_data_t
{
  GtkBox *vbox;
  GtkWidget *strength_scale;
  GtkWidget *bias_scale;
} dt_iop_velvia_gui_data_t;

typedef struct dt_iop_velvia_data_t
{
  float strength;
  float bias;
} dt_iop_velvia_data_t;

typedef struct dt_iop_velvia_global_data_t
{
  int kernel_velvia;
} dt_iop_velvia_global_data_t;


const char *name()
{
  return _("velvia");
}

const char *aliases()
{
  return _("saturation");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_GRADING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

const char *description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("resaturate giving more weight to blacks, whites and low-saturation pixels"),
                                      _("creative"),
                                      _("linear, RGB, scene-referred"),
                                      _("linear, RGB"),
                                      _("linear, RGB, scene-referred"));
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    const dt_iop_velvia_params1_t *old = old_params;
    dt_iop_velvia_params_t *new = new_params;
    new->strength = old->saturation * old->vibrance / 100.0f;
    new->bias = old->luminance;
    return 0;
  }
  return 1;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_velvia_data_t *const data = (dt_iop_velvia_data_t *)piece->data;

  const size_t ch = piece->colors;
  const float strength = data->strength / 100.0f;

  // Apply velvia saturation
  if(strength <= 0.0)
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
  else
  {
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
    dt_omp_firstprivate(ch, data, ivoid, ovoid, roi_out, strength) \
    schedule(static)
#endif
    for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
    {
      const float *const in = (const float *const)ivoid + ch * k;
      float *const out = (float *const)ovoid + ch * k;

      // calculate vibrance, and apply boost velvia saturation at least saturated pixels
      float pmax = MAX(in[0], MAX(in[1], in[2])); // max value in RGB set
      float pmin = MIN(in[0], MIN(in[1], in[2])); // min value in RGB set
      float plum = (pmax + pmin) / 2.0f;          // pixel luminocity
      float psat = (plum <= 0.5f) ? (pmax - pmin) / (1e-5f + pmax + pmin)
                                  : (pmax - pmin) / (1e-5f + MAX(0.0f, 2.0f - pmax - pmin));

      float pweight
          = CLAMPS(((1.0f - (1.5f * psat)) + ((1.0f + (fabsf(plum - 0.5f) * 2.0f)) * (1.0f - data->bias)))
                       / (1.0f + (1.0f - data->bias)),
                   0.0f, 1.0f);              // The weight of pixel
      float saturation = strength * pweight; // So lets calculate the final affection of filter on pixel

      // Apply velvia saturation values
      out[0] = CLAMPS(in[0] + saturation * (in[0] - 0.5f * (in[1] + in[2])), 0.0f, 1.0f);
      out[1] = CLAMPS(in[1] + saturation * (in[1] - 0.5f * (in[2] + in[0])), 0.0f, 1.0f);
      out[2] = CLAMPS(in[2] + saturation * (in[2] - 0.5f * (in[0] + in[1])), 0.0f, 1.0f);
    }
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_velvia_data_t *data = (dt_iop_velvia_data_t *)piece->data;
  dt_iop_velvia_global_data_t *gd = (dt_iop_velvia_global_data_t *)self->global_data;

  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const float strength = data->strength / 100.0f;
  const float bias = data->bias;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  if(strength <= 0.0f)
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    dt_opencl_set_kernel_arg(devid, gd->kernel_velvia, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_velvia, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_velvia, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_velvia, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_velvia, 4, sizeof(float), (void *)&strength);
    dt_opencl_set_kernel_arg(devid, gd->kernel_velvia, 5, sizeof(float), (void *)&bias);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_velvia, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_velvia] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_velvia_global_data_t *gd
      = (dt_iop_velvia_global_data_t *)malloc(sizeof(dt_iop_velvia_global_data_t));
  module->data = gd;
  gd->kernel_velvia = dt_opencl_create_kernel(program, "velvia");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_velvia_global_data_t *gd = (dt_iop_velvia_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_velvia);
  free(module->data);
  module->data = NULL;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)p1;
  dt_iop_velvia_data_t *d = (dt_iop_velvia_data_t *)piece->data;

  d->strength = p->strength;
  d->bias = p->bias;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_velvia_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_velvia_gui_data_t *g = (dt_iop_velvia_gui_data_t *)self->gui_data;
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)self->params;
  dt_bauhaus_slider_set(g->strength_scale, p->strength);
  dt_bauhaus_slider_set(g->bias_scale, p->bias);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_velvia_gui_data_t *g = IOP_GUI_ALLOC(velvia);

  g->strength_scale = dt_bauhaus_slider_from_params(self, N_("strength"));
  dt_bauhaus_slider_set_format(g->strength_scale, "%.0f%%");
  gtk_widget_set_tooltip_text(g->strength_scale, _("the strength of saturation boost"));

  g->bias_scale = dt_bauhaus_slider_from_params(self, "bias");
  gtk_widget_set_tooltip_text(g->bias_scale, _("how much to spare highlights and shadows"));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
