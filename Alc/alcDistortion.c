/**
 * OpenAL cross platform audio library
 * Copyright (C) 2013 by Mike Gorchak
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 *  Boston, MA  02111-1307, USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <math.h>
#include <stdlib.h>

#include "alMain.h"
#include "alFilter.h"
#include "alAuxEffectSlot.h"
#include "alError.h"
#include "alu.h"

/* Filters implementation is based on the "Cookbook formulae for audio   *
 * EQ biquad filter coefficients" by Robert Bristow-Johnson              *
 * http://www.musicdsp.org/files/Audio-EQ-Cookbook.txt                   */

typedef enum ALEQFilterType {
    LOWPASS,
    BANDPASS,
} ALEQFilterType;

typedef struct ALEQFilter {
    ALEQFilterType type;
    ALfloat x[2]; /* History of two last input samples  */
    ALfloat y[2]; /* History of two last output samples */
    ALfloat a[3]; /* Transfer function coefficients "a" */
    ALfloat b[3]; /* Transfer function coefficients "b" */
} ALEQFilter;

typedef struct ALdistortionState {
    DERIVE_FROM_TYPE(ALeffectState);

    /* Effect gains for each channel */
    ALfloat Gain[MaxChannels];

    /* Effect parameters */
    ALEQFilter bandpass;
    ALEQFilter lowpass;
    ALfloat attenuation;
    ALfloat edge_coeff;
} ALdistortionState;

static ALvoid ALdistortionState_Destroy(ALdistortionState *state)
{
    free(state);
}

static ALboolean ALdistortionState_DeviceUpdate(ALdistortionState *state, ALCdevice *Device)
{
    return AL_TRUE;
    (void)state;
    (void)Device;
}

static ALvoid ALdistortionState_Update(ALdistortionState *state, ALCdevice *Device, const ALeffectslot *Slot)
{
    ALfloat gain = sqrtf(1.0f / Device->NumChan) * Slot->Gain;
    ALfloat frequency = (ALfloat)Device->Frequency;
    ALuint it;
    ALfloat w0;
    ALfloat alpha;
    ALfloat bandwidth;
    ALfloat cutoff;
    ALfloat edge;

    for(it = 0;it < MaxChannels;it++)
        state->Gain[it] = 0.0f;
    for(it = 0;it < Device->NumChan;it++)
    {
        enum Channel chan = Device->Speaker2Chan[it];
        state->Gain[chan] = gain;
    }

    /* Store distorted signal attenuation settings */
    state->attenuation = Slot->effect.Distortion.Gain;

    /* Store waveshaper edge settings */
    edge = sinf(Slot->effect.Distortion.Edge * (F_PI/2.0f));
    state->edge_coeff = 2.0f * edge / (1.0f-edge);

    /* Lowpass filter */
    cutoff = Slot->effect.Distortion.LowpassCutoff;
    /* Bandwidth value is constant in octaves */
    bandwidth = (cutoff / 2.0f) / (cutoff * 0.67f);
    w0 = 2.0f*F_PI * cutoff / (frequency*4.0f);
    alpha = sinf(w0) * sinhf(logf(2.0f) / 2.0f * bandwidth * w0 / sinf(w0));
    state->lowpass.b[0] = (1.0f - cosf(w0)) / 2.0f;
    state->lowpass.b[1] = 1.0f - cosf(w0);
    state->lowpass.b[2] = (1.0f - cosf(w0)) / 2.0f;
    state->lowpass.a[0] = 1.0f + alpha;
    state->lowpass.a[1] = -2.0f * cosf(w0);
    state->lowpass.a[2] = 1.0f - alpha;

    /* Bandpass filter */
    cutoff = Slot->effect.Distortion.EQCenter;
    /* Convert bandwidth in Hz to octaves */
    bandwidth = Slot->effect.Distortion.EQBandwidth / (cutoff * 0.67f);
    w0 = 2.0f*F_PI * cutoff / (frequency*4.0f);
    alpha = sinf(w0) * sinhf(logf(2.0f) / 2.0f * bandwidth * w0 / sinf(w0));
    state->bandpass.b[0] = alpha;
    state->bandpass.b[1] = 0;
    state->bandpass.b[2] = -alpha;
    state->bandpass.a[0] = 1.0f + alpha;
    state->bandpass.a[1] = -2.0f * cosf(w0);
    state->bandpass.a[2] = 1.0f - alpha;
}

