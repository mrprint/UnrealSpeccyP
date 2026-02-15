#ifndef __WX_OPTIONS_DIALOG_H__
#define __WX_OPTIONS_DIALOG_H__

#pragma once

#ifdef USE_WXWIDGETS

#include <wx/dialog.h>
#include <wx/radiobut.h>
#include <wx/combobox.h>
#include <wx/checkbox.h>
#include <wx/stattext.h>
#include <wx/slider.h>
#include <wx/bmpbuttn.h>
#include <wx/artprov.h>

class wxWindow;
class wxCommandEvent;

namespace xPlatform {

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
	};

	//=============================================================================
	//	OptionsDialog
	//-----------------------------------------------------------------------------
	class OptionsDialog : public wxDialog
	{
	public:
		OptionsDialog(wxWindow* parent);
		virtual ~OptionsDialog() = default;

	private:
		void OnSoundChipChanged(wxCommandEvent& event);
		void OnStereoChanged(wxCommandEvent& event);
		void OnBetaDriveChanged(wxCommandEvent& event);
		void OnJoyTypeChanged(wxCommandEvent& event);
		void OnCheckboxChanged(wxCommandEvent& event);
		void OnSliderChanged(wxCommandEvent& event);
		void OnApply(wxCommandEvent& event);
		void OnOK(wxCommandEvent& event);

		// Reset button handlers
		void OnResetAudio(wxCommandEvent& event);
		void OnResetVideo(wxCommandEvent& event);
		void OnResetInput(wxCommandEvent& event);
		void OnResetDrive(wxCommandEvent& event);

		// Local state (not written to xOptions until OK/Apply)
		int sound_chip;
		int ay_stereo;
		eDrive beta_drive;
		eJoystick joy_type;

	private:
		DECLARE_EVENT_TABLE()

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

		// Reset buttons (one per section)
		wxBitmapButton* resetAudioBtn_;
		wxBitmapButton* resetVideoBtn_;
		wxBitmapButton* resetInputBtn_;
		wxBitmapButton* resetDriveBtn_;
	};

}//namespace xPlatform

#endif//USE_WXWIDGETS

#endif//__WX_OPTIONS_DIALOG_H__
