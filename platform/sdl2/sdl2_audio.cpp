/*
Portable ZX-Spectrum emulator.
Copyright (C) 2001-2013 SMT, Dexus, Alone Coder, deathsoft, djdron, scor

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "../platform.h"

#ifdef USE_SDL2

#include <SDL.h>
#include "../../options_common.h"
#include "../../tools/options.h"
#include "../../tools/sound_mixer.h"
#ifdef USE_SDL2_DESKTOP
#include "../sdl2_desktop/sdl2_desktop_pacing.h"
#include "../sdl2_desktop/sdl2_desktop_audio_sync.h"
#endif//USE_SDL2_DESKTOP

namespace xPlatform
{

int OpSpeed();

static SDL_AudioDeviceID device = 0;

static eSoundMixer sound_mixer;

#ifdef USE_SDL2_DESKTOP
// The device is slaved to the emulated machine: the controller picks how fast
// the callback consumes the mixer, the resampler does it (sdl2_desktop_audio_sync.h).
static xPacing::AudioSync audio_sync;
static double audio_sample_rate = 44100.0; // what the device actually runs at (InitAudio)
static xPacing::Resampler resampler;
static unsigned underruns_total = 0; // callbacks that found the mixer short, ever
#endif//USE_SDL2_DESKTOP

static void AudioCallback(void* userdata, Uint8* stream, int len)
{
#ifdef USE_SDL2_DESKTOP
	// Stereo 16-bit: 4 bytes a frame. At a ratio of exactly 1.0 this is the
	// plain copy it always was; otherwise the mixer is read a little faster or
	// slower, by whatever keeps its fill level steady.
	{
		const int n_out = len / 4;
		const xPacing::Resampler::Result r = resampler.Process(
			(const int16_t*)sound_mixer.Ptr(), (int)(sound_mixer.Ready() / 4), (int16_t*)stream, n_out, audio_sync.Ratio());
		sound_mixer.Use((dword)r.consumed * 4);
		if(r.produced < n_out)
		{
			memset(stream + r.produced * 4, 0, (size_t)(n_out - r.produced) * 4);
			++underruns_total;
		}
		if(len % 4)
			memset(stream + n_out * 4, 0, (size_t)(len % 4));
		return;
	}
#endif//USE_SDL2_DESKTOP
	if((dword)len <= sound_mixer.Ready())
	{
		memcpy(stream, sound_mixer.Ptr(), len);
		sound_mixer.Use(len);
	}
	else
	{
		memcpy(stream, sound_mixer.Ptr(), sound_mixer.Ready());
		memset(stream + sound_mixer.Ready(), 0, len - sound_mixer.Ready());
		sound_mixer.Use(sound_mixer.Ready());
	}
}

bool InitAudio()
{
	using namespace xOptions;
	struct eOptionBX : public eOptionB
	{
		void Unuse() { customizable = false; storeable = false; }
	};
	eOptionBX* o = (eOptionBX*)eOptionB::Find("sound");
	SAFE_CALL(o)->Unuse();
	o = (eOptionBX*)eOptionB::Find("volume");
	SAFE_CALL(o)->Unuse();

	SDL_AudioSpec audio;
	memset(&audio, 0, sizeof(audio));
	audio.freq = 44100;
	audio.channels = 2;
	audio.format = AUDIO_S16;
#ifndef SDL_AUDIO_SAMPLES
#define SDL_AUDIO_SAMPLES 1024
#endif//SDL_AUDIO_SAMPLES
	audio.samples = SDL_AUDIO_SAMPLES;
	audio.callback = AudioCallback;
	SDL_AudioSpec obtained;
	device = SDL_OpenAudioDevice(NULL, 0, &audio, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if(!device)
		return false;
	Handler()->AudioSetSampleRate(obtained.freq);
#ifdef USE_SDL2_DESKTOP
	audio_sample_rate = obtained.freq;
#endif//USE_SDL2_DESKTOP
	SDL_PauseAudioDevice(device, 0);
	return true;
}
void DoneAudio()
{
	if(device)
	{
		SDL_PauseAudioDevice(device, 1);
		SDL_CloseAudioDevice(device);
		device = 0;
	}
}

void UpdateAudio()
{
	if(!device)
		return;

	SDL_LockAudioDevice(device);
	sound_mixer.Update();
#ifdef USE_SDL2_DESKTOP
	{
		// Emulation is paced by the frame scheduler against the display, not by
		// this buffer; audio follows it (see sdl2_desktop_frame_sched.h). All that
		// is left to do here is steer the consumption ratio and watch for runaway.
		static double last_s = 0;
		static unsigned underruns_seen = 0;
		static bool held = false;
		const double now = xPacing::NowSeconds();
		const double dt = last_s > 0 ? now - last_s : 0.0;
		last_s = now;
		const double fill = sound_mixer.Ready() / 4.0; // frames queued
		// AudioSources() is 0 while fast tape loading: nothing is produced, by design
		const bool active = Handler()->AudioSources() != 0 && !OpSpeed();
		const bool underran = underruns_total != underruns_seen;
		underruns_seen = underruns_total;
		audio_sync.Update(fill, dt, active, underran);

		// Safety valve, not a pacer: if the clocks ever run away further than the
		// controller can follow (it moves the pitch by at most 1.5 %), stop making
		// frames until the mixer is back under 5 frames, rather than let it fill
		// without limit. 6 frames is 120 ms; in normal operation the fill never
		// gets past 4.
		const double frame = audio_sync.FrameSamples();
		const bool held_new = active && (held ? fill > 5 * frame : fill > 6 * frame);
		if(held != held_new)
		{
			held = held_new;
			Handler()->VideoPaused(held);
		}
	}
#else
	static bool audio_filled = false;
	bool audio_filled_new = !OpSpeed() && sound_mixer.Ready() > 44100*2*2/50*3; // 3-frame data
	if(audio_filled != audio_filled_new)
	{
		audio_filled = audio_filled_new;
		Handler()->VideoPaused(audio_filled);
	}
#endif//USE_SDL2_DESKTOP
	SDL_UnlockAudioDevice(device);
}

#ifdef USE_SDL2_DESKTOP
namespace xPacing
{
bool AudioRefillWanted() { return device && audio_sync.RefillWanted(); }
double AudioSampleRate() { return audio_sample_rate; }
void AudioSetMachineHz(double hz)
{
	if(hz > 0.0)
		audio_sync.SetFrameSamples(audio_sample_rate / hz);
}
void AudioRefillDone()
{
	audio_sync.RefillDone();
}
}//namespace xPacing
#endif//USE_SDL2_DESKTOP

}
//namespace xPlatform

#endif//USE_SDL2