static ALvoid ALdistortionState_Process(ALdistortionState *state, ALuint SamplesToDo, const ALfloat *RESTRICT SamplesIn, ALfloat (*RESTRICT SamplesOut)[BUFFERSIZE])
{
    const ALfloat fc = state->edge_coeff;
    float oversample_buffer[64][4];
    ALfloat tempsmp;
    ALuint base;
    ALuint it;
    ALuint ot;
    ALuint kt;

    for(base = 0;base < SamplesToDo;)
    {
        ALfloat temps[64];
        ALuint td = minu(SamplesToDo-base, 64);

        /* Perform 4x oversampling to avoid aliasing.   */
        /* Oversampling greatly improves distortion     */
        /* quality and allows to implement lowpass and  */
        /* bandpass filters using high frequencies, at  */
        /* which classic IIR filters became unstable.   */

        /* Fill oversample buffer using zero stuffing */
        for(it = 0;it < td;it++)
        {
            oversample_buffer[it][0] = SamplesIn[it+base];
            oversample_buffer[it][1] = 0.0f;
            oversample_buffer[it][2] = 0.0f;
            oversample_buffer[it][3] = 0.0f;
        }

        /* First step, do lowpass filtering of original signal,  */
        /* additionally perform buffer interpolation and lowpass */
        /* cutoff for oversampling (which is fortunately first   */
        /* step of distortion). So combine three operations into */
        /* the one.                                              */
        for(it = 0;it < td;it++)
        {
            for(ot = 0;ot < 4;ot++)
            {
                tempsmp = state->lowpass.b[0] / state->lowpass.a[0] * oversample_buffer[it][ot] +
                          state->lowpass.b[1] / state->lowpass.a[0] * state->lowpass.x[0] +
                          state->lowpass.b[2] / state->lowpass.a[0] * state->lowpass.x[1] -
                          state->lowpass.a[1] / state->lowpass.a[0] * state->lowpass.y[0] -
                          state->lowpass.a[2] / state->lowpass.a[0] * state->lowpass.y[1];

                state->lowpass.x[1] = state->lowpass.x[0];
                state->lowpass.x[0] = oversample_buffer[it][ot];
                state->lowpass.y[1] = state->lowpass.y[0];
                state->lowpass.y[0] = tempsmp;
                /* Restore signal power by multiplying sample by amount of oversampling */
                oversample_buffer[it][ot] = tempsmp * 4.0f;
            }
        }

        for(it = 0;it < td;it++)
        {
            /* Second step, do distortion using waveshaper function  */
            /* to emulate signal processing during tube overdriving. */
            /* Three steps of waveshaping are intended to modify     */
            /* waveform without boost/clipping/attenuation process.  */
            for(ot = 0;ot < 4;ot++)
            {
                ALfloat smp = oversample_buffer[it][ot];

                smp = (1.0f + fc) * smp/(1.0f + fc*fabsf(smp));
                smp = (1.0f + fc) * smp/(1.0f + fc*fabsf(smp)) * -1.0f;
                smp = (1.0f + fc) * smp/(1.0f + fc*fabsf(smp));

                /* Third step, do bandpass filtering of distorted signal */
                tempsmp = state->bandpass.b[0] / state->bandpass.a[0] * smp +
                          state->bandpass.b[1] / state->bandpass.a[0] * state->bandpass.x[0] +
                          state->bandpass.b[2] / state->bandpass.a[0] * state->bandpass.x[1] -
                          state->bandpass.a[1] / state->bandpass.a[0] * state->bandpass.y[0] -
                          state->bandpass.a[2] / state->bandpass.a[0] * state->bandpass.y[1];

                state->bandpass.x[1] = state->bandpass.x[0];
                state->bandpass.x[0] = smp;
                state->bandpass.y[1] = state->bandpass.y[0];
                state->bandpass.y[0] = tempsmp;

                oversample_buffer[it][ot] = tempsmp;
            }

            /* Fourth step, final, do attenuation and perform decimation, */
            /* store only one sample out of 4.                            */
            temps[it] = oversample_buffer[it][0] * state->attenuation;
        }

        for(kt = 0;kt < MaxChannels;kt++)
        {
            ALfloat gain = state->Gain[kt];
            if(!(gain > 0.00001f))
                continue;

            for(it = 0;it < td;it++)
                SamplesOut[kt][base+it] += gain * temps[it];
        }

        base += td;
    }
}

DEFINE_ALEFFECTSTATE_VTABLE(ALdistortionState);

