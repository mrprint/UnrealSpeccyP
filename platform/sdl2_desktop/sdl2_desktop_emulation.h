#ifndef __SDL2_DESKTOP_EMULATION_H__
#define __SDL2_DESKTOP_EMULATION_H__

#pragma once

// =============================================================================
//  Emulation speed presets (sdl2_desktop).
//
//  A preset is a pair of numbers taken from a known computer: the CPU clock and
//  the video frame timing. It sets the emulation *speed* and nothing else - the
//  ROMs, the memory model (Device -> Mode 48k), memory contention and the rest
//  of the hardware behaviour are unchanged, so this is not an emulation of the
//  selected computer. The texts shown to the person say so.
//
//  The CPU clock and the frame timing are separate fields on purpose: the
//  emulated screen refresh rate is just clock / frame length, so a preset can
//  differ from another in either (Pentagon 48K and ZX Spectrum 48K share the
//  clock, ZX Spectrum 128K and Pentagon 128K share neither).
//
//  Figures: SpeccyWiki, "256x192" (Timings): clock, T-states per line, lines
//  per frame, lines before the raster. The first entry is not a computer: it is
//  the speed this emulator has always run at (see eSpeccy::eSpeccy), kept as the
//  default so that nothing changes for anybody who does not pick a preset.
// =============================================================================

#include "../platform.h"

namespace xPlatform
{

struct eEmulationPreset
{
	const char* id;			// what the config stores; never translated, never renamed
	const char* name;		// proper name, shown as is; NULL: look up name_key
	const char* name_key;	// translation key of the name when it is not a proper name
	dword cpu_clock_hz;
	int line_tacts;			// T-states per line
	int lines_per_frame;
	int lines_before_raster;

	int FrameTacts() const { return line_tacts * lines_per_frame; }
	double FieldRateHz() const { return (double)cpu_clock_hz / FrameTacts(); }
	// Where the first paper pixel is drawn, in T-states from the interrupt. The
	// 69 is the position inside the line that the Pentagon timing (80 lines,
	// 17989) has always used here; it is applied to every preset.
	int PaperStart() const { return lines_before_raster * line_tacts + 69; }
};

extern const char* const DEFAULT_EMULATION_PRESET_ID;

int EmulationPresetCount();
const eEmulationPreset& EmulationPreset(int index);

// The preset the config / the Options dialog selects. An id nobody knows
// (a config from a newer version, say) is the default entry.
int SelectedEmulationPreset();
void SelectEmulationPreset(int index);

// What the emulated machine runs at right now (-1 before the first apply).
int RunningEmulationPreset();

// Puts the emulated machine on the selected preset. Call before every frame (it
// does nothing unless the selection changed), and only between frames.
// Changing the preset of a machine that is
// already running resets it (Device -> Reset): a program cannot be expected to
// survive its own timing changing under it, and the state of the ULA and of the
// sound devices is tied to the timing too. While an RZX replay is playing the
// change waits until it is over (a replay is recorded against one timing).
// Returns true when the preset just changed - the caller then has to re-pace
// everything that is derived from the field rate (frame scheduler, audio sync,
// display mode).
// The emulation core is not touched for any of this: see the top of the .cpp.
bool ApplyEmulationPreset();

// The two steps of the above on a given machine, for ApplyEmulationPreset() and
// for the tests: the CPU and the ULA onto the preset's frame timing (between
// frames only) and the sound devices onto a CPU clock.
void EnforceEmulationTiming(eSpeccy* speccy, const eEmulationPreset& p);
void RetimeEmulationSound(eSpeccy* speccy, dword cpu_clock_hz, dword sample_rate);

// The audio device has just been opened (the handler timed the sound devices by
// the core's own CPU clock while doing it).
void OnEmulationAudioOpened();

// Display text for the Options dialog. `buf` receives the translated name.
const char* EmulationPresetName(const eEmulationPreset& p);
// Translated multi-line description with the concrete figures, for a tooltip.
void EmulationPresetDetails(const eEmulationPreset& p, char* buf, size_t buf_size);

// sdl2_desktop_video.cpp: the emulated field rate changed - fullscreen with
// "Native refresh" re-picks the display mode if a better one exists now.
void OnEmulatedFieldRateChanged();

}//namespace xPlatform

#endif//__SDL2_DESKTOP_EMULATION_H__
