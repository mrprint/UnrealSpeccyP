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

#include "../platform.h"

#ifdef USE_SDL2_DESKTOP

#include <SDL.h>
#ifdef _WINAPI
#include <windows.h>
#include <SDL_syswm.h>
#endif//_WINAPI

#include "sdl2_desktop_pacing.h"
#include "sdl2_desktop_frame_delay.h"

namespace xPlatform
{
int DisplayNominalHz();     // sdl2_desktop_video.cpp
SDL_Window* GetVideoWindow(); // sdl2_desktop_video.cpp

namespace xPacing
{

namespace
{
// The wall-clock time of the last present (right after SwapWindow returned).
double g_last_present_time = 0;

// ---------------------------------------------------------------------------
//  Waiting for vsync asleep. See sdl2_desktop_frame_delay.h for why.
//
//  Best: ask the OS for the vblank itself - the thread then sleeps in the
//  kernel until the display is about to scan out, no prediction, no spinning
//  driver. Windows only (below); everywhere else the GL driver's own wait is
//  already a sleep (Mesa, macOS) or is made one (__GL_YIELD, InitVideo()), and
//  what remains is the predicted sleep of FrameDelayController.
//
//  The sources are tried in order and given up on, one by one, as soon as they
//  misbehave (fail, return at once, or the presents start missing vblanks).
// ---------------------------------------------------------------------------
#ifdef _WINAPI
// d3dkmthk.h is in the WDK only; the three calls used here are in gdi32.dll
// and the layouts of their arguments are the documented, stable ones.
struct KmtOpenAdapterFromHdc { HDC hDc; UINT hAdapter; LUID AdapterLuid; UINT VidPnSourceId; };
struct KmtWaitForVerticalBlank { UINT hAdapter; UINT hDevice; UINT VidPnSourceId; };
struct KmtCloseAdapter { UINT hAdapter; };

class VblankWaiter
{
public:
	enum Kind { kNone, kKmt, kDwm };

	~VblankWaiter() { CloseAdapter(); }

	Kind GetKind() const { return m_kind; }
	bool Active() const { return m_kind != kNone; }
	const char* Name() const { return m_kind == kKmt ? "D3DKMT" : m_kind == kDwm ? "DwmFlush" : "sleep"; }

	// The window was never looked at, or it is on another monitor now.
	bool NeedsBind(SDL_Window* w) const
	{
		if(!m_bound)
			return true;
		HWND hwnd = Hwnd(w);
		return hwnd && MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != m_monitor;
	}

	// (Re)selects the best source for the monitor the window is on.
	void Bind(SDL_Window* w)
	{
		CloseAdapter();
		m_bound = true;
		m_kind = kNone;
		const Policy p = GetPolicy();
		if(p == kOff)
			return;
		const Fns& f = Load();
		HWND hwnd = Hwnd(w);
		m_monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
		if(hwnd && p != kDwmOnly && f.open && f.wait && f.close)
		{
			MONITORINFOEXW mi = {};
			mi.cbSize = sizeof(mi);
			if(GetMonitorInfoW(m_monitor, &mi))
			{
				if(HDC dc = CreateDCW(mi.szDevice, nullptr, nullptr, nullptr))
				{
					KmtOpenAdapterFromHdc o = {};
					o.hDc = dc;
					const LONG status = f.open(&o);
					DeleteDC(dc);
					if(status == 0)
					{
						m_adapter = o.hAdapter;
						m_source = o.VidPnSourceId;
						m_kind = kKmt;
						return;
					}
				}
			}
		}
		if(p != kKmtOnly && f.dwm_flush)
			m_kind = kDwm;
	}

	// Blocks, asleep, until the next vblank. false: the call failed.
	bool Wait()
	{
		const Fns& f = Load();
		if(m_kind == kKmt)
		{
			KmtWaitForVerticalBlank wv = { m_adapter, 0, m_source };
			return f.wait(&wv) == 0;
		}
		if(m_kind == kDwm)
			return SUCCEEDED(f.dwm_flush());
		return false;
	}

	// This source is no good: the next one, or none.
	void Downgrade()
	{
		const Fns& f = Load();
		if(m_kind == kKmt)
		{
			CloseAdapter();
			m_kind = (GetPolicy() != kKmtOnly && f.dwm_flush) ? kDwm : kNone;
		}
		else
			m_kind = kNone;
	}

private:
	enum Policy { kAuto, kKmtOnly, kDwmOnly, kOff };
	typedef LONG (WINAPI *OpenFn)(KmtOpenAdapterFromHdc*);
	typedef LONG (WINAPI *WaitFn)(const KmtWaitForVerticalBlank*);
	typedef LONG (WINAPI *CloseFn)(const KmtCloseAdapter*);
	typedef HRESULT (WINAPI *DwmFlushFn)();
	struct Fns { OpenFn open; WaitFn wait; CloseFn close; DwmFlushFn dwm_flush; };