ALeffectState *DistortionCreate(void)
{
    ALdistortionState *state;

    state = malloc(sizeof(*state));
    if(!state) return NULL;
    SET_VTABLE2(ALdistortionState, ALeffectState, state);

    state->bandpass.type = BANDPASS;
    state->lowpass.type = LOWPASS;

    /* Initialize sample history only on filter creation to avoid */
    /* sound clicks if filter settings were changed in runtime.   */
    state->bandpass.x[0] = 0.0f;
    state->bandpass.x[1] = 0.0f;
    state->lowpass.y[0] = 0.0f;
    state->lowpass.y[1] = 0.0f;

    return STATIC_CAST(ALeffectState, state);
}

void distortion_SetParami(ALeffect *effect, ALCcontext *context, ALenum param, ALint val)
{
    effect=effect;
    val=val;

    switch(param)
    {
        default:
            alSetError(context, AL_INVALID_ENUM);
            break;
    }
}
void distortion_SetParamiv(ALeffect *effect, ALCcontext *context, ALenum param, const ALint *vals)
{
    distortion_SetParami(effect, context, param, vals[0]);
}
void distortion_SetParamf(ALeffect *effect, ALCcontext *context, ALenum param, ALfloat val)
{
    switch(param)
    {
        case AL_DISTORTION_EDGE:
            if(val >= AL_DISTORTION_MIN_EDGE && val <= AL_DISTORTION_MAX_EDGE)
                effect->Distortion.Edge = val;
            else
                alSetError(context, AL_INVALID_VALUE);
            break;

        case AL_DISTORTION_GAIN:
            if(val >= AL_DISTORTION_MIN_GAIN && val <= AL_DISTORTION_MAX_GAIN)
                effect->Distortion.Gain = val;
            else
                alSetError(context, AL_INVALID_VALUE);
            break;

        case AL_DISTORTION_LOWPASS_CUTOFF:
            if(val >= AL_DISTORTION_MIN_LOWPASS_CUTOFF && val <= AL_DISTORTION_MAX_LOWPASS_CUTOFF)
                effect->Distortion.LowpassCutoff = val;
            else
                alSetError(context, AL_INVALID_VALUE);
            break;

        case AL_DISTORTION_EQCENTER:
            if(val >= AL_DISTORTION_MIN_EQCENTER && val <= AL_DISTORTION_MAX_EQCENTER)
                effect->Distortion.EQCenter = val;
            else
                alSetError(context, AL_INVALID_VALUE);
            break;

        case AL_DISTORTION_EQBANDWIDTH:
            if(val >= AL_DISTORTION_MIN_EQBANDWIDTH && val <= AL_DISTORTION_MAX_EQBANDWIDTH)
                effect->Distortion.EQBandwidth = val;
            else
                alSetError(context, AL_INVALID_VALUE);
            break;

        default:
            alSetError(context, AL_INVALID_ENUM);
            break;
    }
}
void distortion_SetParamfv(ALeffect *effect, ALCcontext *context, ALenum param, const ALfloat *vals)
{
    distortion_SetParamf(effect, context, param, vals[0]);
}

void distortion_GetParami(ALeffect *effect, ALCcontext *context, ALenum param, ALint *val)
{
    effect=effect;
    val=val;

    switch(param)
    {
        default:
            alSetError(context, AL_INVALID_ENUM);
            break;
    }
}
void distortion_GetParamiv(ALeffect *effect, ALCcontext *context, ALenum param, ALint *vals)
{
    distortion_GetParami(effect, context, param, vals);
}
void distortion_GetParamf(ALeffect *effect, ALCcontext *context, ALenum param, ALfloat *val)
{
    switch(param)
    {
        case AL_DISTORTION_EDGE:
            *val = effect->Distortion.Edge;
            break;

        case AL_DISTORTION_GAIN:
            *val = effect->Distortion.Gain;
            break;

        case AL_DISTORTION_LOWPASS_CUTOFF:
            *val = effect->Distortion.LowpassCutoff;
            break;

        case AL_DISTORTION_EQCENTER:
            *val = effect->Distortion.EQCenter;
            break;

        case AL_DISTORTION_EQBANDWIDTH:
            *val = effect->Distortion.EQBandwidth;
            break;

        default:
            alSetError(context, AL_INVALID_ENUM);
            break;
    }
}
void distortion_GetParamfv(ALeffect *effect, ALCcontext *context, ALenum param, ALfloat *vals)
{
    distortion_GetParamf(effect, context, param, vals);
}
