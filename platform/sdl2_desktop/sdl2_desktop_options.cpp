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

// =============================================================================
//  platform/sdl2_desktop/sdl2_desktop_options.cpp
//
//  Options dialog - a port of platform/wxwidgets/wx_optionsdialog.cpp's 5
//  tabs (Audio / Video / Input / Gamepads / Disk Drives) to this platform's
//  ImGui overlay. Tab list is a manual left-side selectable list rather than
//  ImGui's own top tab bar, on purpose: wx_optionsdialog.cpp deliberately
//  uses a left-side wxListbook (its own comments note this was chosen so
//  the tab captions read the same way across platforms), and that layout
//  choice carries over here for the same reason.
//
//  Same buffered-until-OK model as wx: LoadCurrentSettings()-equivalent
//  (OpenOptionsDialog()) snapshots every option into local static state when
//  the dialog opens; every widget below reads/writes that local snapshot,
//  not xOptions directly; OK commits it all via Set()+Apply(); Cancel
//  discards it. The per-tab "Restore Defaults" buttons only touch the local
//  snapshot too, exactly like OnResetAudio/Video/Input/Drive/Gamepad in wx.
//
//  Gamepads tab: same GUID-identified device combo + live capture-by-input
//  model as wx's CreateGamepadsPage()/StartCaptureMode()/OnTimer(), reusing
//  sdl2_desktop_gamepad.h (ported wx_gamepad.h/joystick_mapper.h) - see that
//  header for why it's a port rather than a shared #include. No separate
//  wxTimer is needed for either the 50ms capture poll or the device-hotplug
//  poll: this whole dialog already redraws every real frame (see
//  sdl2_desktop.cpp's single-threaded loop), so both are just checked once
//  per DrawOptionsDialog() call instead.
// =============================================================================

#include "../platform.h"

#ifdef USE_SDL2_DESKTOP

#include <SDL.h>
#include <cstring>
#include <cstdio>
#include "imgui.h"
#include "imgui_shared.h"
#include "sdl2_desktop_gamepad.h"
#include "../../tools/options.h"
#include "../../options_common.h"

