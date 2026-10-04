/*
Portable ZX-Spectrum emulator.
Copyright (C) 2001-2026 SMT, Dexus, Alone Coder, deathsoft, djdron, scor

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

#ifndef __SDL2_DESKTOP_AUDIO_SYNC_H__
#define __SDL2_DESKTOP_AUDIO_SYNC_H__

#pragma once

// =============================================================================
//  platform/sdl2_desktop/sdl2_desktop_audio_sync.h
//
//  Slaves the audio device to the emulated machine (see
//  sdl2_desktop_frame_sched.h for why the machine is the master).
//
//  The emulator hands the mixer 882 samples per emulated frame; the device
//  takes 44100 per second. Over a minute the two clocks drift apart by tens of
//  milliseconds (the display, the audio hardware and the system timer are three
//  separate crystals) and, in Locked mode, by up to 0.8 % by design. Whatever
//  the cause, the symptom is the same: the mixer's fill level creeps up or
//  down, and when it runs out the device plays silence (a click).
//
//    AudioSync   a PI controller keeping the mixer's fill level at a target:
//                it picks the ratio of input samples consumed per output sample,
//                1.000 +- a fraction of a percent. Fill high -> consume a
//                little faster, fill low -> a little slower.
//    Resampler   what that ratio is applied with, in the device callback:
//                8-point Lagrange interpolation, which at these ratios keeps
//                the signal clean up to the top of the band and (unlike
//                dropping or repeating samples) makes no click. With the ratio
//                at exactly 1.0 it is a plain copy, bit for bit.
//
//  Header-only and free of SDL/emulator dependencies on purpose, so it can be
//  driven by synthetic timelines in a test.
// =============================================================================

#include <cmath>
#include <cstdint>
#include <cstring>

namespace xPlatform
{
namespace xPacing
{

class AudioSync
{
public:
	static constexpr double kFrameSamples = 882.0;   // one emulated frame at 44100 Hz
	static constexpr double kTargetFrames = 2.5;     // fill level to hold, in emulated frames
	static constexpr double kMaxDeviation = 0.015;   // consumption ratio stays within 1 +- this
	static constexpr double kKp = 0.016;             // per frame of fill error
	static constexpr double kKi = 0.0032;            // per frame-second of fill error
	static constexpr double kTauS = 1.0;             // smoothing of the (saw-toothed) fill level
	static constexpr double kStarvedFrames = 0.25;   // below this the mixer is empty, not low
	static constexpr int kRefillMax = 3;             // most extra frames run to put the cushion back after an event
	static constexpr double kRefillStopFrames = 2.0; // ...and the level that counts as put back (a frame is the smallest step, so stop a half frame short)

	void Reset() { *this = AudioSync(); }

	// Once per presented frame, main thread, with the mixer in its post-
	// production state. 'fill_samples' is what is queued; 'active' is false
	// while no audio is meant to flow (fast tape loading, fast forward);
	// 'underran' says the device callback found the mixer short since the last
	// call (the callback counts them; this class doesn't know the device).
	void Update(double fill_samples, double dt_s, bool active, bool underran)
	{
		if(dt_s <= 0.0 || dt_s > 1.0)
			dt_s = dt_s <= 0.0 ? 0.0 : 1.0;

		if(!active)
		{
			// Nothing is being produced: whatever the fill does says nothing
			// about the clocks. Stand down at exactly 1.0 and forget the past.
			m_i = 0;
			m_ratio = 1.0;
			m_have = false;
			m_refilling = false;
			m_inactive = true;
			return;
		}
		const bool resumed = m_inactive; // audio is back and the mixer was left to drain
		m_inactive = false;

		const bool starved = fill_samples < kStarvedFrames * kFrameSamples;
		if((starved || underran || resumed) && !m_refilling)
		{
			// Empty (a pause, a stall, a long load) or caught short. Producing
			// and consuming at the same rate the mixer would stay that way -
			// the 1.5 % per second the controller can add is seconds of
			// dropouts followed by seconds of detuned pitch - so run extra
			// frames, one per present, until the cushion is back
			// (RefillWanted()).
			m_refilling = true;
			m_refill_left = kRefillMax;
		}
		if(m_refilling && fill_samples >= kRefillStopFrames * kFrameSamples)
			m_refilling = false; // restored
		if(m_refilling || starved)
		{
			// While that is under way the fill level says nothing about the
			// clocks, and what the refill is for is restoring exactly the
			// target: so stand down at 1.0 and let the smoothed level start
			// from there, instead of from the empty buffer it still remembers
			// (which pulls the pitch down by the full 1.5 % for seconds).
			m_i = 0;
			m_ratio = 1.0;
			m_avg = kTargetFrames * kFrameSamples;
			m_have = true;
			return;
		}

		if(!m_have)
		{
			m_avg = fill_samples;
			m_have = true;
		}
		else
		{
			const double a = 1.0 - std::exp(-dt_s / kTauS);
			m_avg += a * (fill_samples - m_avg);
		}

		const double err = (m_avg - kTargetFrames * kFrameSamples) / kFrameSamples; // frames
		m_i += err * dt_s;
		const double i_limit = kMaxDeviation / kKi;
		if(m_i > i_limit) m_i = i_limit;
		if(m_i < -i_limit) m_i = -i_limit;
		double u = kKp * err + kKi * m_i;
		if(u > kMaxDeviation) u = kMaxDeviation;
		if(u < -kMaxDeviation) u = -kMaxDeviation;
		m_ratio = 1.0 + u;
	}

	// True while the mixer is being refilled after it ran empty (normally
	// false). The caller adds one to the frames it runs for a present and calls
	// RefillDone() once that frame really was emulated: a paused machine
	// produces nothing and must not use up the allowance.
	bool RefillWanted() const { return m_refilling; }
	void RefillDone() { if(m_refilling && --m_refill_left <= 0) m_refilling = false; }

	double Ratio() const { return m_ratio; }          // input samples consumed per output sample
	double FillAverage() const { return m_avg; }      // samples
	double Integral() const { return m_i; }

private:
	bool m_have = false, m_inactive = false, m_refilling = false;
	int m_refill_left = 0;
	double m_avg = 0.0, m_i = 0.0, m_ratio = 1.0;
};

// Stereo interleaved 16-bit, as the mixer produces and the device wants.
//
// Interpolation is an 8-point Lagrange polynomial (3 frames behind the read
// position, 4 ahead). Measured against the ideal on a pure tone (SNR, dB):
//                  1 kHz   4 kHz   8 kHz   12 kHz   16 kHz   20 kHz
//     4-point cubic   86     52      31      19       -        -
//     8-point Lagrange 89    89      56      31       16       6
// i.e. clean (16-bit-rounding limited) through 4 kHz and 25 dB better at 8 kHz,
// where a beeper's edges and the AY's noise start. Near Nyquist any short
// interpolator degrades; that last octave is not where this material lives.
class Resampler
{
public:
	struct Result { int consumed; int produced; };

	static constexpr int kBack = 3;   // frames needed before the read position
	static constexpr int kAhead = 4;  // ...and after it
	static constexpr int kTaps = kBack + 1 + kAhead;

	void Reset() { *this = Resampler(); }

	// Reads from 'in' (avail frames), writes up to n_out frames to 'out'. At
	// 'ratio' input frames per output frame. Stops early only when the input
	// runs out; the caller decides what to do about the rest. Frames are
	// consumed from the front: tell the mixer how many with Result::consumed.
	Result Process(const int16_t* in, int avail, int16_t* out, int n_out, double ratio)
	{
		Result r = { 0, 0 };
		if(ratio == 1.0 && m_pos == 0.0)
		{
			// The untouched path: bit-exact, and no lookahead needed.
			const int n = n_out < avail ? n_out : avail;
			if(n > 0)
			{
				std::memcpy(out, in, (size_t)n * 4);
				RememberHistory(in, n);
			}
			r.consumed = r.produced = n;
			return r;
		}

		double pos = m_pos;
		int j = 0;
		for(; j < n_out; ++j)
		{
			const int i = (int)pos;
			if(i >= avail)
				break;
			double w[kTaps];
			Weights(pos - i, w);
			for(int ch = 0; ch < 2; ++ch)
			{
				double y = 0.0;
				for(int k = 0; k < kTaps; ++k)
					y += w[k] * Sample(in, avail, i + k - kBack, ch);
				if(y > 32767.0) y = 32767.0;
				if(y < -32768.0) y = -32768.0;
				out[j * 2 + ch] = (int16_t)std::lrint(y);
			}
			pos += ratio;
		}
		int consumed = (int)pos;
		if(consumed > avail)
			consumed = avail;
		if(consumed > 0)
			RememberHistory(in, consumed);
		m_pos = pos - consumed;
		if(m_pos < 0.0) m_pos = 0.0;
		r.consumed = consumed;
		r.produced = j;
		return r;
	}

	double Phase() const { return m_pos; }

private:
	// Lagrange weights for nodes -kBack..+kAhead evaluated at f in [0, 1).
	static void Weights(double f, double* w)
	{
		double x[kTaps];
		for(int k = 0; k < kTaps; ++k)
			x[k] = (double)(k - kBack);
		double pre[kTaps + 1], suf[kTaps + 1];
		pre[0] = 1.0;
		for(int k = 0; k < kTaps; ++k)
			pre[k + 1] = pre[k] * (f - x[k]);
		suf[kTaps] = 1.0;
		for(int k = kTaps - 1; k >= 0; --k)
			suf[k] = suf[k + 1] * (f - x[k]);
		for(int k = 0; k < kTaps; ++k)
		{
			double den = 1.0;
			for(int m = 0; m < kTaps; ++m)
				if(m != k)
					den *= x[k] - x[m];
			w[k] = pre[k] * suf[k + 1] / den;
		}
	}

	// Frame 'idx' relative to the first unconsumed one: negative = history,
	// past the end = the last frame held.
	double Sample(const int16_t* in, int avail, int idx, int ch) const
	{
		if(idx < 0)
			return m_hist[kBack + idx][ch];
		if(idx >= avail)
			idx = avail - 1;
		return in[idx * 2 + ch];
	}

	// The last kBack frames of what has just been consumed.
	void RememberHistory(const int16_t* in, int consumed)
	{
		int16_t fresh[kBack][2];
		for(int h = 0; h < kBack; ++h)
		{
			const int idx = consumed - kBack + h; // < 0 -> still in the old history
			for(int ch = 0; ch < 2; ++ch)
				fresh[h][ch] = idx >= 0 ? in[idx * 2 + ch] : m_hist[kBack + idx][ch];
		}
		std::memcpy(m_hist, fresh, sizeof(m_hist));
	}

	double m_pos = 0.0;                // fractional read position within the first unconsumed frame
	int16_t m_hist[kBack][2] = {};     // the frames just before it, oldest first
};

}//namespace xPacing
}//namespace xPlatform

#endif//__SDL2_DESKTOP_AUDIO_SYNC_H__
