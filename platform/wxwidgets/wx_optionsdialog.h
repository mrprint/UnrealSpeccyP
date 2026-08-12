#ifndef __WX_OPTIONS_DIALOG_H__
#define __WX_OPTIONS_DIALOG_H__

#pragma once

#ifdef USE_WXWIDGETS

#include <wx/dialog.h>
#include <wx/listbook.h>
#include <wx/radiobut.h>
#include <wx/combobox.h>
#include <wx/checkbox.h>
#include <wx/stattext.h>
#include <wx/slider.h>
#include <wx/bmpbuttn.h>
#include <wx/artprov.h>

#ifdef USE_SDL2_GAMEPAD
#include <wx/timer.h>
#include <array>
#include <vector>
#include "wx_gamepad.h"
#endif

class wxWindow;
class wxPanel;
class wxCommandEvent;
class wxTimerEvent;
class wxBoxSizer;
class wxButton;

namespace xPlatform {

#ifdef USE_SDL2_GAMEPAD
	struct JoystickProfile;
#endif

	enum DialogIDs {
		ID_RADIO_SOUND = 101,
		ID_RADIO_DRIVE,
		ID_RADIO_JOYSTICK,
		ID_CHECK_GIGASCREEN = 201,
		ID_CHECK_SCANLINES,
		ID_CHECK_PAL_EFFECTS,
		ID_CHECK_MIPMAPPING = 251,
		// Add reset button IDs
		ID_RESET_AUDIO = 301,
		ID_RESET_VIDEO,
		ID_RESET_INPUT,
		ID_RESET_DRIVE,
		ID_RESET_GAMEPAD,

#ifdef USE_SDL2_GAMEPAD
		// Gamepads tab controls (formerly GamepadConfigDialog)
		ID_DEVICE_COMBO_P1 = 500,
		ID_CAPTURE_BTN_UP_P1,
		ID_CAPTURE_BTN_DOWN_P1,
		ID_CAPTURE_BTN_LEFT_P1,
		ID_CAPTURE_BTN_RIGHT_P1,
		ID_CAPTURE_BTN_FIRE1_P1,
		ID_CAPTURE_BTN_FIRE2_P1,

		ID_DEVICE_COMBO_P2 = 600,
		ID_CAPTURE_BTN_UP_P2,
		ID_CAPTURE_BTN_DOWN_P2,
		ID_CAPTURE_BTN_LEFT_P2,
		ID_CAPTURE_BTN_RIGHT_P2,
		ID_CAPTURE_BTN_FIRE1_P2,
		ID_CAPTURE_BTN_FIRE2_P2,

		ID_TIMER_CAPTURE = 700,
		// Slow poll timer: notices controllers plugged in/unplugged while the
		// dialog is open and refreshes the device list live (see
		// OnDevicePollTimer). Separate from ID_TIMER_CAPTURE, which fires
		// much faster and only while actively capturing an input.
		ID_TIMER_DEVICE_POLL,
#endif
	};

	// Declared in wx_canvas.cpp.
	void PauseGLCanvas();
	void ResumeGLCanvas();
	void LockEmulator();
	void UnlockEmulator();
#ifdef USE_SDL2_GAMEPAD
	// Re-reads gamepad device/mapping settings from xOptions into the live
	// GLCanvas, so changes made on the Gamepads tab take effect immediately
	// on OK instead of only after the emulator is restarted.
	void ReloadGamepadProfiles();
#endif

	// ---------------------------------------------------------------------------
	// ScopedRenderPause
	//
	// RAII guard: pauses the GL render thread for the duration of a scope.
	// Use before mutating any render option (zoom, gigascreen, scanlines, PAL
	// effects, etc.) so the render thread is guaranteed idle while the option
	// value changes.  Prevents mid-frame option reads that can stall the GPU
	// driver and make the whole system feel unresponsive.
	// ---------------------------------------------------------------------------
	struct ScopedRenderPause
	{
		ScopedRenderPause() { PauseGLCanvas(); }
		~ScopedRenderPause() { ResumeGLCanvas(); }
		// Non-copyable, non-movable.
		ScopedRenderPause(const ScopedRenderPause&) = delete;
		ScopedRenderPause& operator=(const ScopedRenderPause&) = delete;
	};

