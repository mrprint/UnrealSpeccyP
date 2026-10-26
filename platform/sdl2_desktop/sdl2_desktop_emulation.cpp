#include "../platform.h"

#ifdef USE_SDL2_DESKTOP

#include <cstdio>
#include <cstring>
#include <string>

#include "sdl2_desktop_emulation.h"
#include "imgui_shared.h"
#include "../../tools/options.h"
#include "../../speccy.h"
#include "../../z80/z80.h"
#include "../../devices/ula.h"
#include "../../devices/sound/device_sound.h"
#include "../../devices/sound/ay.h"
#include "../../devices/sound/beeper.h"
#include "../../devices/input/tape.h"
#include "sdl2_desktop_pacing.h"
#include "sdl2_desktop_fullborder.h"

namespace xPlatform
{

const char* const DEFAULT_EMULATION_PRESET_ID = "default";

namespace
{

const eEmulationPreset presets[] =
{
	// The emulator's historical speed: a Pentagon-sized frame at exactly 50 Hz.
	{ DEFAULT_EMULATION_PRESET_ID, NULL, "options.emulation.preset.default", SNDR_DEFAULT_SYSTICK_RATE, 224, 320, 80 },
	{ "zx48",		"ZX Spectrum 48K",	NULL, 3500000, 224, 312, 64 },
	{ "zx128",		"ZX Spectrum 128K",	NULL, 3546900, 228, 311, 63 },
	{ "pentagon48",	"Pentagon 48K",		NULL, 3500000, 224, 320, 80 },
	{ "pentagon128",	"Pentagon 128K",	NULL, 3575000, 224, 320, 80 },
};
const int preset_count = (int)(sizeof(presets) / sizeof(presets[0]));

int running_preset = -1;
// The CPU clock the sound devices are timed by now. What the handler gives them
// when the audio device is opened is the core's own clock (see OnEmulationAudioOpened).
dword sound_clock = SNDR_DEFAULT_SYSTICK_RATE;

// ---------------------------------------------------------------------------
// The emulation core hard-codes one timing - the CPU clock, the frame and line
// length, the paper start (a 71680-tact pentagon frame; speccy.cpp, ula.cpp,
// device_sound.h) - and has no way to change it. The platform changes it from
// outside, and none of the core is modified. Everything it needs is protected:
// naming a protected member through a class derived from its owner is the
// standard way to get a pointer to it, with no cast and nothing instantiated.
// ---------------------------------------------------------------------------
struct eZ80Access : public xZ80::eZ80
{
	static int xZ80::eZ80::* FrameTactsMember() { return &eZ80Access::frame_tacts; }
};
struct eUlaAccess : public eUla
{
	static int eUla::* LineTactsMember() { return &eUlaAccess::line_tacts; }
	static int eUla::* PaperStartMember() { return &eUlaAccess::paper_start; }
	static void (eUla::* CreateTimingsMember())() { return &eUlaAccess::CreateTimings; }
};

// Puts the CPU and the ULA on the preset's frame timing. Idempotent. Only between
// frames: the ULA has no raster state to lose there, and eZ80::Update() carries
// its tact counter over with t -= frame_tacts, whatever the frame length. The core
// leaves it where it is afterwards - a machine reset, a snapshot load and a change
// of the 48k mode do not touch it (eUla::Init(), which sets the pentagon values,
// runs once, when the device is added).
}//namespace

void EnforceEmulationTiming(eSpeccy* speccy, const eEmulationPreset& p)
{
	xZ80::eZ80* cpu = speccy->CPU();
	if(cpu->*eZ80Access::FrameTactsMember() != p.FrameTacts())
		cpu->*eZ80Access::FrameTactsMember() = p.FrameTacts();
	eUla* ula = speccy->Device<eUla>();
	if(ula->*eUlaAccess::LineTactsMember() != p.line_tacts || ula->*eUlaAccess::PaperStartMember() != p.PaperStart())
	{
		ula->*eUlaAccess::LineTactsMember() = p.line_tacts;
		ula->*eUlaAccess::PaperStartMember() = p.PaperStart();
		(ula->*eUlaAccess::CreateTimingsMember())();
		InvalidateFullBorder(); // the table of the extended raster, if it is on, was just replaced
	}
}

// The sound devices turn T-states into samples with the CPU clock, so they follow
// it (the AY chip's own clock does not: it is its crystal). SetTimings() drops
// what a device has in its buffer; the platform has taken it by the time it
// changes the speed between frames.
void RetimeEmulationSound(eSpeccy* speccy, dword cpu_clock_hz, dword sample_rate)
{
	speccy->Device<eAY>()->SetTimings(cpu_clock_hz, SNDR_DEFAULT_AY_RATE, sample_rate);
	speccy->Device<eBeeper>()->SetTimings(cpu_clock_hz, sample_rate);
	speccy->Device<eTape>()->SetTimings(cpu_clock_hz, sample_rate);
}

namespace
{
void RetimeSound(dword cpu_clock_hz)
{
	RetimeEmulationSound(Handler()->Speccy(), cpu_clock_hz, (dword)(xPacing::AudioSampleRate() + 0.5));
	sound_clock = cpu_clock_hz;
}

struct eOptionEmulationPreset : public xOptions::eOptionString
{
	eOptionEmulationPreset() { Value(DEFAULT_EMULATION_PRESET_ID); }
	const char* Name() const override { return "emulation preset"; }
	int Order() const override { return 66; }
} op_emulation_preset;

// 3584000 -> "3.584", 3500000 -> "3.5", 3546900 -> "3.5469"
std::string MHz(dword hz)
{
	char b[32];
	snprintf(b, sizeof(b), "%.4f", hz / 1e6);
	std::string s = b;
	while(!s.empty() && s.back() == '0')
		s.pop_back();
	if(!s.empty() && s.back() == '.')
		s.pop_back();
	return s;
}

}//namespace

int EmulationPresetCount() { return preset_count; }

const eEmulationPreset& EmulationPreset(int index)
{
	return presets[(index >= 0 && index < preset_count) ? index : 0];
}

int SelectedEmulationPreset()
{
	const char* id = op_emulation_preset.Value();
	for(int i = 0; i < preset_count; ++i)
		if(!strcmp(presets[i].id, id))
			return i;
	return 0;
}

void SelectEmulationPreset(int index)
{
	op_emulation_preset.Value(EmulationPreset(index).id);
}

int RunningEmulationPreset() { return running_preset; }

bool ApplyEmulationPreset()
{
	const int want = SelectedEmulationPreset();
	bool changed = false, reset = false;
	if(want != running_preset)
	{
		const bool first = running_preset < 0;
		dword frame = 0, total = 0, cached = 0;
		// a replay is recorded against one timing: wait for its end
		if(first || !Handler()->GetReplayProgress(&frame, &total, &cached))
		{
			const eEmulationPreset& p = presets[want];
			if(p.cpu_clock_hz != sound_clock)
				RetimeSound(p.cpu_clock_hz);
			running_preset = want;
			changed = true;
			reset = !first;
		}
	}
	if(!changed)
		return false;
	const eEmulationPreset& p = presets[running_preset];
	EnforceEmulationTiming(Handler()->Speccy(), p);
	if(reset)
	{
		// Whatever is running was written for the old timing: a program that is in
		// step with the frame (a demo) or waits for an interrupt that no longer comes
		// where it expects it, the state of the ULA and the sound devices, the tact
		// counter. Changing the timing under it is not something a real computer does
		// and a program may hang (frozen picture, a held tone). So the machine is
		// restarted - the same reset as Device -> Reset - and starts clean on the
		// new timing. Not at the first apply: the machine is fresh then.
		Handler()->OnAction(A_RESET);
		char msg[160];
		snprintf(msg, sizeof(msg), xImGui::Tr("status.emulation_preset"), EmulationPresetName(p), p.FieldRateHz());
		xImGui::SetStatusText(msg);
	}
	return changed;
}

void OnEmulationAudioOpened()
{
	// Opening the audio device made the handler time the sound devices by the core's
	// own CPU clock; put them back on the preset's.
	sound_clock = SNDR_DEFAULT_SYSTICK_RATE;
	if(running_preset >= 0 && presets[running_preset].cpu_clock_hz != sound_clock)
		RetimeSound(presets[running_preset].cpu_clock_hz);
}

const char* EmulationPresetName(const eEmulationPreset& p)
{
	return p.name ? p.name : xImGui::Tr(p.name_key);
}

void EmulationPresetDetails(const eEmulationPreset& p, char* buf, size_t buf_size)
{
	int n = snprintf(buf, buf_size, "%s\n", EmulationPresetName(p));
	if(n < 0 || (size_t)n >= buf_size)
		return;
	n += snprintf(buf + n, buf_size - n, xImGui::Tr("options.emulation.preset_values"),
		MHz(p.cpu_clock_hz).c_str(), p.FrameTacts(), p.lines_per_frame, p.line_tacts, p.FieldRateHz());
	if(p.name == NULL && n > 0 && (size_t)n < buf_size)
		snprintf(buf + n, buf_size - n, "\n%s", xImGui::Tr("options.emulation.preset.default_desc"));
}

}//namespace xPlatform

#endif//USE_SDL2_DESKTOP
