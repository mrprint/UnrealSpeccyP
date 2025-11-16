#ifndef __WX_OPTIONS_DIALOG_H__
#define __WX_OPTIONS_DIALOG_H__

#pragma once

#ifdef USE_WXWIDGETS

#include <wx/dialog.h>
#include <wx/radiobut.h>
#include <wx/combobox.h>
#include <wx/checkbox.h>

class wxWindow;
class wxCommandEvent;

namespace xPlatform {

	enum DialogIDs {
		ID_RADIO_SOUND = 101,
		ID_RADIO_DRIVE,
		ID_RADIO_JOYSTICK,
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
		void OnApply(wxCommandEvent& event);
		void OnOK(wxCommandEvent& event);

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

		void LoadCurrentSettings();
	};

}//namespace xPlatform

#endif//USE_WXWIDGETS

#endif//__WX_OPTIONS_DIALOG_H__