	// Serializes main-thread access to emulator state with the render thread.
	// Use for every Handler() call and every option Apply() that touches
	// emulator internals (sound chip, stereo, drive, joystick, tape, etc.).
	//
	// IMPORTANT: m_emu_mutex is std::mutex (non-recursive).  Never nest two
	// ScopedEmuLock guards on the same thread — it will deadlock.  If you need
	// to call two Handler() operations in sequence, use a single guard for both.
	struct ScopedEmuLock
	{
		ScopedEmuLock() { LockEmulator(); }
		~ScopedEmuLock() { UnlockEmulator(); }
		ScopedEmuLock(const ScopedEmuLock&) = delete;
		ScopedEmuLock& operator=(const ScopedEmuLock&) = delete;
	};

	//=============================================================================
	//	OptionsDialog
	//
	//	A single tabbed dialog that hosts every emulator option section: Audio,
	//	Video, Input, Gamepads and Disk Drives. Tab selection uses a wxListbook
	//	(wxLB_LEFT) rather than a plain wxNotebook: on GTK, wxNotebook rotates
	//	tab captions 90 degrees when tabs are placed on the left/right, which
	//	is not what we want here. wxListbook's tab strip is a normal list
	//	control, so captions always stay horizontal regardless of platform,
	//	while still being stacked vertically down the left edge.
	//	Gamepads used to be a separate modal dialog (GamepadConfigDialog); it
	//	is now just another tab here, positioned right after Input. Every tab
	//	keeps its own "Restore ... Defaults" button, and — like every other
	//	tab — changes made on the Gamepads tab are only written to xOptions
	//	when the user presses OK, not as they're made.
	//-----------------------------------------------------------------------------
	class OptionsDialog : public wxDialog
	{
	public:
		OptionsDialog(wxWindow* parent);
		virtual ~OptionsDialog();

	private:
		void OnSoundChipChanged(wxCommandEvent& event);
		void OnStereoChanged(wxCommandEvent& event);
		void OnBetaDriveChanged(wxCommandEvent& event);
		void OnJoyTypeChanged(wxCommandEvent& event);
		void OnCheckboxChanged(wxCommandEvent& event);
		void OnSliderChanged(wxCommandEvent& event);
		void OnOK(wxCommandEvent& event);


		// Reset button handlers
		void OnResetAudio(wxCommandEvent& event);
		void OnResetVideo(wxCommandEvent& event);
		void OnResetInput(wxCommandEvent& event);
		void OnResetDrive(wxCommandEvent& event);

		// Local state (not written to xOptions until OK)
		int sound_chip;
		int ay_stereo;
		eDrive beta_drive;
		eJoystick joy_type;

	private:
		DECLARE_EVENT_TABLE()

		// Tabs (see class comment above for why this is a wxListbook and not
		// a wxNotebook)
		wxListbook* notebook_;

		wxPanel* CreateAudioPage(wxWindow* parent);
		wxPanel* CreateVideoPage(wxWindow* parent);
		wxPanel* CreateInputPage(wxWindow* parent);
		wxPanel* CreateDrivePage(wxWindow* parent);

		wxRadioButton* radioAy_;
		wxRadioButton* radioYm_;
		wxComboBox* comboStereo_;
		wxRadioButton* radioA_;
		wxRadioButton* radioB_;
		wxRadioButton* radioC_;
		wxRadioButton* radioD_;
		wxRadioButton* radioKempston_;
		wxRadioButton* radioCursor_;
		wxRadioButton* radioQaoSpace_;
		wxRadioButton* radioSinclair2_;
		wxCheckBox* checkGigascreen_;
		wxCheckBox* checkScanlines_;
		wxCheckBox* checkPalEffects_;
		wxSlider* sliderPalStrength_;
		wxStaticText* labelPalStrength_;
		wxSlider* sliderBeamSpread_;
		wxStaticText* labelBeamSpread_;
		wxCheckBox* checkMipmapping_;
		wxSlider* sliderMaskScale_;
		wxStaticText* labelMaskScale_;

		bool gigascreen_enabled;
		bool scanlines_enabled;
		bool pal_effects_enabled;
		int pal_strength_val;
		int beam_spread_val;
		bool mipmap_enabled;
		int mask_scale_val;

		void ReflectSettings();
		void LoadCurrentSettings();

