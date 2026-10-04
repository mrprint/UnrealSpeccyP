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

#ifndef __SDL2_DESKTOP_FRAME_SCHED_H__
#define __SDL2_DESKTOP_FRAME_SCHED_H__

#pragma once

// =============================================================================
//  platform/sdl2_desktop/sdl2_desktop_frame_sched.h
//
//  Decides, once per presented frame, how many emulated frames to run.
//
//  Three clocks are involved and only one of them may be the master: the
//  display (vsync), the audio device, and the emulated machine itself, whose
//  frame is exactly 20 ms long - 71680 T-states at the sound engine's tick
//  rate of 71680 * 50 (devices/sound/device_sound.h), i.e. 882 samples at
//  44100 Hz, i.e. 50.000 frames per second.
//
//  The machine is the master and the audio is slaved to it
//  (sdl2_desktop_audio_sync.h). There are two ways to step it:
//
//    Locked    The display runs at N times the machine's rate to within
//              LOCK_ENTER (50 Hz, 100 Hz, 150 Hz, or a hair off them): one
//              emulated frame every N presents, exactly. The machine then
//              follows the display (by under 1 %) instead of the other way
//              round, so no frame is ever repeated or dropped; audio absorbs
//              the difference.
//    Pulldown  Any other display (60, 75, 120, 144 Hz ...): an accumulator
//              advanced by real time releases one frame per 20.000 ms, which
//              spreads the repeats as evenly as they can be spread (a 60 Hz
//              display shows 1 1 1 1 2 1 1 1 1 2 ...). Speed is exact.
//
//  Header-only and free of SDL/ImGui/emulator dependencies on purpose, so it
//  can be driven by synthetic timelines in a test.
// =============================================================================

#include <cmath>

namespace xPlatform
{
namespace xPacing
{

class FrameScheduler
{
public:
	enum class Mode { Pulldown, Locked };

	static constexpr int kMaxLockN = 6;          // 50 Hz ... 300 Hz displays can lock
	static constexpr int kMaxSteps = 3;          // never run more than this many frames for one present
	static constexpr double kLockEnter = 0.008;  // |display / (N * machine) - 1| to start locking
	static constexpr double kLockExit = 0.012;   // ...and to stop (hysteresis)
	static constexpr double kJitterEnter = 0.04; // mean |interval - period| / period of a vsync-locked loop...
	static constexpr double kJitterExit = 0.08;  // ...a timer-paced one is several times worse
	static constexpr int kWarmup = 30;           // intervals measured before locking is considered
	static constexpr double kStallS = 0.25;      // a longer gap is a stall (modeset, dialog, debugger), not an interval

	explicit FrameScheduler(double machine_hz = 50.0) { SetMachineHz(machine_hz); }

	void SetMachineHz(double hz)
	{
		m_machine_hz = hz > 1.0 ? hz : 50.0;
		m_machine_period = 1.0 / m_machine_hz;
	}

	void Reset()
	{
		const double hz = m_machine_hz;
		*this = FrameScheduler(hz);
	}

	// Call once per presented frame, right after the previous present returned.
	// 'bypass' is for the modes that deliberately run as fast as they can
	// (fast forward, fast tape loading): one frame per call, and the state is
	// dropped so that nothing bursts on the way back.
	// Returns how many emulated frames to run now: 0..kMaxSteps.
	int Steps(double now_s, bool bypass)
	{
		if(bypass)
		{
			DropState(now_s);
			return 1;
		}
		if(!m_started)
		{
			m_started = true;
			m_last = now_s;
			m_acc = m_machine_period; // the first call runs a frame
			return Pulldown(0.0);
		}

		const double dt = now_s - m_last;
		m_last = now_s;
		if(dt <= 0.0)
			return 0;
		if(dt > kStallS)
		{
			// A gap this long says nothing about the display, and the time
			// in it must not be paid back as a burst of frames.
			++m_stalls;
			m_samples = 0;
			m_bad_run = 0;
			m_period = 0;
			m_mad = 0;
			m_acc = m_machine_period;
			m_phase = 0;
			if(m_mode == Mode::Locked)
				SetPulldown();
			return 1;
		}

		Observe(dt);
		Decide();

		if(m_mode == Mode::Locked)
			return Locked(dt);
		return Pulldown(dt);
	}

