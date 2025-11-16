#include "../platform.h"
#ifdef USE_WXWIDGETS

#include "../../tools/options.h"
#include "../../options_common.h"
#include "wx_optionsdialog.h"

#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/button.h>

namespace xPlatform {

	BEGIN_EVENT_TABLE(OptionsDialog, wxDialog)
		EVT_RADIOBUTTON(ID_RADIO_SOUND, OptionsDialog::OnSoundChipChanged)
		EVT_COMBOBOX(wxID_ANY, OptionsDialog::OnStereoChanged)
		EVT_RADIOBUTTON(ID_RADIO_DRIVE, OptionsDialog::OnBetaDriveChanged)
		EVT_RADIOBUTTON(ID_RADIO_JOYSTICK, OptionsDialog::OnJoyTypeChanged)
		EVT_BUTTON(wxID_APPLY, OptionsDialog::OnApply)
		EVT_BUTTON(wxID_OK, OptionsDialog::OnOK)
	END_EVENT_TABLE()

		OptionsDialog::OptionsDialog(wxWindow* parent)
		: wxDialog(parent, wxID_ANY, _("Emulator Options"))
	{
		LoadCurrentSettings();

		// Main sizer
		wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL);
		const int padding = 10;

		// Sound Chip Group
		wxStaticBoxSizer* soundSizer = new wxStaticBoxSizer(wxVERTICAL, this, _("Sound Chip"));
		radioAy_ = new wxRadioButton(this, ID_RADIO_SOUND, _("AY-3-8910"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
		radioYm_ = new wxRadioButton(this, ID_RADIO_SOUND, _("YM2149F"), wxDefaultPosition, wxDefaultSize);
		soundSizer->Add(radioAy_, 0, wxALL, padding);
		soundSizer->Add(radioYm_, 0, wxALL, padding);
		mainSizer->Add(soundSizer, 0, wxEXPAND | wxLEFT | wxRIGHT, padding);

		// Stereo Mode Group
		wxStaticBoxSizer* stereoSizer = new wxStaticBoxSizer(wxVERTICAL, this, _("Stereo Mode"));
		comboStereo_ = new wxComboBox(this, wxID_ANY, wxEmptyString,
			wxDefaultPosition, wxDefaultSize,
			{ _("ABC"), _("ACB"), _("BAC"), _("BCA"), _("CAB"), _("CBA"), _("Mono") });
		stereoSizer->Add(comboStereo_, 0, wxALL, padding);
		mainSizer->Add(stereoSizer, 0, wxEXPAND | wxLEFT | wxRIGHT, padding);

		// Beta Disk Drives Group
		wxStaticBoxSizer* betaSizer = new wxStaticBoxSizer(wxHORIZONTAL, this, _("Beta Disk Drive"));
		radioA_ = new wxRadioButton(this, ID_RADIO_DRIVE, _("A"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
		radioB_ = new wxRadioButton(this, ID_RADIO_DRIVE, _("B"), wxDefaultPosition, wxDefaultSize);
		radioC_ = new wxRadioButton(this, ID_RADIO_DRIVE, _("C"), wxDefaultPosition, wxDefaultSize);
		radioD_ = new wxRadioButton(this, ID_RADIO_DRIVE, _("D"), wxDefaultPosition, wxDefaultSize);
		betaSizer->Add(radioA_, 0, wxALL, padding);
		betaSizer->Add(radioB_, 0, wxALL, padding);
		betaSizer->Add(radioC_, 0, wxALL, padding);
		betaSizer->Add(radioD_, 0, wxALL, padding);
		mainSizer->Add(betaSizer, 0, wxEXPAND | wxLEFT | wxRIGHT, padding);

		// Joystick Type Group
		wxStaticBoxSizer* joySizer = new wxStaticBoxSizer(wxVERTICAL, this, _("Joystick"));
		radioKempston_ = new wxRadioButton(this, ID_RADIO_JOYSTICK, _("Kempston"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
		radioCursor_ = new wxRadioButton(this, ID_RADIO_JOYSTICK, _("Cursor"), wxDefaultPosition, wxDefaultSize);
		radioQaoSpace_ = new wxRadioButton(this, ID_RADIO_JOYSTICK, _("QAOPSpace"), wxDefaultPosition, wxDefaultSize);
		radioSinclair2_ = new wxRadioButton(this, ID_RADIO_JOYSTICK, _("Sinclair 2"), wxDefaultPosition, wxDefaultSize);
		joySizer->Add(radioKempston_, 0, wxALL, padding);
		joySizer->Add(radioCursor_, 0, wxALL, padding);
		joySizer->Add(radioQaoSpace_, 0, wxALL, padding);
		joySizer->Add(radioSinclair2_, 0, wxALL, padding);
		mainSizer->Add(joySizer, 0, wxEXPAND | wxLEFT | wxRIGHT, padding);

		// Buttons (Apply/OK/Cancel)
		wxStdDialogButtonSizer* btnSizer = new wxStdDialogButtonSizer();
		btnSizer->Add(new wxButton(this, wxID_APPLY, _("Apply")));
		btnSizer->Add(new wxButton(this, wxID_OK, _("OK")));
		btnSizer->Add(new wxButton(this, wxID_CANCEL, _("Cancel")));
		btnSizer->Realize();
		mainSizer->Add(btnSizer, 0, wxALIGN_RIGHT | wxALL, padding);

		// Set dialog size and layout
		SetSizerAndFit(mainSizer);
		CenterOnParent();

		// Initialize controls with local state
		radioAy_->SetValue(sound_chip == 0);
		radioYm_->SetValue(sound_chip == 1);
		comboStereo_->SetSelection(ay_stereo);

		radioA_->SetValue(beta_drive == D_A);
		radioB_->SetValue(beta_drive == D_B);
		radioC_->SetValue(beta_drive == D_C);
		radioD_->SetValue(beta_drive == D_D);

		radioKempston_->SetValue(joy_type == J_KEMPSTON);
		radioCursor_->SetValue(joy_type == J_CURSOR);
		radioQaoSpace_->SetValue(joy_type == J_QAOPSPACE);
		radioSinclair2_->SetValue(joy_type == J_SINCLAIR2);
	}

	void OptionsDialog::LoadCurrentSettings()
	{
		// Sound Chip
		xOptions::eOption<int>* op_sound = xOptions::eOption<int>::Find("sound chip");
		if (op_sound) sound_chip = *op_sound;

		// Stereo Mode
		xOptions::eOption<int>* op_stereo = xOptions::eOption<int>::Find("ay stereo");
		if (op_stereo) ay_stereo = *op_stereo;

		// Beta Disk Drive
		beta_drive = OpDrive();

		// Joystick Type
		joy_type = OpJoystick();
	}

	void OptionsDialog::OnSoundChipChanged(wxCommandEvent& event)
	{
		sound_chip = radioAy_->GetValue() ? 0 : 1;
	}

	void OptionsDialog::OnStereoChanged(wxCommandEvent& event)
	{
		ay_stereo = comboStereo_->GetSelection();
	}

	void OptionsDialog::OnBetaDriveChanged(wxCommandEvent& event) {
		if (radioA_->GetValue()) beta_drive = D_A;
		else if (radioB_->GetValue()) beta_drive = D_B;
		else if (radioC_->GetValue()) beta_drive = D_C;
		else if (radioD_->GetValue()) beta_drive = D_D;
	}

	void OptionsDialog::OnJoyTypeChanged(wxCommandEvent& event) {
		if (radioKempston_->GetValue()) joy_type = J_KEMPSTON;
		else if (radioCursor_->GetValue()) joy_type = J_CURSOR;
		else if (radioQaoSpace_->GetValue()) joy_type = J_QAOPSPACE;
		else if (radioSinclair2_->GetValue()) joy_type = J_SINCLAIR2;
	}

	void OptionsDialog::OnApply(wxCommandEvent& event)
	{
		// Write local state to xOptions and apply
		xOptions::eOption<int>* op_sound = xOptions::eOption<int>::Find("sound chip");
		if (op_sound) { op_sound->Set(sound_chip); op_sound->Apply(); }

		xOptions::eOption<int>* op_stereo = xOptions::eOption<int>::Find("ay stereo");
		if (op_stereo) { op_stereo->Set(ay_stereo); op_stereo->Apply(); }

		xOptions::eOption<int>* op_drive = xOptions::eOption<int>::Find("drive");
		if (op_drive) { op_drive->Set(beta_drive); op_drive->Apply(); }

		xOptions::eOption<int>* op_joystick = xOptions::eOption<int>::Find("joystick");
		if (op_joystick) { op_joystick->Set(joy_type); op_joystick->Apply(); }

		//OpDrive(beta_drive); // Update beta disk drive
		//OpJoystick(joy_type); // Update joystick type
	}

	void OptionsDialog::OnOK(wxCommandEvent& event)
	{
		OnApply(event);
		EndModal(wxID_OK);
	}

} // namespace xPlatform

#endif//USE_WXWIDGETS
