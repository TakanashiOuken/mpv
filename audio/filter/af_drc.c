/*
 * Copyright (C) 2004 Alex Beregszaszi & Pierre Lombard
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <inttypes.h>
#include <math.h>
#include <limits.h>

#include "af.h"

// Methods:
// 1: uses a 1 value memory and coefficients new=a*old+b*cur (with a+b=1)
// 2: uses several samples to smooth the variations (standard weighted mean
//    on past samples)

// Size of the memory array
// FIXME: should depend on the frequency of the data (should be a few seconds)
#define NSAMPLES 128

// If summing all the mem[].len is lower than MIN_SAMPLE_SIZE bytes, then we
// choose to ignore the computed value as it's not significant enough
// FIXME: should depend on the frequency of the data (0.5s maybe)
#define MIN_SAMPLE_SIZE 32000

// mul is the value by which the samples are scaled
// and has to be in [MUL_MIN, MUL_MAX]
#define MUL_INIT 1.0
#define MUL_MIN 0.1
#define MUL_MAX 5.0

// Silence level
// FIXME: should be relative to the level of the samples
#define SIL_S16 (SHRT_MAX * 0.01)
#define SIL_FLOAT 0.01

// smooth must be in ]0.0, 1.0[
#define SMOOTH_MUL 0.06
#define SMOOTH_LASTAVG 0.06

#define DEFAULT_TARGET 0.25

// Data for specific instances of this filter
typedef struct af_volume_s
{
    int method; // method used
    float mul;
    // method 1
    float lastavg; // history value of the filter
    // method 2
    int idx;
    struct {
	float avg; // average level of the sample
	int len; // sample size (weight)
    } mem[NSAMPLES];
    // "Ideal" level
    float mid_s16;
    float mid_float;
}af_drc_t;

// Initialization and runtime control
static int control(struct af_instance* af, int cmd, void* arg)
{
  af_drc_t* s   = (af_drc_t*)af->setup;

  switch(cmd){
  case AF_CONTROL_REINIT:
    // Sanity check
    if(!arg) return AF_ERROR;

    mp_audio_copy_config(af->data, (struct mp_audio*)arg);

    if(((struct mp_audio*)arg)->format != (AF_FORMAT_S16_NE)){
      mp_audio_set_format(af->data, AF_FORMAT_FLOAT_NE);
    }
    return af_test_output(af,(struct mp_audio*)arg);
  case AF_CONTROL_COMMAND_LINE:{
    int   i = 0;
    float target = DEFAULT_TARGET;
    sscanf((char*)arg,"%d:%f", &i, &target);
    if (i != 1 && i != 2)
	return AF_ERROR;
    s->method = i-1;
    s->mid_s16 = ((float)SHRT_MAX) * target;
    s->mid_float = target;
    return AF_OK;
  }
  }
  return AF_UNKNOWN;
}

// Deallocate memory
static void uninit(struct af_instance* af)
{
    free(af->data);
    free(af->setup);
}

static void method1_int16(af_drc_t *s, struct mp_audio *c)
{
  register int i = 0;
  int16_t *data = (int16_t*)c->audio;	// Audio data
  int len = c->len/2;		// Number of samples
  float curavg = 0.0, newavg, neededmul;
  int tmp;

  for (i = 0; i < len; i++)
  {
    tmp = data[i];
    curavg += tmp * tmp;
  }
  curavg = sqrt(curavg / (float) len);

  // Evaluate an adequate 'mul' coefficient based on previous state, current
  // samples level, etc

  if (curavg > SIL_S16)
  {
    neededmul = s->mid_s16 / (curavg * s->mul);
    s->mul = (1.0 - SMOOTH_MUL) * s->mul + SMOOTH_MUL * neededmul;

    // clamp the mul coefficient
    s->mul = clamp(s->mul, MUL_MIN, MUL_MAX);
  }

  // Scale & clamp the samples
  for (i = 0; i < len; i++)
  {
    tmp = s->mul * data[i];
    tmp = clamp(tmp, SHRT_MIN, SHRT_MAX);
    data[i] = tmp;
  }

  // Evaulation of newavg (not 100% accurate because of values clamping)
  newavg = s->mul * curavg;

  // Stores computed values for future smoothing
  s->lastavg = (1.0 - SMOOTH_LASTAVG) * s->lastavg + SMOOTH_LASTAVG * newavg;
}

static void method1_float(af_drc_t *s, struct mp_audio *c)
{
  register int i = 0;
  float *data = (float*)c->audio;	// Audio data
  int len = c->len/4;		// Number of samples
  float curavg = 0.0, newavg, neededmul, tmp;

  for (i = 0; i < len; i++)
  {
    tmp = data[i];
    curavg += tmp * tmp;
  }
  curavg = sqrt(curavg / (float) len);

  // Evaluate an adequate 'mul' coefficient based on previous state, current
  // samples level, etc

  if (curavg > SIL_FLOAT) // FIXME
  {
    neededmul = s->mid_float / (curavg * s->mul);
    s->mul = (1.0 - SMOOTH_MUL) * s->mul + SMOOTH_MUL * neededmul;

    // clamp the mul coefficient
    s->mul = clamp(s->mul, MUL_MIN, MUL_MAX);
  }

  // Scale & clamp the samples
  for (i = 0; i < len; i++)
    data[i] *= s->mul;

  // Evaulation of newavg (not 100% accurate because of values clamping)
  newavg = s->mul * curavg;

  // Stores computed values for future smoothing
  s->lastavg = (1.0 - SMOOTH_LASTAVG) * s->lastavg + SMOOTH_LASTAVG * newavg;
}

static void method2_int16(af_drc_t *s, struct mp_audio *c)
{
  register int i = 0;
  int16_t *data = (int16_t*)c->audio;	// Audio data
  int len = c->len/2;		// Number of samples
  float curavg = 0.0, newavg, avg = 0.0;
  int tmp, totallen = 0;

  for (i = 0; i < len; i++)
  {
    tmp = data[i];
    curavg += tmp * tmp;
  }
  curavg = sqrt(curavg / (float) len);

  // Evaluate an adequate 'mul' coefficient based on previous state, current
  // samples level, etc
  for (i = 0; i < NSAMPLES; i++)
  {
    avg += s->mem[i].avg * (float)s->mem[i].len;
    totallen += s->mem[i].len;
  }

  if (totallen > MIN_SAMPLE_SIZE)
  {
    avg /= (float)totallen;
    if (avg >= SIL_S16)
    {
	s->mul = s->mid_s16 / avg;
	s->mul = clamp(s->mul, MUL_MIN, MUL_MAX);
    }
  }

  // Scale & clamp the samples
  for (i = 0; i < len; i++)
  {
    tmp = s->mul * data[i];
    tmp = clamp(tmp, SHRT_MIN, SHRT_MAX);
    data[i] = tmp;
  }

  // Evaulation of newavg (not 100% accurate because of values clamping)
  newavg = s->mul * curavg;

  // Stores computed values for future smoothing
  s->mem[s->idx].len = len;
  s->mem[s->idx].avg = newavg;
  s->idx = (s->idx + 1) % NSAMPLES;
}

static void method2_float(af_drc_t *s, struct mp_audio *c)
{
  register int i = 0;
  float *data = (float*)c->audio;	// Audio data
  int len = c->len/4;		// Number of samples
  float curavg = 0.0, newavg, avg = 0.0, tmp;
  int totallen = 0;

  for (i = 0; i < len; i++)
  {
    tmp = data[i];
    curavg += tmp * tmp;
  }
  curavg = sqrt(curavg / (float) len);

  // Evaluate an adequate 'mul' coefficient based on previous state, current
  // samples level, etc
  for (i = 0; i < NSAMPLES; i++)
  {
    avg += s->mem[i].avg * (float)s->mem[i].len;
    totallen += s->mem[i].len;
  }

  if (totallen > MIN_SAMPLE_SIZE)
  {
    avg /= (float)totallen;
    if (avg >= SIL_FLOAT)
    {
	s->mul = s->mid_float / avg;
	s->mul = clamp(s->mul, MUL_MIN, MUL_MAX);
    }
  }

  // Scale & clamp the samples
  for (i = 0; i < len; i++)
    data[i] *= s->mul;

  // Evaulation of newavg (not 100% accurate because of values clamping)
  newavg = s->mul * curavg;

  // Stores computed values for future smoothing
  s->mem[s->idx].len = len;
  s->mem[s->idx].avg = newavg;
  s->idx = (s->idx + 1) % NSAMPLES;
}

// Filter data through filter
static struct mp_audio* play(struct af_instance* af, struct mp_audio* data)
{
  af_drc_t *s = af->setup;

  if(af->data->format == (AF_FORMAT_S16_NE))
  {
    if (s->method)
	method2_int16(s, data);
    else
	method1_int16(s, data);
  }
  else if(af->data->format == (AF_FORMAT_FLOAT_NE))
  {
    if (s->method)
	method2_float(s, data);
    else
	method1_float(s, data);
  }
  return data;
}

// Allocate memory and set function pointers
static int af_open(struct af_instance* af){
  int i = 0;
  af->control=control;
  af->uninit=uninit;
  af->play=play;
  af->mul=1;
  af->data=calloc(1,sizeof(struct mp_audio));
  af->setup=calloc(1,sizeof(af_drc_t));
  if(af->data == NULL || af->setup == NULL)
    return AF_ERROR;

  ((af_drc_t*)af->setup)->mul = MUL_INIT;
  ((af_drc_t*)af->setup)->lastavg = ((float)SHRT_MAX) * DEFAULT_TARGET;
  ((af_drc_t*)af->setup)->idx = 0;
  ((af_drc_t*)af->setup)->mid_s16 = ((float)SHRT_MAX) * DEFAULT_TARGET;
  ((af_drc_t*)af->setup)->mid_float = DEFAULT_TARGET;
  for (i = 0; i < NSAMPLES; i++)
  {
     ((af_drc_t*)af->setup)->mem[i].len = 0;
     ((af_drc_t*)af->setup)->mem[i].avg = 0;
  }
  return AF_OK;
}

// Description of this filter
struct af_info af_info_drc = {
    .info = "Dynamic range compression filter",
    .name = "drc",
    .flags = AF_FLAGS_NOT_REENTRANT,
    .open = af_open,
};
