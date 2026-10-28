#ifndef __SDL2_DESKTOP_FRAME_DELAY_H__
#define __SDL2_DESKTOP_FRAME_DELAY_H__

#pragma once

// =============================================================================
//  platform/sdl2_desktop/sdl2_desktop_frame_delay.h
//
//  Keeps the main thread from burning a CPU core while it waits for vsync.
//
//  The problem. With SDL_GL_SetSwapInterval(1) the driver does not make
//  SwapWindow() the only place that waits: it lets the CPU run ahead and
//  blocks at the first GL call of the next frame that needs the GPU to have
//  caught up (typically the first draw / glTexSubImage2D into a buffer that is
//  not free yet). Several drivers (notably NVIDIA on Windows) wait there by
//  *spinning*, so the profiler - and Task Manager - show a full core busy
//  inside glTexSubImage2D although the upload itself costs ~0.1 ms.
//
//  The fix is platform independent and needs no driver cooperation: do the
//  waiting ourselves, asleep. Right after a present returns (which is aligned
//  with a vblank, because the driver blocked until the buffer was free) the
//  thread sleeps for D, then does the frame's work and presents. The
//  driver is left with only a small reserve to wait out:
//
//      D = T - W - reserve
//
//      T        the display's refresh period (measured; seeded by the nominal one)
//      W        the frame's own CPU work before it starts to render
//                 (events, emulation, UI) - a running average
//      reserve  everything else (GL submission, the sleep's wake-up jitter,
//                 spikes in W) - adapts: grows on a missed vblank, decays when calm
//
//  Because the call still blocks for the reserve, the presents stay aligned
//  with vblank - which is what FrameScheduler (sdl2_desktop_frame_sched.h)
//  measures the display from. It also samples input later in the period, so
//  latency drops by about D.
//
//  Self-protecting. The delay is dropped (D = 0, the old behaviour) when
//    - the loop is not vsync-bound (minimized window, driver ignoring the
//      swap interval, variable refresh): intervals shorter than the period;
//    - frames are being missed too often (slow machine, software GL);
//    - fast-forward / fast tape loading ask to run flat out.
//
//  Header-only and free of SDL/GL on purpose, so it can be driven by
//  synthetic timelines in a test. All times are in seconds.
// =============================================================================

#include <algorithm>
#include <cmath>

namespace xPlatform
{
namespace xPacing
{

class FrameDelayController
{
public:
	static constexpr double kReserveInit  = 0.0030; // start cautious...
	static constexpr double kReserveMin   = 0.0013; // ...and settle toward this
	static constexpr double kReserveStep  = 0.0010; // added per missed vblank
	static constexpr double kReserveDecay = 0.00002;// removed (plus 5 % of the excess) per kCalmFrames without a miss
	static constexpr int    kCalmFrames   = 300;    // ~5 s at 60 Hz
	static constexpr double kMaxDelayFrac = 0.85;   // never sleep more than this part of a period
	static constexpr double kRampStep     = 0.00025;// D may grow by this per frame (shrinks at once)
	static constexpr double kStallS       = 0.25;   // a longer gap is a stall (modeset, dialog), not an interval
	static constexpr double kStallPeriods = 4.0;    // ...and so is one this many display periods long: a missed vblank
	                                                // caused by a too small reserve costs 1-2 periods, not 4+
	static constexpr double kMissRatio    = 1.5;    // interval / period above which a vblank was missed
	static constexpr double kUnboundRatio = 0.93;   // interval / period below which vsync is not pacing us
	static constexpr int    kUnboundRun   = 8;      // ...for this many frames in a row
	static constexpr int    kRebindRun    = 30;     // in-range frames in a row to resume after that
	static constexpr int    kPanicMisses  = 6;      // misses among the last 32 frames that disable the delay...
	static constexpr int    kPanicHold    = 240;    // ...for this many frames
	static constexpr double kAvgAlpha     = 0.05;   // running averages of the frame's work and of the sleep overshoot

	// The display's nominal refresh rate (0 = unknown -> 60 Hz). A change
	// (another display, a new mode) restarts the measurement.
	void SetNominalHz(double hz)
	{
		if(!(hz > 20.0 && hz < 1000.0))
			hz = 60.0;
		if(std::fabs(hz - m_nominal_hz) < 0.5 && m_period > 0.0)
			return;
		const double keep_reserve = m_reserve;
		const bool keep_enabled = m_enabled;
		*this = FrameDelayController();
		m_reserve = keep_reserve;
		m_enabled = keep_enabled;
		m_nominal_hz = hz;
		m_period = 1.0 / hz;
	}

	// false while the loop deliberately runs flat out (fast forward, fast
	// tape loading): no delay, and nothing learned from such frames.
	void SetEnabled(bool on)
	{
		if(on == m_enabled)
			return;
		m_enabled = on;
		m_last_present = 0.0;
		m_delay = 0.0;
		m_target = 0.0;
	}

