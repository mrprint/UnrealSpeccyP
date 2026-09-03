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
//  (OpenOptionsDialog()) snapshots every option into local state when the
//  dialog opens; every widget below reads/writes that local snapshot, not
//  xOptions directly; OK commits it all via Set()+Apply(); Cancel discards
//  it. The per-tab "Restore Defaults" buttons only touch the local snapshot
//  too, exactly like OnResetAudio/Video/Input/Drive/Gamepad in wx.
//
//  Gamepad capture/device-list state is a distinct concern from the rest of
//  the dialog's buffered settings - it has its own GamepadMappingPanel class
//  further down, which OptionsDialog owns as a member rather than folding
//  into itself.
//
//  Gamepads tab: same GUID-identified device combo + live capture-by-input
//  model as wx's CreateGamepadsPage()/StartCaptureMode()/OnTimer(), reusing
//  sdl2_desktop_gamepad.h (ported wx_gamepad.h/joystick_mapper.h) - see that
//  header for why it's a port rather than a shared #include. No separate
//  wxTimer is needed for either the 50ms capture poll or the device-hotplug
//  poll: this whole dialog already redraws every real frame (see
//  sdl2_desktop.cpp's single-threaded loop), so both are just checked once
//  per GamepadMappingPanel::Draw() call instead.
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

// Values of OptionsDialog::m_active_tab; the order matches tab_names[] in
// OptionsDialog::Draw(). (The Gamepads tab is only in the list when
// SDL_USE_JOYSTICK is defined, so only that entry is ever referenced by
// name; everything else treats m_active_tab as a plain list index.)
enum class EOptionsTab { Audio, Video, Input, Gamepads, Drives };

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

// ---------------------------------------------------------------------------
// GamepadMappingPanel - the Gamepads tab's own state and drawing: device
// enumeration, per-player profiles, and the live capture-by-input flow. A
// distinct concern from the rest of OptionsDialog's buffered settings (see
// the file comment above), so it's its own class rather than more fields
// and methods on OptionsDialog itself; OptionsDialog owns one as a member.
// ---------------------------------------------------------------------------

class GamepadMappingPanel
{
public:
	// Snapshots both players' profiles from xOptions and refreshes the
	// device list - called when the dialog opens (mirrors
	// OptionsDialog::LoadCurrentSettings() for the rest of the dialog).
	void Load()
	{
		for(int i = 0; i < 2; ++i)
			m_profiles[i] = LoadProfileFromOptions(i);
		RefreshDeviceList();
		StopCapture();
	}

	// Writes both players' profiles back to xOptions - called from OK.
	void Commit()
	{
		for(int i = 0; i < 2; ++i)
		{
			SaveProfileToOptions(i, m_profiles[i]);
			if(m_profiles[i].host_device_index >= 0)
				GamepadBackend().RefreshDeviceState(m_profiles[i].host_device_index);
		}
	}

	void StopCapture()
	{
		m_capturing_player = -1;
	}

	// Draws both player panes and the Restore Defaults button - called once
	// per frame while the Gamepads tab is selected.
	void Draw()
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
				m_profiles[i] = JoystickProfile();
			RefreshDeviceList();
		}
	}

