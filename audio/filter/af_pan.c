/*
 * Copyright (C) 2002 Anders Johansson ajh@atri.curtin.edu.au
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

#include <inttypes.h>
#include <math.h>
#include <limits.h>

#include "af.h"

// Data for specific instances of this filter
typedef struct af_pan_s
{
  int nch; // Number of output channels; zero means same as input
  float level[AF_NCH][AF_NCH];	// Gain level for each channel
}af_pan_t;

static void set_channels(struct mp_audio *mpa, int num)
{
    struct mp_chmap map;
    // "unknown" channel layouts make it easier to pass through audio data,
    // without triggering remixing.
    mp_chmap_set_unknown(&map, num);
    mp_audio_set_channels(mpa, &map);
}

// Initialization and runtime control
static int control(struct af_instance* af, int cmd, void* arg)
{
  af_pan_t* s = af->setup;

  switch(cmd){
  case AF_CONTROL_REINIT:
    // Sanity check
    if(!arg) return AF_ERROR;

    af->data->rate   = ((struct mp_audio*)arg)->rate;
    mp_audio_set_format(af->data, AF_FORMAT_FLOAT_NE);
    set_channels(af->data, s->nch ? s->nch: ((struct mp_audio*)arg)->nch);
    af->mul          = (double)af->data->nch / ((struct mp_audio*)arg)->nch;

    if((af->data->format != ((struct mp_audio*)arg)->format) ||
       (af->data->bps != ((struct mp_audio*)arg)->bps)){
      mp_audio_set_format((struct mp_audio*)arg, af->data->format);
      return AF_FALSE;
    }
    return AF_OK;
  case AF_CONTROL_COMMAND_LINE:{
    int   nch = 0;
    int   n = 0;
    char* cp = NULL;
    int   j,k;
    // Read number of outputs
    sscanf((char*)arg,"%i%n", &nch,&n);
    if(AF_OK != control(af,AF_CONTROL_PAN_NOUT | AF_CONTROL_SET, &nch))
      return AF_ERROR;

    // Read pan values
    cp = &((char*)arg)[n];
    j = 0; k = 0;
    while((*cp == ':') && (k < AF_NCH)){
      sscanf(cp, ":%f%n" , &s->level[j][k], &n);
      mp_msg(MSGT_AFILTER, MSGL_V, "[pan] Pan level from channel %i to"
	     " channel %i = %f\n",k,j,s->level[j][k]);
      cp =&cp[n];
      j++;
      if(j>=nch){
	j = 0;
	k++;
      }
    }
    return AF_OK;
  }
  case AF_CONTROL_PAN_LEVEL | AF_CONTROL_SET:{
    int    i;
    int    ch = ((af_control_ext_t*)arg)->ch;
    float* level = ((af_control_ext_t*)arg)->arg;
    if (ch >= AF_NCH)
      return AF_FALSE;
    for(i=0;i<AF_NCH;i++)
      s->level[ch][i] = level[i];
    return AF_OK;
  }
  case AF_CONTROL_PAN_LEVEL | AF_CONTROL_GET:{
    int    i;
    int ch = ((af_control_ext_t*)arg)->ch;
    float* level = ((af_control_ext_t*)arg)->arg;
    if (ch >= AF_NCH)
      return AF_FALSE;
    for(i=0;i<AF_NCH;i++)
      level[i] = s->level[ch][i];
    return AF_OK;
  }
  case AF_CONTROL_PAN_NOUT | AF_CONTROL_SET:
    // Reinit must be called after this function has been called

    // Sanity check
    if(((int*)arg)[0] <= 0 || ((int*)arg)[0] > AF_NCH){
      mp_msg(MSGT_AFILTER, MSGL_ERR, "[pan] The number of output channels must be"
            " between 1 and %i. Current value is %i\n",AF_NCH,((int*)arg)[0]);
      return AF_ERROR;
    }
    s->nch=((int*)arg)[0];
    return AF_OK;
  case AF_CONTROL_PAN_BALANCE | AF_CONTROL_SET:{
    float val = *(float*)arg;
    if (s->nch)
      return AF_ERROR;
    if (af->data->nch >= 2) {
      s->level[0][0] = min(1.f, 1.f - val);
      s->level[0][1] = max(0.f, val);
      s->level[1][0] = max(0.f, -val);
      s->level[1][1] = min(1.f, 1.f + val);
    }
    return AF_OK;
  }
  case AF_CONTROL_PAN_BALANCE | AF_CONTROL_GET:
    if (s->nch)
      return AF_ERROR;
    *(float*)arg = s->level[0][1] - s->level[1][0];
    return AF_OK;
  }
  return AF_UNKNOWN;
}

// Deallocate memory
static void uninit(struct af_instance* af)
{
  if(af->data)
    free(af->data->audio);
  free(af->data);
  free(af->setup);
}

// Filter data through filter
static struct mp_audio* play(struct af_instance* af, struct mp_audio* data)
{
  struct mp_audio*    c    = data;		// Current working data
  struct mp_audio*	l    = af->data;	// Local data
  af_pan_t*  	s    = af->setup; 	// Setup for this instance
  float*   	in   = c->audio;	// Input audio data
  float*   	out  = NULL;		// Output audio data
  float*	end  = in+c->len/4; 	// End of loop
  int		nchi = c->nch;		// Number of input channels
  int		ncho = l->nch;		// Number of output channels
  register int  j,k;

  if(AF_OK != RESIZE_LOCAL_BUFFER(af,data))
    return NULL;

  out = l->audio;
  // Execute panning
  // FIXME: Too slow
  while(in < end){
    for(j=0;j<ncho;j++){
      register float  x   = 0.0;
      register float* tin = in;
      for(k=0;k<nchi;k++)
	x += tin[k] * s->level[j][k];
      out[j] = x;
    }
    out+= ncho;
    in+= nchi;
  }

  // Set output data
  c->audio = l->audio;
  c->len   = c->len / c->nch * l->nch;
  set_channels(c, l->nch);

  return c;
}

// Allocate memory and set function pointers
static int af_open(struct af_instance* af){
  af->control=control;
  af->uninit=uninit;
  af->play=play;
  af->mul=1;
  af->data=calloc(1,sizeof(struct mp_audio));
  af->setup=calloc(1,sizeof(af_pan_t));
  if(af->data == NULL || af->setup == NULL)
    return AF_ERROR;
  return AF_OK;
}

// Description of this filter
struct af_info af_info_pan = {
    .info = "Panning audio filter",
    .name = "pan",
    .open = af_open,
};