	// How long to sleep now, at the start of a frame, right after the
	// previous present returned.
	double Delay() const { return (m_enabled && m_active && m_panic == 0) ? m_delay : 0.0; }

	// The sleep that was just done overshot its target by this much (OS wake-up
	// jitter). Feeds the reserve so that a coarse timer does not cost vblanks.
	void OnOvershoot(double over_s)
	{
		if(over_s < 0.0)
			over_s = 0.0;
		m_over_avg += kAvgAlpha * (over_s - m_over_avg);
	}

	// A gap between presents that says nothing about the pacing: a mode switch,
	// a window drag, a dialog, a debugger. Neither a vblank source nor the reserve
	// is to be blamed for it.
	bool IsStall(double dt) const { return dt > kStallS || dt > kStallPeriods * m_period; }

	// Once per frame, right after the present returned.
	//   work_s     CPU time of this frame from its start (after the sleep) to
	//              the moment it began to render - it does NOT include the
	//              time spent in GL calls, where the driver may be waiting.
	//   present_s  the wall-clock time the present returned.
	void OnFrame(double work_s, double present_s)
	{
		if(!m_enabled || m_period <= 0.0)
			return;
		if(m_last_present <= 0.0)
		{
			m_last_present = present_s;
			return;
		}
		const double dt = present_s - m_last_present;
		m_last_present = present_s;
		if(dt <= 0.0)
			return;
		if(IsStall(dt))
		{
			// Says nothing about the display; do not learn from it, and do
			// not delay on top of whatever caused it for a moment.
			m_delay = 0.0;
			m_target = 0.0;
			m_hist = 0;
			return;
		}

		const bool in_range = dt >= 0.9 * m_period && dt <= 1.1 * m_period;
		if(in_range)
		{
			// Measured period, kept within 8 % of the nominal one.
			m_period += 0.02 * (dt - m_period);
			const double nominal = 1.0 / m_nominal_hz;
			m_period = (std::min)((std::max)(m_period, nominal * 0.92), nominal * 1.08);
		}
		const bool missed = dt > kMissRatio * m_period;
		m_hist = (m_hist << 1) | (missed ? 1u : 0u);

		// Is the loop paced by vsync at all?
		if(dt < kUnboundRatio * m_period)
		{
			m_in_range_run = 0;
			if(++m_unbound_run >= kUnboundRun)
				m_active = false;
		}
		else
		{
			m_unbound_run = 0;
			if(in_range && ++m_in_range_run >= kRebindRun)
				m_active = true;
		}

		m_work_avg += kAvgAlpha * (work_s - m_work_avg);

		if(m_panic > 0)
		{
			if(--m_panic == 0)
			{
				m_reserve = (std::min)(m_reserve + kReserveStep, MaxReserve());
				m_hist = 0;
			}
		}
		else if(missed)
		{
			m_reserve = (std::min)(m_reserve + kReserveStep, MaxReserve());
			m_calm = 0;
			if(Popcount(m_hist) >= kPanicMisses)
				m_panic = kPanicHold;
		}
		else if(++m_calm >= kCalmFrames)
		{
			m_calm = 0;
			// proportional: a reserve inflated by a bad patch (a drag, a stall) comes back
			// down in a minute or two, while one near its floor creeps down slowly
			m_reserve = (std::max)(m_reserve - kReserveDecay - 0.05 * (m_reserve - kReserveMin), kReserveMin);
		}

		// New target, approached slowly from below.
		double d = m_period - m_work_avg - m_over_avg - m_reserve;
		d = (std::min)(d, kMaxDelayFrac * m_period);
		m_target = d > 0.0 ? d : 0.0;
		if(m_target > m_delay)
			m_delay = (std::min)(m_target, m_delay + kRampStep);
		else
			m_delay = m_target;
	}

	double Period() const { return m_period; }
	double Reserve() const { return m_reserve; }
	double WorkAvg() const { return m_work_avg; }
	bool   Active() const { return m_enabled && m_active && m_panic == 0; }
	bool   Panicking() const { return m_panic > 0; }

private:
	double MaxReserve() const { return 0.5 * m_period; }
	static int Popcount(unsigned v) { int n = 0; while(v) { v &= v - 1; ++n; } return n; }

	bool     m_enabled = true;
	bool     m_active = true;        // vsync seems to pace the loop
	double   m_nominal_hz = 60.0;
	double   m_period = 1.0 / 60.0;  // measured display period
	double   m_last_present = 0.0;
	double   m_work_avg = 0.002;
	double   m_over_avg = 0.0;
	double   m_reserve = kReserveInit;
	double   m_delay = 0.0;          // what Delay() hands out (ramped)
	double   m_target = 0.0;
	unsigned m_hist = 0;             // last 32 frames, 1 = missed vblank
	int      m_calm = 0;
	int      m_panic = 0;
	int      m_unbound_run = 0;
	int      m_in_range_run = 0;
};

}//namespace xPacing
}//namespace xPlatform

#endif//__SDL2_DESKTOP_FRAME_DELAY_H__
