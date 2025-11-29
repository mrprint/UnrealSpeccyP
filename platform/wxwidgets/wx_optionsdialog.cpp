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
		EVT_CHECKBOX(ID_CHECK_GIGASCREEN, OptionsDialog::OnCheckboxChanged)
		EVT_CHECKBOX(ID_CHECK_SCANLINES, OptionsDialog::OnCheckboxChanged)
		EVT_CHECKBOX(ID_CHECK_PAL_EFFECTS, OptionsDialog::OnCheckboxChanged)
		EVT_CHECKBOX(ID_CHECK_DOT_CRAWL, OptionsDialog::OnCheckboxChanged)
		EVT_CHECKBOX(ID_CHECK_PHASE_MOD, OptionsDialog::OnCheckboxChanged)
		EVT_SLIDER(wxID_ANY, OptionsDialog::OnSliderChanged)
		EVT_BUTTON(wxID_APPLY, OptionsDialog::OnApply)
		EVT_BUTTON(wxID_OK, OptionsDialog::OnOK)
		// Add reset button events
		EVT_BUTTON(ID_RESET_AUDIO, OptionsDialog::OnResetAudio)
		EVT_BUTTON(ID_RESET_VIDEO, OptionsDialog::OnResetVideo)
		EVT_BUTTON(ID_RESET_INPUT, OptionsDialog::OnResetInput)
		EVT_BUTTON(ID_RESET_DRIVE, OptionsDialog::OnResetDrive)
	END_EVENT_TABLE()

	OptionsDialog::OptionsDialog(wxWindow* parent)
		: wxDialog(parent, wxID_ANY, _("Emulator Options"))
	{
		LoadCurrentSettings();

		// OUTER vertical layout (keeps buttons below everything)
		wxBoxSizer* outerSizer = new wxBoxSizer(wxVERTICAL);

		// MAIN horizontal layout for the 3 blocks
		wxBoxSizer* mainSizer = new wxBoxSizer(wxHORIZONTAL);
		const int padding = 10;

		// ======================================================
		//  AUDIO GROUP (with reset button)
		// ======================================================
		wxStaticBoxSizer* audioSizer = new wxStaticBoxSizer(wxVERTICAL, this, _("Audio"));
		wxWindow* audioBox = audioSizer->GetStaticBox();

		// Sound Chip Sizer
		wxStaticBoxSizer* soundSizer = new wxStaticBoxSizer(wxVERTICAL, audioBox, _("Sound Chip"));
		wxWindow* soundBox = soundSizer->GetStaticBox(); // Get static box for soundSizer
		radioAy_ = new wxRadioButton(soundBox, ID_RADIO_SOUND, _("AY-3-8910"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
		radioYm_ = new wxRadioButton(soundBox, ID_RADIO_SOUND, _("YM2149F"));
		soundSizer->Add(radioAy_, 0, wxALL, padding);
		soundSizer->Add(radioYm_, 0, wxALL, padding);
		audioSizer->Add(soundSizer, 0, wxEXPAND | wxALL, padding);

		// Stereo Mode Sizer
		wxStaticBoxSizer* stereoSizer = new wxStaticBoxSizer(wxVERTICAL, audioBox, _("Stereo Mode"));
		wxWindow* stereoBox = stereoSizer->GetStaticBox(); // Get static box for stereoSizer
		comboStereo_ = new wxComboBox(stereoBox, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
			{ _("ABC"), _("ACB"), _("BAC"), _("BCA"), _("CAB"), _("CBA"), _("Mono") });
		stereoSizer->Add(comboStereo_, 0, wxALL, padding);
		audioSizer->Add(stereoSizer, 0, wxEXPAND | wxALL, padding);

		// --- Reset Button (lower-right) ---
		wxBoxSizer* audioVaultSizer = new wxBoxSizer(wxHORIZONTAL);
		audioVaultSizer->AddStretchSpacer(1); // Push button to right
		resetAudioBtn_ = new wxBitmapButton(
			audioBox, ID_RESET_AUDIO,
			wxArtProvider::GetBitmap(wxART_UNDO, wxART_BUTTON), // Trash icon
			wxDefaultPosition, wxSize(16, 16) // Small size (16x16 pixels)
		);
		resetAudioBtn_->SetToolTip(_("Restore Audio Defaults"));
		audioVaultSizer->Add(resetAudioBtn_, 0, wxRIGHT | wxBOTTOM, padding / 2);

		// Add header to audio section at the end
		audioSizer->AddStretchSpacer();
		audioSizer->Add(audioVaultSizer, 0, wxEXPAND | wxBOTTOM | wxRIGHT, padding / 2);

		mainSizer->Add(audioSizer, 0, wxEXPAND | wxALL, padding);

		// ======================================================
		//  VIDEO GROUP (with reset button)
		// ======================================================
		wxStaticBoxSizer* videoSizer = new wxStaticBoxSizer(wxVERTICAL, this, _("Video"));

		// Video Sizer Direct Controls
		wxWindow* videoBox = videoSizer->GetStaticBox(); // Get static box for videoSizer
		checkGigascreen_ = new wxCheckBox(videoBox, ID_CHECK_GIGASCREEN, _("Enable Gigascreen"));
		videoSizer->Add(checkGigascreen_, 0, wxALL, padding);

		checkScanlines_ = new wxCheckBox(videoBox, ID_CHECK_SCANLINES, _("Enable CRT Scanlines"));
		videoSizer->Add(checkScanlines_, 0, wxALL, padding);

		// PAL Effects Sub-Sizer
		wxStaticBoxSizer* palSubSizer = new wxStaticBoxSizer(wxVERTICAL, videoBox, _("PAL Effects"));
		wxWindow* palBox = palSubSizer->GetStaticBox(); // Get static box for palSubSizer
		checkPalEffects_ = new wxCheckBox(palBox, ID_CHECK_PAL_EFFECTS, _("Enable PAL effects"));
		palSubSizer->Add(checkPalEffects_, 0, wxALL, padding);

		checkDotCrawl_ = new wxCheckBox(palBox, ID_CHECK_DOT_CRAWL, _("Enable Dot Crawl"));
		palSubSizer->Add(checkDotCrawl_, 0, wxLEFT | wxRIGHT | wxBOTTOM, padding);

		checkPhaseMod_ = new wxCheckBox(palBox, ID_CHECK_PHASE_MOD, _("Enable Phase Modulation"));
		palSubSizer->Add(checkPhaseMod_, 0, wxLEFT | wxRIGHT | wxBOTTOM, padding);

		palSubSizer->Add(new wxStaticText(palBox, wxID_ANY, _("PAL Strength")), 0, wxALL, padding / 2);
		sliderPalStrength_ = new wxSlider(palBox, wxID_ANY, pal_strength_val, 0, 100, wxDefaultPosition, wxSize(200, -1));
		labelPalStrength_ = new wxStaticText(palBox, wxID_ANY, wxString::Format(_("%d%%"), pal_strength_val));
		palSubSizer->Add(sliderPalStrength_, 0, wxALL, padding / 2);
		palSubSizer->Add(labelPalStrength_, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, padding);

		palSubSizer->Add(new wxStaticText(palBox, wxID_ANY, _("Beam Spread")), 0, wxALL, padding / 2);
		sliderBeamSpread_ = new wxSlider(palBox, wxID_ANY, beam_spread_val, 0, 200, wxDefaultPosition, wxSize(200, -1));
		labelBeamSpread_ = new wxStaticText(palBox, wxID_ANY, wxString::Format(_("%.1f"), (float)beam_spread_val / 100 * 2.0f));
		palSubSizer->Add(sliderBeamSpread_, 0, wxALL, padding / 2);
		palSubSizer->Add(labelBeamSpread_, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, padding);

		videoSizer->Add(palSubSizer, 0, wxEXPAND | wxALL, padding);

		// --- Reset Button (lower-right) ---
		wxBoxSizer* videoVaultSizer = new wxBoxSizer(wxHORIZONTAL);
		videoVaultSizer->AddStretchSpacer(1); // Push button to right
		resetVideoBtn_ = new wxBitmapButton(
			videoBox, ID_RESET_VIDEO,
			wxArtProvider::GetBitmap(wxART_UNDO, wxART_BUTTON), // Trash icon
			wxDefaultPosition, wxSize(16, 16) // Small size (16x16 pixels)
		);
		resetVideoBtn_->SetToolTip(_("Restore Video Defaults"));
		videoVaultSizer->Add(resetVideoBtn_, 0, wxRIGHT | wxBOTTOM, padding / 2);

		// Add header to video section at the end
		videoSizer->AddStretchSpacer();
		videoSizer->Add(videoVaultSizer, 0, wxEXPAND | wxBOTTOM | wxRIGHT, padding / 2);

		mainSizer->Add(videoSizer, 0, wxEXPAND | wxALL, padding);

		// ======================================================
		//  INPUT + STORAGE (stacked vertically)
		// ======================================================
		wxBoxSizer* rightColumnSizer = new wxBoxSizer(wxVERTICAL);

		// --- INPUT GROUP (with reset button) ---
		wxStaticBoxSizer* inputSizer = new wxStaticBoxSizer(wxVERTICAL, this, _("Input"));
		wxWindow* inputBox = inputSizer->GetStaticBox();

		// Joystick Type Sizer
		wxStaticBoxSizer* joySizer = new wxStaticBoxSizer(wxVERTICAL, inputBox, _("Joystick Type"));
		wxWindow* joyBox = joySizer->GetStaticBox(); // Get static box for joySizer
		radioKempston_ = new wxRadioButton(joyBox, ID_RADIO_JOYSTICK, _("Kempston"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
		radioCursor_ = new wxRadioButton(joyBox, ID_RADIO_JOYSTICK, _("Cursor"));
		radioQaoSpace_ = new wxRadioButton(joyBox, ID_RADIO_JOYSTICK, _("QAOPSpace"));
		radioSinclair2_ = new wxRadioButton(joyBox, ID_RADIO_JOYSTICK, _("Sinclair 2"));
		joySizer->Add(radioKempston_, 0, wxALL, padding);
		joySizer->Add(radioCursor_, 0, wxALL, padding);
		joySizer->Add(radioQaoSpace_, 0, wxALL, padding);
		joySizer->Add(radioSinclair2_, 0, wxALL, padding);
		inputSizer->Add(joySizer, 0, wxEXPAND | wxALL, padding);

		// --- Reset Button (lower-right) ---
		wxBoxSizer* inputVaultSizer = new wxBoxSizer(wxHORIZONTAL);
		inputVaultSizer->AddStretchSpacer(1); // Push button to right
		resetInputBtn_ = new wxBitmapButton(
			inputBox, ID_RESET_INPUT,
			wxArtProvider::GetBitmap(wxART_UNDO, wxART_BUTTON), // Trash icon
			wxDefaultPosition, wxSize(16, 16) // Small size (16x16 pixels)
		);
		resetInputBtn_->SetToolTip(_("Restore Input Defaults"));
		inputVaultSizer->Add(resetInputBtn_, 0, wxRIGHT | wxBOTTOM, padding / 2);

		// Add header to input section at the end
		inputSizer->AddStretchSpacer();
		inputSizer->Add(inputVaultSizer, 0, wxEXPAND | wxBOTTOM | wxRIGHT, padding / 2);
		rightColumnSizer->Add(inputSizer, 0, wxEXPAND | wxALL, padding);

		// --- STORAGE GROUP (with reset button) ---
		wxStaticBoxSizer* storageSizer = new wxStaticBoxSizer(wxVERTICAL, this, _("Disk Drives"));

		// Disk Drives Sizer
		wxWindow* driveBox = storageSizer->GetStaticBox(); // Get static box for storageSizer
		radioA_ = new wxRadioButton(driveBox, ID_RADIO_DRIVE, _("A"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
		radioB_ = new wxRadioButton(driveBox, ID_RADIO_DRIVE, _("B"));
		radioC_ = new wxRadioButton(driveBox, ID_RADIO_DRIVE, _("C"));
		radioD_ = new wxRadioButton(driveBox, ID_RADIO_DRIVE, _("D"));
		storageSizer->Add(radioA_, 0, wxALL, padding);
		storageSizer->Add(radioB_, 0, wxALL, padding);
		storageSizer->Add(radioC_, 0, wxALL, padding);
		storageSizer->Add(radioD_, 0, wxALL, padding);

		// --- Reset Button (lower-right) ---
		wxBoxSizer* driveVaultSizer = new wxBoxSizer(wxHORIZONTAL);
		driveVaultSizer->AddStretchSpacer(1); // Push button to right
		resetDriveBtn_ = new wxBitmapButton(
			driveBox, ID_RESET_DRIVE,
			wxArtProvider::GetBitmap(wxART_UNDO, wxART_BUTTON), // Trash icon
			wxDefaultPosition, wxSize(16, 16) // Small size (16x16 pixels)
		);
		resetDriveBtn_->SetToolTip(_("Restore Disk Drive Defaults"));
		driveVaultSizer->Add(resetDriveBtn_, 0, wxRIGHT | wxBOTTOM, padding / 2);

		// Add header to storage section at the end
		storageSizer->AddStretchSpacer();
		storageSizer->Add(driveVaultSizer, 0, wxEXPAND | wxBOTTOM | wxRIGHT, padding / 2);
		rightColumnSizer->Add(storageSizer, 0, wxEXPAND | wxALL, padding);

		mainSizer->Add(rightColumnSizer, 0, wxEXPAND | wxALL, 0);

		// Add horizontal section to vertical wrapper
		outerSizer->Add(mainSizer, 0, wxEXPAND | wxALL, padding);

		// ======================================================
		// BUTTONS
		// ======================================================
		wxStdDialogButtonSizer* btnSizer = new wxStdDialogButtonSizer();
		btnSizer->Add(new wxButton(this, wxID_APPLY, _("Apply")));
		btnSizer->Add(new wxButton(this, wxID_OK, _("OK")));
		btnSizer->Add(new wxButton(this, wxID_CANCEL, _("Cancel")));
		btnSizer->Realize();

		outerSizer->Add(btnSizer, 0, wxALIGN_RIGHT | wxALL, padding);

		SetSizerAndFit(outerSizer);
		CenterOnParent();

		ReflectSettings();
	}

	void OptionsDialog::ReflectSettings()
	{
		radioAy_->SetValue(sound_chip == SC_AY);
		radioYm_->SetValue(sound_chip == SC_YM);
		comboStereo_->SetSelection(ay_stereo);

		radioA_->SetValue(beta_drive == D_A);
		radioB_->SetValue(beta_drive == D_B);
		radioC_->SetValue(beta_drive == D_C);
		radioD_->SetValue(beta_drive == D_D);

		radioKempston_->SetValue(joy_type == J_KEMPSTON);
		radioCursor_->SetValue(joy_type == J_CURSOR);
		radioQaoSpace_->SetValue(joy_type == J_QAOPSPACE);
		radioSinclair2_->SetValue(joy_type == J_SINCLAIR2);

		checkPalEffects_->SetValue(pal_effects_enabled);
		checkDotCrawl_->SetValue(dot_crawl_enabled);
		checkPhaseMod_->SetValue(phase_mod_enabled);
		checkGigascreen_->SetValue(gigascreen_enabled);
		checkScanlines_->SetValue(scanlines_enabled);

		sliderPalStrength_->SetValue(pal_strength_val);
		labelPalStrength_->SetLabel(wxString::Format(_("%d%%"), pal_strength_val));

		sliderBeamSpread_->SetValue(beam_spread_val);
		float beam_val = static_cast<float>(beam_spread_val) / 100.0f * 2.0f;
		labelBeamSpread_->SetLabel(wxString::Format(_("%.1f"), beam_val));
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

		gigascreen_enabled = *xOptions::eOption<bool>::Find("gigascreen");
		scanlines_enabled = *xOptions::eOption<bool>::Find("scanlines");
		pal_effects_enabled = *xOptions::eOption<bool>::Find("pal effects");
		dot_crawl_enabled = *xOptions::eOption<bool>::Find("dot crawl");
		phase_mod_enabled = *xOptions::eOption<bool>::Find("phase modulation");
		pal_strength_val = *xOptions::eOption<int>::Find("pal strength");
		beam_spread_val = *xOptions::eOption<int>::Find("beam spread");
	}

	void OptionsDialog::OnSoundChipChanged(wxCommandEvent& event)
	{
		sound_chip = radioAy_->GetValue() ? SC_AY : SC_YM;
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

	void OptionsDialog::OnCheckboxChanged(wxCommandEvent& event) {
		switch (event.GetId()) {
		case ID_CHECK_GIGASCREEN:
			gigascreen_enabled = event.IsChecked();
			break;
		case ID_CHECK_SCANLINES:
			scanlines_enabled = event.IsChecked();
			break;
		case ID_CHECK_PAL_EFFECTS:
			pal_effects_enabled = event.IsChecked();
			break;
		case ID_CHECK_DOT_CRAWL:
			dot_crawl_enabled = event.IsChecked();
			break;
		case ID_CHECK_PHASE_MOD:
			phase_mod_enabled = event.IsChecked();
			break;
		default:
			// Handle unexpected IDs (e.g., log a warning)
			break;
		}
	}

	void OptionsDialog::OnSliderChanged(wxCommandEvent& event)
	{
		if (event.GetId() == sliderPalStrength_->GetId()) {
			pal_strength_val = sliderPalStrength_->GetValue();
			labelPalStrength_->SetLabel(wxString::Format(_("%d%%"), pal_strength_val));
		}
		else if (event.GetId() == sliderBeamSpread_->GetId()) {
			beam_spread_val = sliderBeamSpread_->GetValue();
			float val = static_cast<float>(beam_spread_val) / 100.0f * 2.0f;
			labelBeamSpread_->SetLabel(wxString::Format(_("%.1f"), val));
		}
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

		xOptions::eOption<bool>* op_gigascreen = xOptions::eOption<bool>::Find("gigascreen");
		if (op_gigascreen) { op_gigascreen->Set(gigascreen_enabled); op_gigascreen->Apply(); }

		xOptions::eOption<bool>* op_scanlines = xOptions::eOption<bool>::Find("scanlines");
		if (op_scanlines) { op_scanlines->Set(scanlines_enabled); op_scanlines->Apply(); }

		xOptions::eOption<bool>* op_pal_effects = xOptions::eOption<bool>::Find("pal effects");
		if (op_pal_effects) { op_pal_effects->Set(pal_effects_enabled); op_pal_effects->Apply(); }

		xOptions::eOption<bool>* op_dot_crawl = xOptions::eOption<bool>::Find("dot crawl");
		if (op_dot_crawl) { op_dot_crawl->Set(dot_crawl_enabled); op_dot_crawl->Apply(); }

		xOptions::eOption<bool>* op_phase_mod = xOptions::eOption<bool>::Find("phase modulation");
		if (op_phase_mod) { op_phase_mod->Set(phase_mod_enabled); op_phase_mod->Apply(); }

		xOptions::eOption<int>* op_pal_strength = xOptions::eOption<int>::Find("pal strength");
		if (op_pal_strength) { op_pal_strength->Set(pal_strength_val); op_pal_strength->Apply(); }

		xOptions::eOption<int>* op_beam_spread = xOptions::eOption<int>::Find("beam spread");
		if (op_beam_spread) { op_beam_spread->Set(beam_spread_val); op_beam_spread->Apply(); }
	}

	void OptionsDialog::OnOK(wxCommandEvent& event)
	{
		OnApply(event);
		EndModal(wxID_OK);
	}

	// Reset handlers implementation
	void OptionsDialog::OnResetAudio(wxCommandEvent& event) {
		sound_chip = DEFAULT_SOUND_CHIP;
		ay_stereo = DEFAULT_STEREO;
		ReflectSettings();
	}

	void OptionsDialog::OnResetVideo(wxCommandEvent& event) {
		// Reset video options to defaults
		gigascreen_enabled = DEFAULT_GIGASCREEN;
		scanlines_enabled = DEFAULT_SCANLINES;
		pal_effects_enabled = DEFAULT_PAL_EFFECTS;
		dot_crawl_enabled = DEFAULT_DOT_CRAWL;
		phase_mod_enabled = DEFAULT_PHASE_MODULATION;
		pal_strength_val = DEFAULT_PAL_STRENGTH;
		beam_spread_val = DEFAULT_BEAM_SPREAD;
		ReflectSettings();
	}

	void OptionsDialog::OnResetInput(wxCommandEvent& event) {
		// Reset joystick to default (Kempston, as it's the first in the group)
		joy_type = DEFAULT_JOYSTICK;
		ReflectSettings();
	}

	void OptionsDialog::OnResetDrive(wxCommandEvent& event) {
		// Reset drive to default
		beta_drive = DEFAULT_DRIVE;
		ReflectSettings();
	}

} // namespace xPlatform

#endif//USE_WXWIDGETS