	Mode GetMode() const { return m_mode; }
	int LockN() const { return m_mode == Mode::Locked ? m_n : 0; }
	bool DisplayKnown() const { return m_samples >= kWarmup && m_period > 0.0; }
	double DisplayHz() const { return m_period > 0.0 ? 1.0 / m_period : 0.0; } // as measured
	double JitterRatio() const { return m_period > 0.0 ? m_mad / m_period : 0.0; }
	// The rate the machine is really being run at.
	double EffectiveMachineHz() const { return m_mode == Mode::Locked ? DisplayHz() / m_n : m_machine_hz; }
	unsigned Stalls() const { return m_stalls; }
	unsigned ModeChanges() const { return m_mode_changes; }

private:
	void DropState(double now_s)
	{
		m_started = true;
		m_last = now_s;
		m_acc = m_machine_period;
		m_phase = 0;
		if(m_mode == Mode::Locked)
			SetPulldown();
		m_samples = 0;
		m_bad_run = 0;
		m_period = 0;
		m_mad = 0;
	}

	// Smoothed present interval and its mean absolute deviation.
	void Observe(double dt)
	{
		if(m_samples == 0 || m_period <= 0.0)
		{
			m_period = dt;
			m_mad = 0;
			m_samples = 1;
			m_bad_run = 0;
			return;
		}
		if(dt > 1.5 * m_period || dt < 0.5 * m_period)
		{
			// A missed or doubled present: not the display's period. If the
			// "outliers" keep coming, the period itself has changed.
			if(++m_bad_run >= 20)
			{
				m_period = dt;
				m_mad = 0;
				m_samples = 1;
				m_bad_run = 0;
			}
			return;
		}
		m_bad_run = 0;
		const double a = m_samples < 16 ? 1.0 / (m_samples + 1) : 1.0 / 64.0;
		m_period += a * (dt - m_period);
		m_mad += a * (std::fabs(dt - m_period) - m_mad);
		if(m_samples < 1 << 20)
			++m_samples;
	}

	void SetPulldown()
	{
		if(m_mode != Mode::Pulldown)
			++m_mode_changes;
		m_mode = Mode::Pulldown;
		m_acc = m_machine_period;
	}

	void Decide()
	{
		if(!DisplayKnown())
		{
			if(m_mode == Mode::Locked)
				SetPulldown();
			return;
		}
		const double hz = 1.0 / m_period;
		int n = (int)std::floor(hz / m_machine_hz + 0.5);
		if(n < 1)
			n = 1;
		const double dev = hz / (n * m_machine_hz) - 1.0;
		const double jitter = m_mad / m_period;

		if(m_mode == Mode::Pulldown)
		{
			if(n <= kMaxLockN && std::fabs(dev) <= kLockEnter && jitter <= kJitterEnter)
			{
				m_mode = Mode::Locked;
				++m_mode_changes;
				m_n = n;
				m_phase = n - 1; // the next present runs a frame
			}
		}
		else if(n != m_n || std::fabs(dev) > kLockExit || jitter > kJitterExit)
		{
			SetPulldown();
		}
	}

	int Pulldown(double dt)
	{
		m_acc += dt;
		int steps = (int)(m_acc / m_machine_period);
		m_acc -= steps * m_machine_period;
		if(steps > kMaxSteps)
		{
			steps = kMaxSteps;
			if(m_acc > m_machine_period)
				m_acc = m_machine_period; // what couldn't be run is dropped, not owed
		}
		return steps;
	}

	int Locked(double dt)
	{
		// One count per vsync that has passed, so a missed present is
		// caught up rather than silently stretching the machine's time.
		int k = (int)std::floor(dt / m_period + 0.5);
		if(k < 1)
			k = 1;
		if(k > 2 * m_n)
			k = 2 * m_n;
		const int total = m_phase + k;
		int steps = total / m_n;
		m_phase = total % m_n;
		if(steps > kMaxSteps)
			steps = kMaxSteps;
		return steps;
	}

	double m_machine_hz = 50.0, m_machine_period = 0.02;
	bool m_started = false;
	double m_last = 0.0;
	double m_acc = 0.0;
	double m_period = 0.0, m_mad = 0.0;
	int m_samples = 0, m_bad_run = 0;
	Mode m_mode = Mode::Pulldown;
	int m_n = 1, m_phase = 0;
	unsigned m_stalls = 0, m_mode_changes = 0;
};

}//namespace xPacing
}//namespace xPlatform

#endif//__SDL2_DESKTOP_FRAME_SCHED_H__