		// Reset buttons (one per tab)
		wxBitmapButton* resetAudioBtn_;
		wxBitmapButton* resetVideoBtn_;
		wxBitmapButton* resetInputBtn_;
		wxBitmapButton* resetDriveBtn_;

#ifdef USE_SDL2_GAMEPAD
		// ---------------------------------------------------------------
		// Gamepads tab (formerly GamepadConfigDialog, now embedded here)
		// ---------------------------------------------------------------
		void OnDeviceSelected(wxCommandEvent& event);
		void OnCaptureClick(wxCommandEvent& event);
		void OnTimer(wxTimerEvent& event);
		void OnResetGamepad(wxCommandEvent& event);
		// Makes sure an in-progress capture is cancelled if the whole
		// options dialog is closed via Cancel while capturing.
		void OnCancelBtn(wxCommandEvent& event);
		// Live hot-plug detection: fires roughly once a second while the
		// dialog is open, notices when a controller was plugged in or
		// unplugged since the last check, and refreshes the device
		// comboboxes if so - without the user having to close and reopen
		// the dialog to see the new device. Keeps running during an active
		// capture too (does not skip it): if the capturing player has no
		// device assigned yet, a controller that appears mid-capture is
		// auto-assigned to them so pressing Capture before plugging
		// anything in still works - but only when exactly one new device
		// appeared; if that's ambiguous, or if the device the capturing
		// player already had gets unplugged mid-capture, the capture is
		// cancelled (StopCaptureMode()) rather than guessing or being left
		// stuck forever.
		void OnDevicePollTimer(wxTimerEvent& event);

		struct PlayerControls {
			wxComboBox* device_combo;
			std::array<wxButton*, 6> capture_buttons;
			std::array<wxStaticText*, 6> source_labels;
		};

		std::array<PlayerControls, 2> m_players;

		// Local, not-yet-applied gamepad state (device assignment + input
		// mapping) per player — mirrors how sound_chip/beta_drive/etc. are
		// buffered above. Seeded from xOptions in LoadCurrentSettings(),
		// mutated by OnDeviceSelected()/OnTimer()/OnResetGamepad(), and only
		// written back to xOptions in OnOK().
		std::array<JoystickProfile, 2> gamepadProfiles_;

		// Кэш последнего списка устройств из EnumerateDevices(). Индекс в этом
		// векторе НЕ совпадает с реальным SDL device_index (устройства без
		// геймпад-маппинга пропускаются при перечислении), поэтому позицию в
		// комбобоксе всегда нужно транслировать через этот список, а не
		// использовать её как device_index напрямую.
		std::vector<WxGamepadBackend::DeviceInfo> m_devices;

		int m_capturing_player = -1;
		EEmulatedJoystickInput m_capturing_input = EEmulatedJoystickInput::UP;
		wxTimer* m_capture_timer = nullptr;
		wxTimer* m_device_poll_timer = nullptr;

		wxBitmapButton* resetGamepadBtn_;

		wxPanel* CreateGamepadsPage(wxWindow* parent);
		wxBoxSizer* CreatePlayerSection(wxWindow* parent, int player_idx, int combo_id);
		void RefreshDeviceList();
		// True if the set of connected controllers changed since the last
		// RefreshDeviceList() call (compares m_devices against a fresh
		// enumeration). Used by OnDevicePollTimer() to avoid rebuilding the
		// comboboxes - and disturbing whatever the user is doing with them -
		// every single poll tick when nothing actually changed.
		bool DeviceListChanged() const;
		// Text to show in the "current source" column for one input, derived
		// from gamepadProfiles_ (not xOptions - see comment on that member).
		// Shared by CreatePlayerSection() (initial widget text) and
		// UpdateMappingLabels() (text after a capture or a reset), so both
		// paths always agree and initial widget sizing already accounts for
		// real content instead of a placeholder.
		wxString GetMappingLabelText(int player_idx, int input_idx) const;
		void UpdateMappingLabels(int player_idx);
		void StartCaptureMode(int player_idx, EEmulatedJoystickInput input);
		void StopCaptureMode();
		JoystickProfile LoadProfileFromOptions(int player_idx) const;
		void SaveProfileToOptions(int player_idx, const JoystickProfile& profile) const;
#endif
	};

}//namespace xPlatform

#endif//USE_WXWIDGETS

#endif//__WX_OPTIONS_DIALOG_H__