	// UNREALSPECCYP_VBLANK: unset = auto (D3DKMT, then DwmFlush); kmt / dwm = that one only; 0 = neither.
	static Policy GetPolicy()
	{
		static const Policy policy = []
		{
			const char* v = SDL_getenv("UNREALSPECCYP_VBLANK");
			if(!v || !v[0]) return kAuto;
			if(!SDL_strcasecmp(v, "kmt")) return kKmtOnly;
			if(!SDL_strcasecmp(v, "dwm")) return kDwmOnly;
			if(!SDL_strcmp(v, "0") || !SDL_strcasecmp(v, "off")) return kOff;
			return kAuto;
		}();
		return policy;
	}
	static const Fns& Load()
	{
		static const Fns fns = []
		{
			Fns r = {};
			if(HMODULE gdi = LoadLibraryW(L"gdi32.dll"))
			{
				r.open = (OpenFn)GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc");
				r.wait = (WaitFn)GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent");
				r.close = (CloseFn)GetProcAddress(gdi, "D3DKMTCloseAdapter");
			}
			if(HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
				r.dwm_flush = (DwmFlushFn)GetProcAddress(dwm, "DwmFlush");
			return r;
		}();
		return fns;
	}
	static HWND Hwnd(SDL_Window* w)
	{
		SDL_SysWMinfo info;
		SDL_VERSION(&info.version);
		if(w && SDL_GetWindowWMInfo(w, &info) && info.subsystem == SDL_SYSWM_WINDOWS)
			return info.info.win.window;
		return nullptr;
	}
	void CloseAdapter()
	{
		if(m_adapter)
		{
			const Fns& f = Load();
			KmtCloseAdapter c = { m_adapter };
			if(f.close)
				f.close(&c);
			m_adapter = 0;
		}
	}

	bool     m_bound = false;
	Kind     m_kind = kNone;
	HMONITOR m_monitor = nullptr;
	UINT     m_adapter = 0;
	UINT     m_source = 0;
};
#else//_WINAPI
class VblankWaiter
{
public:
	enum Kind { kNone };
	Kind GetKind() const { return kNone; }
	bool Active() const { return false; }
	const char* Name() const { return "sleep"; }
	bool NeedsBind(SDL_Window*) const { return false; }
	void Bind(SDL_Window*) {}
	bool Wait() { return false; }
	void Downgrade() {}
};
#endif//_WINAPI

VblankWaiter g_vblank;
FrameDelayController g_frame_delay;
int      g_hz_poll = 0;
int      g_vblank_bad = 0;       // consecutive waits that were no waits
unsigned g_vblank_hist = 0;      // last 32 presents, 1 = a vblank missed
int      g_hold = 0;             // frames left in which the source is not judged (right after a stall / mode change)
int      g_last_hz = 0;
bool     g_downgraded = false;   // running on a worse source than Bind() would pick
double   g_retry_at = 0;         // when to give the better source another chance
double   g_backoff = 20.0;       // seconds until that; doubles with every failure, so a source that really is bad costs next to nothing
double   g_bind_time = 0;
const double kBackoffMin = 20.0, kBackoffMax = 640.0;

// This frame's timing.
double g_t_start = 0;   // the wait / sleep is over, the frame's own work begins
double g_t_render = 0;  // UpdateScreen() entered
double g_frame_work = 0;

bool FrameDelayAllowed()
{
	static const bool allowed = []
	{
		const char* v = SDL_getenv("UNREALSPECCYP_FRAME_DELAY");
		return !(v && v[0] == '0');
	}();
	return allowed;
}

int Popcount(unsigned v) { int n = 0; while(v) { v &= v - 1; ++n; } return n; }

// Sleeps until 'target_s' (NowSeconds() scale), leaving the last ~1 ms to the
// driver: SDL_Delay() has a whole-millisecond granularity and a timer that
// wakes late would cost a vblank, while an early wake costs only a little
// waiting in the driver. The overshoot is reported so the controller can keep
// a reserve for it.
void SleepUntil(double target_s)
{
	double now = NowSeconds();
	bool slept = false;
	while(target_s - now > 0.0015)
	{
		const int ms = (int)((target_s - now - 0.0005) * 1000.0);
		SDL_Delay(ms > 1 ? (Uint32)ms : 1);
		now = NowSeconds();
		slept = true;
	}
	if(slept)
		g_frame_delay.OnOvershoot(now - target_s);
}

// Gives up the vblank source for the next one (or for the sleep) and schedules
// another try of the better one later.
void DowngradeVblank(const char* why)
{
	const char* was = g_vblank.Name();
	g_vblank.Downgrade();
	g_vblank_bad = 0;
	g_vblank_hist = 0;
	g_downgraded = true;
	g_retry_at = NowSeconds() + g_backoff;
	SDL_Log("frame-sync: %s %s, now: %s (will retry in %.0f s)", was, why, g_vblank.Name(), g_backoff);
	g_backoff = g_backoff * 2.0 < kBackoffMax ? g_backoff * 2.0 : kBackoffMax;
}

// One wait for the vblank. A wait that fails, or that returns long before a
// vblank can be due, is no wait at all; a few of those in a row and the source
// is dropped for the next one.
void WaitVblank()
{
	const bool ok = g_vblank.Wait();
	const double now = NowSeconds();
	const double since = g_last_present_time > 0.0 ? now - g_last_present_time : 1.0;
	if(ok && since >= 0.4 * g_frame_delay.Period())
	{
		g_vblank_bad = 0;
		return;
	}
	if(++g_vblank_bad >= 5)
		DowngradeVblank(ok ? "returns at once instead of waiting" : "fails");
}

// Presents that keep missing vblanks while a vblank source paces the loop: the
// source (or its interplay with the swap) is not working here.
void CheckVblankHealth(double dt)
{
	if(g_frame_delay.IsStall(dt))
	{
		// A mode switch, a drag, a dialog: not the source's doing. Do not judge
		// it for a while, and look the display up again at once (the refresh rate
		// may have changed, and the period measured so far with it).
		g_vblank_hist = 0;
		g_hold = 60;
		g_hz_poll = 0;
		return;
	}
	if(dt > 1.2 * g_frame_delay.Period())
		g_hz_poll = 0; // irregular: is the display still what it was?
	if(g_hold > 0)
	{
		--g_hold;
		return;
	}
	if(!g_vblank.Active())
		return;
	g_vblank_hist = (g_vblank_hist << 1) | (dt > FrameDelayController::kMissRatio * g_frame_delay.Period() ? 1u : 0u);
	if(Popcount(g_vblank_hist) >= FrameDelayController::kPanicMisses)
		DowngradeVblank("keeps missing vblanks");
}

}

double NowSeconds()
{
	static const double freq = (double)SDL_GetPerformanceFrequency();
	return (double)SDL_GetPerformanceCounter() / freq;
}

double LastPresentTime() { return g_last_present_time; }

void OnPresent(double now_s)
{
	const double prev = g_last_present_time;
	g_last_present_time = now_s;
	g_frame_delay.OnFrame(g_frame_work, now_s);
	if(prev > 0.0)
	{
		CheckVblankHealth(now_s - prev);
	}
}

void FrameDelayBegin(bool flat_out)
{
	const double now = NowSeconds();
	// The display is looked up now and then, not every frame: it only
	// changes when the window moves to another display or a mode is applied.
	if((g_hz_poll++ & 31) == 0)
	{
		const int hz = DisplayNominalHz();
		g_frame_delay.SetNominalHz(hz);
		const bool hz_changed = hz != g_last_hz;
		g_last_hz = hz;
		if(hz_changed || g_vblank.NeedsBind(GetVideoWindow()))
		{
			// A new display or mode: whatever was learnt about the old one is void,
			// the best source is the one to start with again.
			g_vblank.Bind(GetVideoWindow());
			g_vblank_bad = 0;
			g_vblank_hist = 0;
			g_downgraded = false;
			g_backoff = kBackoffMin;
			g_bind_time = now;
			if(hz_changed)
				g_hold = 60;
		}
		else if(g_downgraded && g_hold == 0 && now >= g_retry_at)
		{
			g_vblank.Bind(GetVideoWindow());
			g_vblank_bad = 0;
			g_vblank_hist = 0;
			g_downgraded = false;
			g_bind_time = now;
		}
		else if(!g_downgraded && now - g_bind_time > 120.0)
			g_backoff = kBackoffMin; // has been fine for a while: the next failure is a new one
	}
	const bool on = FrameDelayAllowed() && !flat_out;
	g_frame_delay.SetEnabled(on);

	if(on)
	{
		if(g_vblank.Active())
		{
			WaitVblank();
		}
		else
		{
			const double d = g_frame_delay.Delay();
			// Absolute, from the previous present: everything done since it returned
			// (audio, the end of the loop iteration) is part of the delay, not on top.
			if(d > 0.0 && g_last_present_time > 0.0)
				SleepUntil(g_last_present_time + d);
		}
	}
	g_t_start = NowSeconds();
}

void OnRenderBegin()
{
	g_t_render = NowSeconds();
	g_frame_work = g_t_render - g_t_start;
}

}//namespace xPacing
}//namespace xPlatform

#endif//USE_SDL2_DESKTOP