private:
	JoystickProfile m_profiles[2];
	std::vector<WxGamepadBackend::DeviceInfo> m_devices;
	int m_capturing_player = -1;
	EEmulatedJoystickInput m_capturing_input = EEmulatedJoystickInput::UP;

	void RefreshDeviceList()
	{
		m_devices = GamepadBackend().EnumerateDevices();
		for(int player = 0; player < 2; ++player)
		{
			const std::string& guid = m_profiles[player].device_guid;
			int found_index = -1;
			if(!guid.empty())
			{
				for(const auto& dev : m_devices)
					if(dev.guid == guid) { found_index = dev.index; break; }
			}
			m_profiles[player].host_device_index = found_index;
			if(found_index >= 0)
				GamepadBackend().RefreshDeviceState(found_index);
		}
	}

	// If the assigned device is not connected, show a short placeholder
	// instead of stale mapping data that belongs to a different controller.
	const char* MappingLabelText(int player_idx, EEmulatedJoystickInput input) const
	{
		if(m_profiles[player_idx].host_device_index < 0)
			return "No device";

		auto it = m_profiles[player_idx].input_map.find(input);
		if(it == m_profiles[player_idx].input_map.end())
			return "Not set";
		return SourceTypeDisplayString(it->second.source_type);
	}

	void StartCapture(int player_idx, EEmulatedJoystickInput input)
	{
		m_capturing_player = player_idx;
		m_capturing_input = input;
	}

	// Checked once per frame while a capture is pending - equivalent of
	// OptionsDialog::OnTimer(), just driven by the main loop's own frame
	// rate instead of a dedicated 50ms wxTimer.
	void PollCapture()
	{
		if(m_capturing_player < 0)
			return;

		int assigned = m_profiles[m_capturing_player].host_device_index;
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
			m_profiles[m_capturing_player].input_map[m_capturing_input] = entry;
			StopCapture();
			return;
		}
	}

	// True if the set of connected controllers changed since the last
	// RefreshDeviceList() call (compares m_devices against a fresh
	// enumeration). Used by PollDeviceChanges() to avoid rebuilding the
	// comboboxes - and disturbing whatever the user is doing with them -
	// every single frame when nothing actually changed.
	bool DeviceListChanged() const
	{
		auto current = GamepadBackend().EnumerateDevices();
		if(current.size() != m_devices.size()) return true;
		for(size_t i = 0; i < current.size(); ++i) {
			if(current[i].index != m_devices[i].index || current[i].name != m_devices[i].name) {
				return true;
			}
		}
		return false;
	}

	// Live hot-plug detection: called every frame while the Gamepads tab is
	// visible (see Draw() - this is only ever reached while that's true, so
	// unlike the old free-function version there's no separate g_open/
	// active-tab guard to re-check here), but only does real work roughly
	// once a second. Notifies when a controller was plugged in or unplugged
	// since the last check, and refreshes the device list if so - without
	// the user having to close and reopen the dialog to see the new device.
	// Keeps running during an active capture too: if the capturing player
	// has no device assigned yet, a controller that appears mid-capture is
	// auto-assigned to them so pressing Capture before plugging anything in
	// still works - but only when exactly one new device appeared; if
	// that's ambiguous, or if the device the capturing player already had
	// gets unplugged mid-capture, the capture is cancelled rather than
	// guessing or being left stuck forever.
	void PollDeviceChanges()
	{
		static Uint32 last_device_poll_ms = 0;
		Uint32 now = SDL_GetTicks();
		if (now - last_device_poll_ms < 1000)
			return;
		last_device_poll_ms = now;

		if (!DeviceListChanged())
			return;

		std::vector<WxGamepadBackend::DeviceInfo> old_devices = m_devices;
		RefreshDeviceList();

		if (m_capturing_player < 0) return;
		JoystickProfile& capturing_profile = m_profiles[m_capturing_player];

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

			for (const auto& dev : m_devices) {
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

	void DrawPlayerSection(int player_idx)
	{
		ImGui::PushID(player_idx);
		char header[16];
		snprintf(header, sizeof(header), "Player %d", player_idx + 1);
		ImGui::SeparatorText(header);

		// Device combo: "None" + every currently connected SDL_GameController.
		int selection = 0;
		for(size_t i = 0; i < m_devices.size(); ++i)
			if(m_devices[i].index == m_profiles[player_idx].host_device_index)
				{ selection = (int)i + 1; break; }
		const char* preview = selection == 0 ? "None" : m_devices[selection - 1].name.c_str();
		ImGui::SetNextItemWidth(-1.0f);
		if(ImGui::BeginCombo("##device", preview))
		{
			bool sel_none = (selection == 0);
			if(ImGui::Selectable("None", sel_none))
			{
				m_profiles[player_idx].host_device_index = -1;
				m_profiles[player_idx].device_guid.clear();
			}
			for(size_t i = 0; i < m_devices.size(); ++i)
			{
				bool sel = ((int)i + 1 == selection);
				if(ImGui::Selectable(m_devices[i].name.c_str(), sel))
				{
					m_profiles[player_idx].host_device_index = m_devices[i].index;
					m_profiles[player_idx].device_guid = m_devices[i].guid;
					GamepadBackend().RefreshDeviceState(m_devices[i].index);
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
				bool capturing = (m_capturing_player == player_idx && m_capturing_input == inputs[i]);
				bool no_device = (m_profiles[player_idx].host_device_index < 0);

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
};

#endif//SDL_USE_JOYSTICK

// ---------------------------------------------------------------------------
// Small layout helper: label + slider + live value on one row, matching the
// "slider row" layout wx_optionsdialog.cpp uses for CRT Mask Scale/PAL
// Strength/Beam Spread. Stateless, so it stays a free function rather than
// an OptionsDialog method.
// ---------------------------------------------------------------------------

void SliderRow(const char* label, int* value, int lo, int hi, const char* value_fmt_is_percent)
{
	ImGui::TextUnformatted(label);
	ImGui::SetNextItemWidth(220.0f);
	ImGui::SliderInt((std::string("##") + label).c_str(), value, lo, hi, value_fmt_is_percent);
}

// ---------------------------------------------------------------------------
// OptionsDialog - owns every piece of the dialog's buffered state (see the
// file comment above) and every function that reads or writes it. Only one
// instance of this ever exists (g_dialog below); imgui_shared.h's four
// free functions (OpenOptionsDialog/CloseOptionsDialog/DrawOptionsDialog/
// OptionsDialogActive) are thin facades over it.
// ---------------------------------------------------------------------------

class OptionsDialog
{
public:
	void Open();
	void Close();
	void Draw();
	bool IsOpen() const { return m_open; }

private:
	bool m_open = false;
	int m_active_tab = 0; // see EOptionsTab / tab_names[] in Draw()

	int m_sound_chip = SC_AY;
	int m_ay_stereo = AS_ABC;

	bool m_gigascreen = false;
	bool m_scanlines = false;
	bool m_pal_effects = true;
	int m_pal_strength = 50;
	int m_beam_spread = 30;
	bool m_mipmapping = true;
	int m_mask_scale = 1;

	int m_joystick = J_KEMPSTON;

	int m_drive = D_A;

#ifdef SDL_USE_JOYSTICK
	GamepadMappingPanel m_gamepad;
#endif//SDL_USE_JOYSTICK

	void LoadCurrentSettings();
	void CommitToOptions();

	void DrawAudioTab();
	void DrawVideoTab();
	void DrawInputTab();
	void DrawDriveTab();
#ifdef SDL_USE_JOYSTICK
	void DrawGamepadsTab();
#endif//SDL_USE_JOYSTICK
};

void OptionsDialog::LoadCurrentSettings()
{
	xOptions::eOption<int>* op;
	xOptions::eOption<bool>* opb;

	op = xOptions::eOption<int>::Find("sound chip");   if(op) m_sound_chip = *op;
	op = xOptions::eOption<int>::Find("ay stereo");    if(op) m_ay_stereo = *op;

	opb = xOptions::eOption<bool>::Find("gigascreen");   if(opb) m_gigascreen = *opb;
	opb = xOptions::eOption<bool>::Find("scanlines");    if(opb) m_scanlines = *opb;
	opb = xOptions::eOption<bool>::Find("pal effects");  if(opb) m_pal_effects = *opb;
	op = xOptions::eOption<int>::Find("pal strength");   if(op) m_pal_strength = *op;
	op = xOptions::eOption<int>::Find("beam spread");    if(op) m_beam_spread = *op;
	opb = xOptions::eOption<bool>::Find("mipmapping");   m_mipmapping = opb ? (bool)*opb : DEFAULT_MIPMAPPING;
	op = xOptions::eOption<int>::Find("mask scale");     m_mask_scale = op ? *op : DEFAULT_MASK_SCALE;

	m_drive = OpDrive();
	m_joystick = OpJoystick();
}

void OptionsDialog::CommitToOptions()
{
	xOptions::eOption<int>* op;
	xOptions::eOption<bool>* opb;

	op = xOptions::eOption<int>::Find("sound chip"); if(op) { op->Set(m_sound_chip); op->Apply(); }
	op = xOptions::eOption<int>::Find("ay stereo");  if(op) { op->Set(m_ay_stereo); op->Apply(); }
	op = xOptions::eOption<int>::Find("drive");      if(op) { op->Set(m_drive); op->Apply(); }
	op = xOptions::eOption<int>::Find("joystick");   if(op) { op->Set(m_joystick); op->Apply(); }

	opb = xOptions::eOption<bool>::Find("gigascreen");  if(opb) { opb->Set(m_gigascreen); opb->Apply(); }
	opb = xOptions::eOption<bool>::Find("scanlines");   if(opb) { opb->Set(m_scanlines); opb->Apply(); }
	opb = xOptions::eOption<bool>::Find("pal effects"); if(opb) { opb->Set(m_pal_effects); opb->Apply(); }
	op = xOptions::eOption<int>::Find("pal strength");  if(op) { op->Set(m_pal_strength); op->Apply(); }
	op = xOptions::eOption<int>::Find("beam spread");   if(op) { op->Set(m_beam_spread); op->Apply(); }
	opb = xOptions::eOption<bool>::Find("mipmapping");  if(opb) { opb->Set(m_mipmapping); opb->Apply(); }
	op = xOptions::eOption<int>::Find("mask scale");    if(op) { op->Set(m_mask_scale); op->Apply(); }

#ifdef SDL_USE_JOYSTICK
	m_gamepad.Commit();
#endif//SDL_USE_JOYSTICK
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

void OptionsDialog::DrawAudioTab()
{
	ImGui::SeparatorText("Sound Chip");
	ImGui::RadioButton("AY-3-8910", &m_sound_chip, SC_AY);
	ImGui::RadioButton("YM2149F", &m_sound_chip, SC_YM);

	ImGui::Spacing();
	ImGui::SeparatorText("Stereo Mode");
	static const char* stereo_names[] = { "ABC", "ACB", "BAC", "BCA", "CAB", "CBA", "Mono" };
	ImGui::SetNextItemWidth(160.0f);
	if(ImGui::BeginCombo("##stereo", stereo_names[m_ay_stereo >= 0 && m_ay_stereo < 7 ? m_ay_stereo : 0]))
	{
		for(int i = 0; i < 7; ++i)
		{
			bool sel = (i == m_ay_stereo);
			if(ImGui::Selectable(stereo_names[i], sel)) m_ay_stereo = i;
			if(sel) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	ImGui::Spacing();
	ImGui::Spacing();
	if(ImGui::Button("Restore Audio Defaults"))
	{
		m_sound_chip = DEFAULT_SOUND_CHIP;
		m_ay_stereo = DEFAULT_STEREO;
	}
}

void OptionsDialog::DrawVideoTab()
{
	ImGui::Checkbox("Enable Mipmapping", &m_mipmapping);
	ImGui::Checkbox("Enable Gigascreen", &m_gigascreen);
	ImGui::Checkbox("Enable CRT Scanlines", &m_scanlines);

	ImGui::Spacing();
	SliderRow("CRT Mask Scale", &m_mask_scale, 0, 4, "%d");

	ImGui::Spacing();
	ImGui::SeparatorText("PAL Effects");
	ImGui::Checkbox("Enable PAL effects", &m_pal_effects);
	SliderRow("PAL Strength", &m_pal_strength, 0, 100, "%d%%");
	SliderRow("Beam Spread", &m_beam_spread, 0, 200, "%d");

	ImGui::Spacing();
	ImGui::Spacing();
	if(ImGui::Button("Restore Video Defaults"))
	{
		m_gigascreen = DEFAULT_GIGASCREEN;
		m_scanlines = DEFAULT_SCANLINES;
		m_pal_effects = DEFAULT_PAL_EFFECTS;
		m_pal_strength = DEFAULT_PAL_STRENGTH;
		m_beam_spread = DEFAULT_BEAM_SPREAD;
		m_mipmapping = DEFAULT_MIPMAPPING;
		m_mask_scale = DEFAULT_MASK_SCALE;
	}
}

void OptionsDialog::DrawInputTab()
{
	ImGui::SeparatorText("Joystick Type");
	ImGui::RadioButton("Kempston", &m_joystick, J_KEMPSTON);
	ImGui::RadioButton("Cursor", &m_joystick, J_CURSOR);
	ImGui::RadioButton("QAOPSpace", &m_joystick, J_QAOPSPACE);
	ImGui::RadioButton("Sinclair 2", &m_joystick, J_SINCLAIR2);

	ImGui::Spacing();
	ImGui::Spacing();
	if(ImGui::Button("Restore Input Defaults"))
		m_joystick = DEFAULT_JOYSTICK;
}

void OptionsDialog::DrawDriveTab()
{
	ImGui::RadioButton("A", &m_drive, D_A);
	ImGui::RadioButton("B", &m_drive, D_B);
	ImGui::RadioButton("C", &m_drive, D_C);
	ImGui::RadioButton("D", &m_drive, D_D);

	ImGui::Spacing();
	ImGui::Spacing();
	if(ImGui::Button("Restore Disk Drive Defaults"))
		m_drive = DEFAULT_DRIVE;
}

#ifdef SDL_USE_JOYSTICK
void OptionsDialog::DrawGamepadsTab()
{
	m_gamepad.Draw();
}
#endif//SDL_USE_JOYSTICK

// ---------------------------------------------------------------------------
// Open/Close/Draw
// ---------------------------------------------------------------------------

void OptionsDialog::Open()
{
	m_open = true;
	m_active_tab = (int)EOptionsTab::Audio;
	LoadCurrentSettings();
#ifdef SDL_USE_JOYSTICK
	m_gamepad.Load();
#endif//SDL_USE_JOYSTICK
}

// Mirrors the cleanup Draw() itself runs when the user closes the window
// normally (its own 'if(!open)' branch below), so a forced close from
// outside behaves identically.
void OptionsDialog::Close()
{
	if(!m_open)
		return;
#ifdef SDL_USE_JOYSTICK
	m_gamepad.StopCapture();
#endif//SDL_USE_JOYSTICK
	m_open = false;
}

void OptionsDialog::Draw()
{
	if(!m_open)
		return;

	ImGui::SetNextWindowSize(ImVec2(620, 460), ImGuiCond_FirstUseEver);
	bool open = m_open;
	if(!ImGui::Begin("Options", &open, ImGuiWindowFlags_NoCollapse))
	{
		ImGui::End();
		m_open = open;
		return;
	}
	if(!open)
	{
#ifdef SDL_USE_JOYSTICK
		m_gamepad.StopCapture();
#endif
		m_open = false;
		ImGui::End();
		return;
	}

	// Esc closes the topmost dialog while UI has keyboard focus (see
	// Loop1()'s ui_want_keyboard gate in sdl2_desktop.cpp - Esc is only even
	// offered to this window when the UI genuinely wants the keyboard right
	// now, not e.g. while the emulated ZX keyboard has it with this window
	// merely sitting open in the background). Goes through Close() rather
	// than setting m_open directly, matching [x]/Cancel above: discard,
	// don't CommitToOptions(). RootAndChildWindows so this still fires no
	// matter which tab/child pane currently has focus, not only the
	// window's exact root.
	if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))
	{
		Close();
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
		bool selected = (m_active_tab == i);
		if(ImGui::Selectable(tab_names[i], selected))
			m_active_tab = i;
	}
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::BeginChild("##tabcontent", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
	const char* selected_name = (m_active_tab >= 0 && m_active_tab < tab_count) ? tab_names[m_active_tab] : "";
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
		m_gamepad.StopCapture();
#endif
		m_open = false;
	}
	ImGui::SameLine();
	if(ImGui::Button("Cancel", ImVec2(90, 0)))
	{
#ifdef SDL_USE_JOYSTICK
		m_gamepad.StopCapture();
#endif
		m_open = false;
	}

	ImGui::End();
}

OptionsDialog g_dialog;

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API - thin facades over the one OptionsDialog instance above.
// ---------------------------------------------------------------------------

void OpenOptionsDialog() { g_dialog.Open(); }

bool OptionsDialogActive() { return g_dialog.IsOpen(); }

void CloseOptionsDialog() { g_dialog.Close(); }

void DrawOptionsDialog() { g_dialog.Draw(); }

}//namespace xImGui
}//namespace xPlatform

#endif//USE_SDL2_DESKTOP