namespace xPlatform
{
namespace xImGui
{

namespace {

// ---------------------------------------------------------------------------
// Buffered state - snapshotted from xOptions on open, written back on OK.
// ---------------------------------------------------------------------------

bool g_open = false;
int  g_active_tab = 0; // 0=Audio 1=Video 2=Input 3=Gamepads 4=Disk Drives

int  g_sound_chip = SC_AY;
int  g_ay_stereo = AS_ABC;

bool g_gigascreen = false;
bool g_scanlines = false;
bool g_pal_effects = true;
int  g_pal_strength = 50;
int  g_beam_spread = 30;
bool g_mipmapping = true;
int  g_mask_scale = 1;

int  g_joystick = J_KEMPSTON;

int  g_drive = D_A;

#ifdef SDL_USE_JOYSTICK
JoystickProfile g_gamepad_profiles[2];
std::vector<WxGamepadBackend::DeviceInfo> g_devices;
int g_capturing_player = -1;
EEmulatedJoystickInput g_capturing_input = EEmulatedJoystickInput::UP;
#endif//SDL_USE_JOYSTICK

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void LoadCurrentSettings()
{
	xOptions::eOption<int>* op;
	xOptions::eOption<bool>* opb;

	op = xOptions::eOption<int>::Find("sound chip");   if(op) g_sound_chip = *op;
	op = xOptions::eOption<int>::Find("ay stereo");    if(op) g_ay_stereo = *op;

	opb = xOptions::eOption<bool>::Find("gigascreen");   if(opb) g_gigascreen = *opb;
	opb = xOptions::eOption<bool>::Find("scanlines");    if(opb) g_scanlines = *opb;
	opb = xOptions::eOption<bool>::Find("pal effects");  if(opb) g_pal_effects = *opb;
	op = xOptions::eOption<int>::Find("pal strength");   if(op) g_pal_strength = *op;
	op = xOptions::eOption<int>::Find("beam spread");    if(op) g_beam_spread = *op;
	opb = xOptions::eOption<bool>::Find("mipmapping");   g_mipmapping = opb ? (bool)*opb : DEFAULT_MIPMAPPING;
	op = xOptions::eOption<int>::Find("mask scale");     g_mask_scale = op ? *op : DEFAULT_MASK_SCALE;

	g_drive = OpDrive();
	g_joystick = OpJoystick();
}

#ifdef SDL_USE_JOYSTICK
JoystickProfile LoadProfileFromOptions(int player_idx)
{
	JoystickProfile profile;
	DeserializeProfile(OpJoystickMappingData(player_idx), profile);
	int hinted_index = OpHostGamepadDevice(player_idx);
	std::string resolved_guid;
	profile.host_device_index = ResolveDeviceIndexForGuid(profile.device_guid, hinted_index, &resolved_guid);
	profile.device_guid = resolved_guid;
	return profile;
}

void SaveProfileToOptions(int player_idx, const JoystickProfile& profile)
{
	OpJoystickMappingData(player_idx, SerializeProfile(profile));
}

void RefreshDeviceList()
{
	g_devices = GamepadBackend().EnumerateDevices();
	for(int player = 0; player < 2; ++player)
	{
		const std::string& guid = g_gamepad_profiles[player].device_guid;
		int found_index = -1;
		if(!guid.empty())
		{
			for(const auto& dev : g_devices)
				if(dev.guid == guid) { found_index = dev.index; break; }
		}
		g_gamepad_profiles[player].host_device_index = found_index;
		if(found_index >= 0)
			GamepadBackend().RefreshDeviceState(found_index);
	}
}

const char* MappingLabelText(int player_idx, EEmulatedJoystickInput input)
{
	// If the assigned device is not connected, show a short placeholder
	// instead of stale mapping data that belongs to a different controller.
	if(g_gamepad_profiles[player_idx].host_device_index < 0)
		return "No device";

	auto it = g_gamepad_profiles[player_idx].input_map.find(input);
	if(it == g_gamepad_profiles[player_idx].input_map.end())
		return "Not set";
	return SourceTypeDisplayString(it->second.source_type);
}

void StopCapture()
{
	g_capturing_player = -1;
}

void StartCapture(int player_idx, EEmulatedJoystickInput input)
{
	g_capturing_player = player_idx;
	g_capturing_input = input;
}

// Checked once per frame while a capture is pending - equivalent of
// OptionsDialog::OnTimer(), just driven by the main loop's own frame rate
// instead of a dedicated 50ms wxTimer.
void PollCapture()
{
	if(g_capturing_player < 0)
		return;

	int assigned = g_gamepad_profiles[g_capturing_player].host_device_index;
	if(assigned < 0)
		return; // nothing plugged in for this player - nothing to capture from yet

	GamepadBackend().RefreshDeviceState(assigned);
	const GamepadState& s = GamepadBackend().GetState(assigned);

	struct Check { EHostSourceType type; bool active; };
	const Check checks[] = {
		{EHostSourceType::BUTTON_A, s.a}, {EHostSourceType::BUTTON_B, s.b},
		{EHostSourceType::BUTTON_X, s.x}, {EHostSourceType::BUTTON_Y, s.y},
		{EHostSourceType::BUTTON_BACK, s.back}, {EHostSourceType::BUTTON_START, s.start},
		{EHostSourceType::BUTTON_LEFTSTICK, s.leftstick}, {EHostSourceType::BUTTON_RIGHTSTICK, s.rightstick},
		{EHostSourceType::BUTTON_LEFTSHOULDER, s.leftshoulder}, {EHostSourceType::BUTTON_RIGHTSHOULDER, s.rightshoulder},
		{EHostSourceType::HAT_UP, s.IsHatUp()}, {EHostSourceType::HAT_DOWN, s.IsHatDown()},
		{EHostSourceType::HAT_LEFT, s.IsHatLeft()}, {EHostSourceType::HAT_RIGHT, s.IsHatRight()},
		{EHostSourceType::AXIS_LEFT_X_POS, s.GetAxisWithDeadzone(s.leftX) > 0.5f},
		{EHostSourceType::AXIS_LEFT_X_NEG, s.GetAxisWithDeadzone(s.leftX) < -0.5f},
		{EHostSourceType::AXIS_LEFT_Y_POS, s.GetAxisWithDeadzone(s.leftY) > 0.5f},
		{EHostSourceType::AXIS_LEFT_Y_NEG, s.GetAxisWithDeadzone(s.leftY) < -0.5f},
		{EHostSourceType::AXIS_RIGHT_X_POS, s.GetAxisWithDeadzone(s.rightX) > 0.5f},
		{EHostSourceType::AXIS_RIGHT_X_NEG, s.GetAxisWithDeadzone(s.rightX) < -0.5f},
		{EHostSourceType::AXIS_RIGHT_Y_POS, s.GetAxisWithDeadzone(s.rightY) > 0.5f},
		{EHostSourceType::AXIS_RIGHT_Y_NEG, s.GetAxisWithDeadzone(s.rightY) < -0.5f},
		{EHostSourceType::TRIGGER_LEFT, s.triggerLeft > 0.5f},
		{EHostSourceType::TRIGGER_RIGHT, s.triggerRight > 0.5f},
	};
	for(const auto& c : checks)
	{
		if(!c.active)
			continue;
		JoystickMappingEntry entry;
		entry.source_type = c.type;
		if(c.type >= EHostSourceType::AXIS_LEFT_X_POS && c.type <= EHostSourceType::TRIGGER_RIGHT)
			entry.threshold = 0.5f;
		g_gamepad_profiles[g_capturing_player].input_map[g_capturing_input] = entry;
		StopCapture();
		return;
	}
}
// True if the set of connected controllers changed since the last
// RefreshDeviceList() call (compares g_devices against a fresh
// enumeration). Used by PollDeviceChanges() to avoid rebuilding the
// comboboxes - and disturbing whatever the user is doing with them -
// every single frame when nothing actually changed.
bool DeviceListChanged()
{
    auto current = GamepadBackend().EnumerateDevices();
    if (current.size() != g_devices.size()) return true;
    for (size_t i = 0; i < current.size(); ++i) {
        if (current[i].index != g_devices[i].index || current[i].name != g_devices[i].name) {
            return true;
        }
    }
    return false;
}

// Live hot-plug detection: called every frame while the Gamepads tab is
// visible, but only does real work roughly once a second. Notifies when
// a controller was plugged in or unplugged since the last check, and
// refreshes the device list if so - without the user having to close and
// reopen the dialog to see the new device. Keeps running during an active
// capture too: if the capturing player has no device assigned yet, a
// controller that appears mid-capture is auto-assigned to them so
// pressing Capture before plugging anything in still works - but only
// when exactly one new device appeared; if that's ambiguous, or if the
// device the capturing player already had gets unplugged mid-capture,
// the capture is cancelled rather than guessing or being left stuck
// forever.
void PollDeviceChanges()
{
    if (!g_open || g_active_tab != 3) // Gamepads tab index
        return;

    static Uint32 last_device_poll_ms = 0;
    Uint32 now = SDL_GetTicks();
    if (now - last_device_poll_ms < 1000)
        return;
    last_device_poll_ms = now;

    if (!DeviceListChanged())
        return;

    std::vector<WxGamepadBackend::DeviceInfo> old_devices = g_devices;
    RefreshDeviceList();

    if (g_capturing_player < 0) return;
    JoystickProfile& capturing_profile = g_gamepad_profiles[g_capturing_player];

    if (capturing_profile.device_guid.empty()) {
        // No device was assigned to the capturing player at all (combo
        // still on "None"). If exactly one new controller appeared,
        // assign it and let the capture carry on waiting for the actual
        // button press - this is what makes "press Capture, then plug
        // the controller in" work. If none appeared (something
        // unrelated changed in the device set) or more than one
        // appeared at once, there's nothing safe to guess: cancel the
        // capture rather than silently binding it to a possibly wrong
        // device, or leaving it stuck forever waiting on nothing.
        const WxGamepadBackend::DeviceInfo* new_device = nullptr;
        int new_count = 0;

        for (const auto& dev : g_devices) {
            bool is_new = true;
            for (const auto& old_dev : old_devices) {
                if (old_dev.guid == dev.guid) { is_new = false; break; }
            }
            if (!is_new) continue;
            ++new_count;
            if (new_count == 1) new_device = &dev;
        }

        if (new_count == 1) {
            capturing_profile.device_guid = new_device->guid;
            capturing_profile.host_device_index = new_device->index;
            GamepadBackend().RefreshDeviceState(new_device->index);
        } else {
            StopCapture();
        }
    } else if (capturing_profile.host_device_index < 0) {
        // A device *was* assigned, but RefreshDeviceList() just
        // couldn't find it among the currently connected devices - it
        // was unplugged mid-capture. The capture can never complete
        // against a device that isn't there; cancel it instead of
        // leaving the button stuck showing "Capturing..." forever.
        StopCapture();
    }
    // else: still capturing against a device that's still connected -
    // nothing to do, PollCapture() keeps polling it normally.
}

#endif//SDL_USE_JOYSTICK

void CommitToOptions()
{
	xOptions::eOption<int>* op;
	xOptions::eOption<bool>* opb;

	op = xOptions::eOption<int>::Find("sound chip"); if(op) { op->Set(g_sound_chip); op->Apply(); }
	op = xOptions::eOption<int>::Find("ay stereo");  if(op) { op->Set(g_ay_stereo); op->Apply(); }
	op = xOptions::eOption<int>::Find("drive");      if(op) { op->Set(g_drive); op->Apply(); }
	op = xOptions::eOption<int>::Find("joystick");   if(op) { op->Set(g_joystick); op->Apply(); }

	opb = xOptions::eOption<bool>::Find("gigascreen");  if(opb) { opb->Set(g_gigascreen); opb->Apply(); }
	opb = xOptions::eOption<bool>::Find("scanlines");   if(opb) { opb->Set(g_scanlines); opb->Apply(); }
	opb = xOptions::eOption<bool>::Find("pal effects"); if(opb) { opb->Set(g_pal_effects); opb->Apply(); }
	op = xOptions::eOption<int>::Find("pal strength");  if(op) { op->Set(g_pal_strength); op->Apply(); }
	op = xOptions::eOption<int>::Find("beam spread");   if(op) { op->Set(g_beam_spread); op->Apply(); }
	opb = xOptions::eOption<bool>::Find("mipmapping");  if(opb) { opb->Set(g_mipmapping); opb->Apply(); }
	op = xOptions::eOption<int>::Find("mask scale");    if(op) { op->Set(g_mask_scale); op->Apply(); }

#ifdef SDL_USE_JOYSTICK
	for(int i = 0; i < 2; ++i)
	{
		SaveProfileToOptions(i, g_gamepad_profiles[i]);
		if(g_gamepad_profiles[i].host_device_index >= 0)
			GamepadBackend().RefreshDeviceState(g_gamepad_profiles[i].host_device_index);
	}
#endif//SDL_USE_JOYSTICK
}

// ---------------------------------------------------------------------------
// Small layout helper: label + slider + live value on one row, matching the
// "slider row" layout wx_optionsdialog.cpp uses for CRT Mask Scale/PAL
// Strength/Beam Spread.
// ---------------------------------------------------------------------------

void SliderRow(const char* label, int* value, int lo, int hi, const char* value_fmt_is_percent)
{
	ImGui::TextUnformatted(label);
	ImGui::SetNextItemWidth(220.0f);
	ImGui::SliderInt((std::string("##") + label).c_str(), value, lo, hi, value_fmt_is_percent);
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

void DrawAudioTab()
{
	ImGui::SeparatorText("Sound Chip");
	ImGui::RadioButton("AY-3-8910", &g_sound_chip, SC_AY);
	ImGui::RadioButton("YM2149F", &g_sound_chip, SC_YM);

	ImGui::Spacing();
	ImGui::SeparatorText("Stereo Mode");
	static const char* stereo_names[] = { "ABC", "ACB", "BAC", "BCA", "CAB", "CBA", "Mono" };
	ImGui::SetNextItemWidth(160.0f);
	if(ImGui::BeginCombo("##stereo", stereo_names[g_ay_stereo >= 0 && g_ay_stereo < 7 ? g_ay_stereo : 0]))
	{
		for(int i = 0; i < 7; ++i)
		{
			bool sel = (i == g_ay_stereo);
			if(ImGui::Selectable(stereo_names[i], sel)) g_ay_stereo = i;
			if(sel) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	ImGui::Spacing();
	ImGui::Spacing();
	if(ImGui::Button("Restore Audio Defaults"))
	{
		g_sound_chip = DEFAULT_SOUND_CHIP;
		g_ay_stereo = DEFAULT_STEREO;
	}
}

void DrawVideoTab()
{
	ImGui::Checkbox("Enable Mipmapping", &g_mipmapping);
	ImGui::Checkbox("Enable Gigascreen", &g_gigascreen);
	ImGui::Checkbox("Enable CRT Scanlines", &g_scanlines);

	ImGui::Spacing();
	SliderRow("CRT Mask Scale", &g_mask_scale, 0, 4, "%d");

	ImGui::Spacing();
	ImGui::SeparatorText("PAL Effects");
	ImGui::Checkbox("Enable PAL effects", &g_pal_effects);
	SliderRow("PAL Strength", &g_pal_strength, 0, 100, "%d%%");
	SliderRow("Beam Spread", &g_beam_spread, 0, 200, "%d");

	ImGui::Spacing();
	ImGui::Spacing();
	if(ImGui::Button("Restore Video Defaults"))
	{
		g_gigascreen = DEFAULT_GIGASCREEN;
		g_scanlines = DEFAULT_SCANLINES;
		g_pal_effects = DEFAULT_PAL_EFFECTS;
		g_pal_strength = DEFAULT_PAL_STRENGTH;
		g_beam_spread = DEFAULT_BEAM_SPREAD;
		g_mipmapping = DEFAULT_MIPMAPPING;
		g_mask_scale = DEFAULT_MASK_SCALE;
	}
}

void DrawInputTab()
{
	ImGui::SeparatorText("Joystick Type");
	ImGui::RadioButton("Kempston", &g_joystick, J_KEMPSTON);
	ImGui::RadioButton("Cursor", &g_joystick, J_CURSOR);
	ImGui::RadioButton("QAOPSpace", &g_joystick, J_QAOPSPACE);
	ImGui::RadioButton("Sinclair 2", &g_joystick, J_SINCLAIR2);

	ImGui::Spacing();
	ImGui::Spacing();
	if(ImGui::Button("Restore Input Defaults"))
		g_joystick = DEFAULT_JOYSTICK;
}

void DrawDriveTab()
{
	ImGui::RadioButton("A", &g_drive, D_A);
	ImGui::RadioButton("B", &g_drive, D_B);
	ImGui::RadioButton("C", &g_drive, D_C);
	ImGui::RadioButton("D", &g_drive, D_D);

	ImGui::Spacing();
	ImGui::Spacing();
	if(ImGui::Button("Restore Disk Drive Defaults"))
		g_drive = DEFAULT_DRIVE;
}

#ifdef SDL_USE_JOYSTICK
void DrawPlayerSection(int player_idx)
{
	ImGui::PushID(player_idx);
	char header[16];
	snprintf(header, sizeof(header), "Player %d", player_idx + 1);
	ImGui::SeparatorText(header);

	// Device combo: "None" + every currently connected SDL_GameController.
	int selection = 0;
	for(size_t i = 0; i < g_devices.size(); ++i)
		if(g_devices[i].index == g_gamepad_profiles[player_idx].host_device_index)
			{ selection = (int)i + 1; break; }
	const char* preview = selection == 0 ? "None" : g_devices[selection - 1].name.c_str();
	ImGui::SetNextItemWidth(-1.0f);
	if(ImGui::BeginCombo("##device", preview))
	{
		bool sel_none = (selection == 0);
		if(ImGui::Selectable("None", sel_none))
		{
			g_gamepad_profiles[player_idx].host_device_index = -1;
			g_gamepad_profiles[player_idx].device_guid.clear();
		}
		for(size_t i = 0; i < g_devices.size(); ++i)
		{
			bool sel = ((int)i + 1 == selection);
			if(ImGui::Selectable(g_devices[i].name.c_str(), sel))
			{
				g_gamepad_profiles[player_idx].host_device_index = g_devices[i].index;
				g_gamepad_profiles[player_idx].device_guid = g_devices[i].guid;
				GamepadBackend().RefreshDeviceState(g_devices[i].index);
			}
			if(sel) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	ImGui::Spacing();
	static const EEmulatedJoystickInput inputs[6] = {
		EEmulatedJoystickInput::UP, EEmulatedJoystickInput::DOWN,
		EEmulatedJoystickInput::LEFT, EEmulatedJoystickInput::RIGHT,
		EEmulatedJoystickInput::FIRE1, EEmulatedJoystickInput::FIRE2
	};
	static const char* input_names[6] = { "UP", "DOWN", "LEFT", "RIGHT", "FIRE1", "FIRE2" };

	if(ImGui::BeginTable("##mapping", 3, ImGuiTableFlags_SizingFixedFit))
	{
		for(int i = 0; i < 6; ++i)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(input_names[i]);
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(MappingLabelText(player_idx, inputs[i]));
			ImGui::TableSetColumnIndex(2);
			ImGui::PushID(i);
			bool capturing = (g_capturing_player == player_idx && g_capturing_input == inputs[i]);
			bool no_device = (g_gamepad_profiles[player_idx].host_device_index < 0);

			if(capturing)
			{
				ImGui::BeginDisabled();
				ImGui::Button("Capturing...");
				ImGui::EndDisabled();
			}
			else if(no_device)
			{
				ImGui::BeginDisabled();
				ImGui::Button("Capture");
				ImGui::EndDisabled();
			}
			else if(ImGui::Button("Capture"))
			{
				StartCapture(player_idx, inputs[i]);
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::PopID();
}

void DrawGamepadsTab()
{
	PollCapture();
	PollDeviceChanges();

	float half_w = ImGui::GetContentRegionAvail().x * 0.5f - 8.0f;
	ImGui::BeginChild("##p1", ImVec2(half_w, 260), true);
	DrawPlayerSection(0);
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("##p2", ImVec2(half_w, 260), true);
	DrawPlayerSection(1);
	ImGui::EndChild();

	ImGui::Spacing();
	if(ImGui::Button("Restore Gamepad Defaults"))
	{
		StopCapture();
		for(int i = 0; i < 2; ++i)
			g_gamepad_profiles[i] = JoystickProfile();
		RefreshDeviceList();
	}
}
#endif//SDL_USE_JOYSTICK

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void OpenOptionsDialog()
{
	g_open = true;
	g_active_tab = 0;
	LoadCurrentSettings();
#ifdef SDL_USE_JOYSTICK
	for(int i = 0; i < 2; ++i)
		g_gamepad_profiles[i] = LoadProfileFromOptions(i);
	RefreshDeviceList();
	StopCapture();
#endif//SDL_USE_JOYSTICK
}

bool OptionsDialogActive() { return g_open; }

// See imgui_shared.h - mirrors the cleanup DrawOptionsDialog() itself runs
// when the user closes the window normally (its own 'if(!open)' branch
// below), so a forced close from outside behaves identically.
void CloseOptionsDialog()
{
	if(!g_open)
		return;
#ifdef SDL_USE_JOYSTICK
	StopCapture();
#endif//SDL_USE_JOYSTICK
	g_open = false;
}

void DrawOptionsDialog()
{
	if(!g_open)
		return;

	ImGui::SetNextWindowSize(ImVec2(620, 460), ImGuiCond_FirstUseEver);
	bool open = g_open;
	if(!ImGui::Begin("Options", &open, ImGuiWindowFlags_NoCollapse))
	{
		ImGui::End();
		g_open = open;
		return;
	}
	if(!open)
	{
#ifdef SDL_USE_JOYSTICK
		StopCapture();
#endif
		g_open = false;
		ImGui::End();
		return;
	}

	// Esc closes the topmost dialog while UI has keyboard focus (see
	// Loop1()'s ui_want_keyboard gate in sdl2_desktop.cpp - Esc is only even
	// offered to this window when the UI genuinely wants the keyboard right
	// now, not e.g. while the emulated ZX keyboard has it with this window
	// merely sitting open in the background). Goes through
	// CloseOptionsDialog() rather than setting g_open directly, matching
	// [x]/Cancel above: discard, don't CommitToOptions().
	// RootAndChildWindows so this still fires no matter which tab/child pane
	// currently has focus, not only the window's exact root.
	if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))
	{
		CloseOptionsDialog();
		ImGui::End();
		return;
	}

	static const char* tab_names[] = {
		"Audio", "Video", "Input",
#ifdef SDL_USE_JOYSTICK
		"Gamepads",
#endif
		"Disk Drives"
	};
	int tab_count = (int)(sizeof(tab_names) / sizeof(tab_names[0]));

	// Left-side vertical tab list, mirroring wx_optionsdialog.cpp's
	// wxListbook layout (see the file comment for why).
	ImGui::BeginChild("##tabs", ImVec2(140, -ImGui::GetFrameHeightWithSpacing()), true);
	for(int i = 0; i < tab_count; ++i)
	{
		bool selected = (g_active_tab == i);
		if(ImGui::Selectable(tab_names[i], selected))
			g_active_tab = i;
	}
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::BeginChild("##tabcontent", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
	const char* selected_name = (g_active_tab >= 0 && g_active_tab < tab_count) ? tab_names[g_active_tab] : "";
	if(strcmp(selected_name, "Audio") == 0) DrawAudioTab();
	else if(strcmp(selected_name, "Video") == 0) DrawVideoTab();
	else if(strcmp(selected_name, "Input") == 0) DrawInputTab();
#ifdef SDL_USE_JOYSTICK
	else if(strcmp(selected_name, "Gamepads") == 0) DrawGamepadsTab();
#endif
	else if(strcmp(selected_name, "Disk Drives") == 0) DrawDriveTab();
	ImGui::EndChild();

	ImGui::Separator();
	if(ImGui::Button("OK", ImVec2(90, 0)))
	{
		CommitToOptions();
#ifdef SDL_USE_JOYSTICK
		StopCapture();
#endif
		g_open = false;
	}
	ImGui::SameLine();
	if(ImGui::Button("Cancel", ImVec2(90, 0)))
	{
#ifdef SDL_USE_JOYSTICK
		StopCapture();
#endif
		g_open = false;
	}

	ImGui::End();
}

}//namespace xImGui
}//namespace xPlatform

#endif//USE_SDL2_DESKTOP
