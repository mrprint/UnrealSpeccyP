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

#ifndef __SDL2_DESKTOP_PACING_H__
#define __SDL2_DESKTOP_PACING_H__

#pragma once

// =============================================================================
//  platform/sdl2_desktop/sdl2_desktop_pacing.h
//
//  Shared plumbing of the pacing design: the emulated machine is the master
//  clock (sdl2_desktop_frame_sched.h decides how many of its frames each
//  present runs, sdl2_desktop_audio_sync.h slaves the audio device to it).
//
//  The frame scheduler (Loop1() in sdl2_desktop.cpp) paces by the wall-clock
//  time of the *previous* present - the moment SDL_GL_SwapWindow() returned -
//  rather than by the time it is called: the latter carries event-handling /
//  ImGui / audio-lock overhead, which would smear the measured display
//  interval. OnPresent() records that moment, right after the swap;
//  LastPresentTime() hands it out.
// =============================================================================

namespace xPlatform
{
namespace xPacing
{

double NowSeconds();                       // monotonic
// The wall-clock time of the most recent present (right after SwapWindow
// returned), or 0 if no present has happened yet. This is the clean signal
// for the frame scheduler: unlike NowSeconds() called elsewhere in the main
// loop it carries no event-handling / ImGui / audio-lock overhead.
double LastPresentTime();
// Once per presented frame, right after SDL_GL_SwapWindow() returned:
// record the wall-clock time of the present.
void OnPresent(double now_s);

// ---- Waiting for vsync asleep (sdl2_desktop_frame_delay.h) ------------------
// Keeps the main thread asleep, instead of spinning in the GL driver, while it
// waits for vsync. Windows: the OS's own vblank wait (D3DKMT, else DwmFlush);
// elsewhere, or when that does not work out: a sleep up to shortly before the
// next vblank is due. Hooks, in the order they run each frame:
//   FrameDelayBegin()  top of Loop1(): waits / sleeps. 'flat_out' = fast
//                      forward / fast tape loading: no waiting at all.
//   OnRenderBegin()    entry of UpdateScreen(): ends the frame's own CPU work.
//   OnPresent()        right after SwapWindow(), above.
// Environment:
//   UNREALSPECCYP_FRAME_DELAY=0      all of it off (the old behaviour)
//   UNREALSPECCYP_VBLANK=kmt|dwm|0   Windows: only that vblank source / none (sleep instead)
void FrameDelayBegin(bool flat_out);
void OnRenderBegin();

// The mixer was found empty and extra frames are owed to it: run one more
// than the schedule says for this present, then say so with AudioRefillDone()
// - but only if frames really were emulated (a paused machine produces none).
bool AudioRefillWanted();                // sdl2_audio.cpp
void AudioRefillDone();                  // sdl2_audio.cpp
// The emulated field rate changed (a new emulation speed): one frame is now
// sample_rate / hz samples, which the fill-level controller measures in.
void AudioSetMachineHz(double hz);       // sdl2_audio.cpp
double AudioSampleRate();                // sdl2_audio.cpp: what the device runs at, 44100 before it is opened

}//namespace xPacing
}//namespace xPlatform

#endif//__SDL2_DESKTOP_PACING_H__
