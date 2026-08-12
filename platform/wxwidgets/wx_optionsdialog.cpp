#include "../platform.h"
#ifdef USE_WXWIDGETS

#include "../../tools/options.h"
#include "../../options_common.h"
#include "wx_optionsdialog.h"
#ifdef USE_SDL2_GAMEPAD
#include "joystick_mapper.h"
#endif

#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/button.h>
#include <wx/panel.h>
#include <wx/listbook.h>
#include <wx/listctrl.h>

#ifdef USE_SDL2_GAMEPAD
#include <wx/timer.h>
#include <functional>
#include <utility>
#endif

namespace xPlatform {

	BEGIN_EVENT_TABLE(OptionsDialog, wxDialog)
		EVT_RADIOBUTTON(ID_RADIO_SOUND, OptionsDialog::OnSoundChipChanged)
		EVT_RADIOBUTTON(ID_RADIO_DRIVE, OptionsDialog::OnBetaDriveChanged)
		EVT_RADIOBUTTON(ID_RADIO_JOYSTICK, OptionsDialog::OnJoyTypeChanged)
		EVT_CHECKBOX(ID_CHECK_GIGASCREEN, OptionsDialog::OnCheckboxChanged)
		EVT_CHECKBOX(ID_CHECK_SCANLINES, OptionsDialog::OnCheckboxChanged)
		EVT_CHECKBOX(ID_CHECK_PAL_EFFECTS, OptionsDialog::OnCheckboxChanged)
		EVT_CHECKBOX(ID_CHECK_MIPMAPPING, OptionsDialog::OnCheckboxChanged)
		EVT_SLIDER(wxID_ANY, OptionsDialog::OnSliderChanged)

#ifdef USE_SDL2_GAMEPAD
		// Specific combobox IDs must be matched before the generic wxID_ANY
		// entry below (wx event tables are searched in declaration order).
		EVT_COMBOBOX(ID_DEVICE_COMBO_P1, OptionsDialog::OnDeviceSelected)
		EVT_COMBOBOX(ID_DEVICE_COMBO_P2, OptionsDialog::OnDeviceSelected)
#endif
		EVT_COMBOBOX(wxID_ANY, OptionsDialog::OnStereoChanged)

		EVT_BUTTON(wxID_OK, OptionsDialog::OnOK)
		// Reset buttons, one per tab
		EVT_BUTTON(ID_RESET_AUDIO, OptionsDialog::OnResetAudio)
		EVT_BUTTON(ID_RESET_VIDEO, OptionsDialog::OnResetVideo)
		EVT_BUTTON(ID_RESET_INPUT, OptionsDialog::OnResetInput)
		EVT_BUTTON(ID_RESET_DRIVE, OptionsDialog::OnResetDrive)
#ifdef USE_SDL2_GAMEPAD
		EVT_BUTTON(ID_RESET_GAMEPAD, OptionsDialog::OnResetGamepad)
		EVT_BUTTON(wxID_CANCEL, OptionsDialog::OnCancelBtn)

		// Gamepads tab: capture buttons for Player 1
		EVT_BUTTON(ID_CAPTURE_BTN_UP_P1, OptionsDialog::OnCaptureClick)
		EVT_BUTTON(ID_CAPTURE_BTN_DOWN_P1, OptionsDialog::OnCaptureClick)
		EVT_BUTTON(ID_CAPTURE_BTN_LEFT_P1, OptionsDialog::OnCaptureClick)
		EVT_BUTTON(ID_CAPTURE_BTN_RIGHT_P1, OptionsDialog::OnCaptureClick)
		EVT_BUTTON(ID_CAPTURE_BTN_FIRE1_P1, OptionsDialog::OnCaptureClick)
		EVT_BUTTON(ID_CAPTURE_BTN_FIRE2_P1, OptionsDialog::OnCaptureClick)

		// Gamepads tab: capture buttons for Player 2
		EVT_BUTTON(ID_CAPTURE_BTN_UP_P2, OptionsDialog::OnCaptureClick)
		EVT_BUTTON(ID_CAPTURE_BTN_DOWN_P2, OptionsDialog::OnCaptureClick)
		EVT_BUTTON(ID_CAPTURE_BTN_LEFT_P2, OptionsDialog::OnCaptureClick)
		EVT_BUTTON(ID_CAPTURE_BTN_RIGHT_P2, OptionsDialog::OnCaptureClick)
		EVT_BUTTON(ID_CAPTURE_BTN_FIRE1_P2, OptionsDialog::OnCaptureClick)
		EVT_BUTTON(ID_CAPTURE_BTN_FIRE2_P2, OptionsDialog::OnCaptureClick)

		EVT_TIMER(ID_TIMER_CAPTURE, OptionsDialog::OnTimer)
		EVT_TIMER(ID_TIMER_DEVICE_POLL, OptionsDialog::OnDevicePollTimer)
#endif
	END_EVENT_TABLE()

	namespace {
		// Small 16x16 "Restore Defaults" button, right-aligned at the bottom
		// of a tab page. Shared helper so every tab looks/behaves the same.
		wxBitmapButton* AddRestoreButton(wxWindow* box, wxSizer* pageSizer, int id, const wxString& tooltip, int padding)
		{
			wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
			row->AddStretchSpacer(1); // Push button to the right

			wxBitmapButton* btn = new wxBitmapButton(
				box, id,
				wxArtProvider::GetBitmap(wxART_UNDO, wxART_BUTTON),
				wxDefaultPosition, wxSize(16, 16));
			btn->SetToolTip(tooltip);
			row->Add(btn, 0, wxRIGHT | wxBOTTOM, padding / 2);

			pageSizer->AddStretchSpacer();
			pageSizer->Add(row, 0, wxEXPAND | wxBOTTOM | wxRIGHT, padding / 2);
			return btn;
		}
	}

	OptionsDialog::OptionsDialog(wxWindow* parent)
		: wxDialog(parent, wxID_ANY, _("Emulator Options"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
		, mipmap_enabled(DEFAULT_MIPMAPPING)
		, mask_scale_val(DEFAULT_MASK_SCALE)
	{
		LoadCurrentSettings();

#ifdef USE_SDL2_GAMEPAD
		// Seed the local (not-yet-applied) gamepad state from xOptions *before*
		// building the Gamepads tab's widgets, so CreatePlayerSection() can give
		// each "current source" label its real starting text (e.g. "DPAD_UP")
		// right away instead of a "Not set" placeholder that gets replaced by
		// SetLabel() after the page has already been laid out and Fit(). That
		// after-the-fact SetLabel() was the cause of the label/Capture-button
		// text overlap: the grid's column widths were computed and frozen
		// around the short placeholder text, then the real (longer) label
		// text was poured in without ever re-running layout.
		for (int i = 0; i < 2; ++i) {
			gamepadProfiles_[i] = LoadProfileFromOptions(i);
		}
#endif

		// OUTER vertical layout (keeps OK/Cancel below the tabs)
		wxBoxSizer* outerSizer = new wxBoxSizer(wxVERTICAL);
		const int padding = 10;

		// ======================================================
		//  TABS: Audio / Video / Input / Gamepads / Disk Drives
		//  wxListbook with wxLB_LEFT: tab strip down the left edge, with
		//  normal horizontal captions (see class comment in the header for
		//  why this isn't a plain wxNotebook).
		// ======================================================
		notebook_ = new wxListbook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLB_LEFT);

		wxPanel* audioPage = CreateAudioPage(notebook_);
		wxPanel* videoPage = CreateVideoPage(notebook_);
		wxPanel* inputPage = CreateInputPage(notebook_);
#ifdef USE_SDL2_GAMEPAD
		wxPanel* gamepadsPage = CreateGamepadsPage(notebook_);
#endif
		wxPanel* drivePage = CreateDrivePage(notebook_);

		// Give every page the same size: the largest requirement seen on
		// either axis across all of them. Without this, wxListbook sizes
		// itself to whichever page happens to be shown, so switching tabs -
		// or even just opening on a tab other than the biggest one - made
		// the dialog resize/reflow, and the window could end up narrower
		// than the widest tab needed (which is what was clipping the
		// Gamepads tab's labels and shrinking "Gamepads"/"Disk Drives" in
		// the tab list to "Gamepa…"/"Disk Driv…").
		wxSize maxPageSize = audioPage->GetBestSize();
		maxPageSize.IncTo(videoPage->GetBestSize());
		maxPageSize.IncTo(inputPage->GetBestSize());
#ifdef USE_SDL2_GAMEPAD
		maxPageSize.IncTo(gamepadsPage->GetBestSize());
#endif
		maxPageSize.IncTo(drivePage->GetBestSize());

		audioPage->SetMinSize(maxPageSize);
		videoPage->SetMinSize(maxPageSize);
		inputPage->SetMinSize(maxPageSize);
#ifdef USE_SDL2_GAMEPAD
		gamepadsPage->SetMinSize(maxPageSize);
#endif
		drivePage->SetMinSize(maxPageSize);

		notebook_->AddPage(audioPage, _("Audio"));
		notebook_->AddPage(videoPage, _("Video"));
		notebook_->AddPage(inputPage, _("Input"));
#ifdef USE_SDL2_GAMEPAD
		notebook_->AddPage(gamepadsPage, _("Gamepads"));
#endif
		notebook_->AddPage(drivePage, _("Disk Drives"));

		// The list-view tab strip on the left also defaults to a narrow
		// column that ellipsizes long captions ("Gamepads" -> "Gamepa…").
		// Size it explicitly to fit the longest caption in full.
		if (wxListView* tabList = notebook_->GetListView()) {
			int list_w = tabList->GetTextExtent(_("Disk Drives")).GetWidth() + 40;
			tabList->SetColumnWidth(0, list_w);
		}

		outerSizer->Add(notebook_, 1, wxEXPAND | wxALL, padding);

		// ======================================================
		// BUTTONS
		// ======================================================
		wxStdDialogButtonSizer* btnSizer = new wxStdDialogButtonSizer();

		btnSizer->Add(new wxButton(this, wxID_OK, _("OK")));
		btnSizer->Add(new wxButton(this, wxID_CANCEL, _("Cancel")));
		btnSizer->Realize();

		outerSizer->Add(btnSizer, 0, wxALIGN_RIGHT | wxALL, padding);

		SetSizerAndFit(outerSizer);
		CenterOnParent();

		ReflectSettings();

#ifdef USE_SDL2_GAMEPAD
		// Timer for input capture mode
		m_capture_timer = new wxTimer(this, ID_TIMER_CAPTURE);

		RefreshDeviceList();

		// Display saved mappings when opening the dialog
		UpdateMappingLabels(0);
		UpdateMappingLabels(1);

		// Poll for controllers being plugged in/unplugged while the dialog
		// is open, so the device list updates live instead of only ever
		// being refreshed once at construction (which meant the dialog had
		// to be closed and reopened to see a newly connected controller).
		m_device_poll_timer = new wxTimer(this, ID_TIMER_DEVICE_POLL);
		m_device_poll_timer->Start(1000);
#endif
	}

#ifdef USE_SDL2_GAMEPAD
	OptionsDialog::~OptionsDialog() {
		if (m_capture_timer) {
			m_capture_timer->Stop();
			delete m_capture_timer;
		}
		if (m_device_poll_timer) {
			m_device_poll_timer->Stop();
			delete m_device_poll_timer;
		}
	}
#else
	OptionsDialog::~OptionsDialog() = default;
#endif

	// ======================================================
	//  AUDIO TAB (with reset button)
	// ======================================================
	wxPanel* OptionsDialog::CreateAudioPage(wxWindow* parent)
	{
		const int padding = 10;
		wxPanel* page = new wxPanel(parent);
		wxBoxSizer* pageSizer = new wxBoxSizer(wxVERTICAL);

		// Sound Chip Sizer
		wxStaticBoxSizer* soundSizer = new wxStaticBoxSizer(wxVERTICAL, page, _("Sound Chip"));
		wxWindow* soundBox = soundSizer->GetStaticBox();
		radioAy_ = new wxRadioButton(soundBox, ID_RADIO_SOUND, _("AY-3-8910"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
		radioYm_ = new wxRadioButton(soundBox, ID_RADIO_SOUND, _("YM2149F"));
		soundSizer->Add(radioAy_, 0, wxALL, padding);
		soundSizer->Add(radioYm_, 0, wxALL, padding);
		pageSizer->Add(soundSizer, 0, wxEXPAND | wxALL, padding);

		// Stereo Mode Sizer
		wxStaticBoxSizer* stereoSizer = new wxStaticBoxSizer(wxVERTICAL, page, _("Stereo Mode"));
		wxWindow* stereoBox = stereoSizer->GetStaticBox();
		comboStereo_ = new wxComboBox(stereoBox, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
			{ _("ABC"), _("ACB"), _("BAC"), _("BCA"), _("CAB"), _("CBA"), _("Mono") });
		stereoSizer->Add(comboStereo_, 0, wxALL, padding);
		pageSizer->Add(stereoSizer, 0, wxEXPAND | wxALL, padding);

		resetAudioBtn_ = AddRestoreButton(page, pageSizer, ID_RESET_AUDIO, _("Restore Audio Defaults"), padding);

		page->SetSizerAndFit(pageSizer);
		return page;
	}

	// ======================================================
	//  VIDEO TAB (with reset button)
	// ======================================================
	wxPanel* OptionsDialog::CreateVideoPage(wxWindow* parent)
	{
		const int padding = 8;
		wxPanel* page = new wxPanel(parent);
		wxBoxSizer* pageSizer = new wxBoxSizer(wxVERTICAL);

		checkMipmapping_ = new wxCheckBox(page, ID_CHECK_MIPMAPPING, _("Enable Mipmapping"));
		pageSizer->Add(checkMipmapping_, 0, wxALL, padding);

		checkGigascreen_ = new wxCheckBox(page, ID_CHECK_GIGASCREEN, _("Enable Gigascreen"));
		pageSizer->Add(checkGigascreen_, 0, wxALL, padding);

		checkScanlines_ = new wxCheckBox(page, ID_CHECK_SCANLINES, _("Enable CRT Scanlines"));
		pageSizer->Add(checkScanlines_, 0, wxALL, padding);

		// Slider + its live value share one row instead of three stacked
		// rows (label / slider / value) - this tab was the tallest of the
		// five, and since every tab is sized to match the tallest one (see
		// the constructor), that height was forced onto the whole dialog,
		// leaving the shorter tabs mostly empty. A fixed-width value label
		// also keeps the row from jittering horizontally as the number's
		// width changes (e.g. "5%" vs "100%").
		pageSizer->Add(new wxStaticText(page, wxID_ANY, _("CRT Mask Scale")), 0, wxLEFT | wxRIGHT | wxTOP, padding);
		wxBoxSizer* maskRow = new wxBoxSizer(wxHORIZONTAL);
		sliderMaskScale_ = new wxSlider(page, wxID_ANY, DEFAULT_MASK_SCALE, 0, 4, wxDefaultPosition, wxSize(220, -1));
		labelMaskScale_ = new wxStaticText(page, wxID_ANY, _("1"), wxDefaultPosition, wxSize(30, -1));
		maskRow->Add(sliderMaskScale_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, padding / 2);
		maskRow->Add(labelMaskScale_, 0, wxALIGN_CENTER_VERTICAL);
		pageSizer->Add(maskRow, 0, wxALL, padding);

		// PAL Effects Sub-Sizer
		wxStaticBoxSizer* palSubSizer = new wxStaticBoxSizer(wxVERTICAL, page, _("PAL Effects"));
		wxWindow* palBox = palSubSizer->GetStaticBox();
		checkPalEffects_ = new wxCheckBox(palBox, ID_CHECK_PAL_EFFECTS, _("Enable PAL effects"));
		palSubSizer->Add(checkPalEffects_, 0, wxALL, padding);

		palSubSizer->Add(new wxStaticText(palBox, wxID_ANY, _("PAL Strength")), 0, wxLEFT | wxRIGHT | wxTOP, padding);
		wxBoxSizer* palStrengthRow = new wxBoxSizer(wxHORIZONTAL);
		sliderPalStrength_ = new wxSlider(palBox, wxID_ANY, pal_strength_val, 0, 100, wxDefaultPosition, wxSize(180, -1));
		labelPalStrength_ = new wxStaticText(palBox, wxID_ANY, wxString::Format(_("%d%%"), pal_strength_val), wxDefaultPosition, wxSize(45, -1));
		palStrengthRow->Add(sliderPalStrength_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, padding / 2);
		palStrengthRow->Add(labelPalStrength_, 0, wxALIGN_CENTER_VERTICAL);
		palSubSizer->Add(palStrengthRow, 0, wxALL, padding);

		palSubSizer->Add(new wxStaticText(palBox, wxID_ANY, _("Beam Spread")), 0, wxLEFT | wxRIGHT | wxTOP, padding);
		wxBoxSizer* beamSpreadRow = new wxBoxSizer(wxHORIZONTAL);
		sliderBeamSpread_ = new wxSlider(palBox, wxID_ANY, beam_spread_val, 0, 200, wxDefaultPosition, wxSize(180, -1));
		labelBeamSpread_ = new wxStaticText(palBox, wxID_ANY, wxString::Format(_("%.1f"), (float)beam_spread_val / 100 * 2.0f), wxDefaultPosition, wxSize(45, -1));
		beamSpreadRow->Add(sliderBeamSpread_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, padding / 2);
		beamSpreadRow->Add(labelBeamSpread_, 0, wxALIGN_CENTER_VERTICAL);
		palSubSizer->Add(beamSpreadRow, 0, wxALL, padding);

		pageSizer->Add(palSubSizer, 0, wxEXPAND | wxALL, padding);

		resetVideoBtn_ = AddRestoreButton(page, pageSizer, ID_RESET_VIDEO, _("Restore Video Defaults"), padding);

		page->SetSizerAndFit(pageSizer);
		return page;
	}

	// ======================================================
	//  INPUT TAB (with reset button)
	// ======================================================
	wxPanel* OptionsDialog::CreateInputPage(wxWindow* parent)
	{
		const int padding = 10;
		wxPanel* page = new wxPanel(parent);
		wxBoxSizer* pageSizer = new wxBoxSizer(wxVERTICAL);

		// Joystick Type Sizer
		wxStaticBoxSizer* joySizer = new wxStaticBoxSizer(wxVERTICAL, page, _("Joystick Type"));
		wxWindow* joyBox = joySizer->GetStaticBox();
		radioKempston_ = new wxRadioButton(joyBox, ID_RADIO_JOYSTICK, _("Kempston"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
		radioCursor_ = new wxRadioButton(joyBox, ID_RADIO_JOYSTICK, _("Cursor"));
		radioQaoSpace_ = new wxRadioButton(joyBox, ID_RADIO_JOYSTICK, _("QAOPSpace"));
		radioSinclair2_ = new wxRadioButton(joyBox, ID_RADIO_JOYSTICK, _("Sinclair 2"));
		joySizer->Add(radioKempston_, 0, wxALL, padding);
		joySizer->Add(radioCursor_, 0, wxALL, padding);
		joySizer->Add(radioQaoSpace_, 0, wxALL, padding);
		joySizer->Add(radioSinclair2_, 0, wxALL, padding);
		pageSizer->Add(joySizer, 0, wxEXPAND | wxALL, padding);

		// NOTE: gamepad configuration used to be launched from a button here
		// ("Configure Gamepads..."); it now lives in its own "Gamepads" tab.

		resetInputBtn_ = AddRestoreButton(page, pageSizer, ID_RESET_INPUT, _("Restore Input Defaults"), padding);

		page->SetSizerAndFit(pageSizer);
		return page;
	}

	// ======================================================
	//  DISK DRIVES TAB (with reset button)
	// ======================================================
	wxPanel* OptionsDialog::CreateDrivePage(wxWindow* parent)
	{
		const int padding = 10;
		wxPanel* page = new wxPanel(parent);
		wxBoxSizer* pageSizer = new wxBoxSizer(wxVERTICAL);

		radioA_ = new wxRadioButton(page, ID_RADIO_DRIVE, _("A"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
		radioB_ = new wxRadioButton(page, ID_RADIO_DRIVE, _("B"));
		radioC_ = new wxRadioButton(page, ID_RADIO_DRIVE, _("C"));
		radioD_ = new wxRadioButton(page, ID_RADIO_DRIVE, _("D"));
		pageSizer->Add(radioA_, 0, wxALL, padding);
		pageSizer->Add(radioB_, 0, wxALL, padding);
		pageSizer->Add(radioC_, 0, wxALL, padding);
		pageSizer->Add(radioD_, 0, wxALL, padding);

		resetDriveBtn_ = AddRestoreButton(page, pageSizer, ID_RESET_DRIVE, _("Restore Disk Drive Defaults"), padding);

		page->SetSizerAndFit(pageSizer);
		return page;
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
		checkGigascreen_->SetValue(gigascreen_enabled);
		checkScanlines_->SetValue(scanlines_enabled);

		sliderPalStrength_->SetValue(pal_strength_val);
		labelPalStrength_->SetLabel(wxString::Format(_("%d%%"), pal_strength_val));

		sliderBeamSpread_->SetValue(beam_spread_val);
		float beam_val = static_cast<float>(beam_spread_val) / 100.0f * 2.0f;
		labelBeamSpread_->SetLabel(wxString::Format(_("%.1f"), beam_val));

		checkMipmapping_->SetValue(mipmap_enabled);
		sliderMaskScale_->SetValue(mask_scale_val);

		wxString label;
		label.Printf(wxT("%d"), mask_scale_val);
		labelMaskScale_->SetLabel(label);
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
		pal_strength_val = *xOptions::eOption<int>::Find("pal strength");
		beam_spread_val = *xOptions::eOption<int>::Find("beam spread");

		// Mipmapping option
		xOptions::eOption<bool>* op_mipmap = xOptions::eOption<bool>::Find("mipmapping");
		mipmap_enabled = op_mipmap ? *op_mipmap : DEFAULT_MIPMAPPING;

		// Mask scale option
		xOptions::eOption<int>* op_mask = xOptions::eOption<int>::Find("mask scale");
		mask_scale_val = op_mask ? *op_mask : DEFAULT_MASK_SCALE;
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
		case ID_CHECK_MIPMAPPING:
			mipmap_enabled = event.IsChecked();
			break;
		default:
			// Handle unexpected IDs (e.g., log a warning)
			break;
		}
	}

	void OptionsDialog::OnSliderChanged(wxCommandEvent& event)
	{
		if (event.GetId() == sliderMaskScale_->GetId()) {
			mask_scale_val = sliderMaskScale_->GetValue();
			labelMaskScale_->SetLabel(wxString::Format(_("%d"), mask_scale_val));
		}
		else if (event.GetId() == sliderPalStrength_->GetId()) {
			pal_strength_val = sliderPalStrength_->GetValue();
			labelPalStrength_->SetLabel(wxString::Format(_("%d%%"), pal_strength_val));
		}
		else if (event.GetId() == sliderBeamSpread_->GetId()) {
			beam_spread_val = sliderBeamSpread_->GetValue();
			float val = static_cast<float>(beam_spread_val) / 100.0f * 2.0f;
			labelBeamSpread_->SetLabel(wxString::Format(_("%.1f"), val));
		}
	}

	void OptionsDialog::OnOK(wxCommandEvent& event)
	{
		// ScopedRenderPause is intentionally absent here.
		// Frame::OnOptions() holds a ScopedRenderPause for the entire lifetime
		// of this dialog.  MaybePause() blocks the render thread completely —
		// including OnLoop() — so the emulator is effectively paused while the
		// dialog is open.  Taking a second ScopedRenderPause would deadlock:
		// Pause() waits for m_idle, but MaybePause() is already blocked in
		// m_cv_thread.wait() and will never set m_idle again.
		//
		// ScopedEmuLock is kept for defensive correctness: it costs nothing
		// (m_emu_mutex is uncontested while the render thread is in MaybePause),
		// but makes OnOK() safe if the threading model changes in the future.
		ScopedEmuLock emu_guard;

#ifdef USE_SDL2_GAMEPAD
		if (m_capturing_player >= 0) {
			StopCaptureMode();
		}
#endif

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

		xOptions::eOption<int>* op_pal_strength = xOptions::eOption<int>::Find("pal strength");
		if (op_pal_strength) { op_pal_strength->Set(pal_strength_val); op_pal_strength->Apply(); }

		xOptions::eOption<int>* op_beam_spread = xOptions::eOption<int>::Find("beam spread");
		if (op_beam_spread) { op_beam_spread->Set(beam_spread_val); op_beam_spread->Apply(); }

		xOptions::eOption<bool>* op_mipmap = xOptions::eOption<bool>::Find("mipmapping");
		if (op_mipmap) { op_mipmap->Set(mipmap_enabled); op_mipmap->Apply();}

		xOptions::eOption<int>* op_mask = xOptions::eOption<int>::Find("mask scale");
		if (op_mask) { op_mask->Set(mask_scale_val); op_mask->Apply(); }

#ifdef USE_SDL2_GAMEPAD
		// Write the Gamepads tab's buffered device/mapping selections to
		// xOptions now, same as every other tab.
		for (int i = 0; i < 2; ++i) {
			SaveProfileToOptions(i, gamepadProfiles_[i]);
		}
		for (int i = 0; i < 2; ++i) {
			int dev_idx = gamepadProfiles_[i].host_device_index;
			if (dev_idx >= 0) {
				GamepadBackend().RefreshDeviceState(dev_idx);
			}
		}
		// GLCanvas caches its own copy of these profiles (m_profiles) and
		// only ever read it from xOptions at construction time, so without
		// this the emulator kept using the *previous* device/mapping until
		// restarted, even though xOptions itself was already up to date.
		ReloadGamepadProfiles();
#endif

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
		pal_strength_val = DEFAULT_PAL_STRENGTH;
		beam_spread_val = DEFAULT_BEAM_SPREAD;
		mipmap_enabled = DEFAULT_MIPMAPPING;
		mask_scale_val = DEFAULT_MASK_SCALE;
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

#ifdef USE_SDL2_GAMEPAD
	// ======================================================
	//  GAMEPADS TAB (formerly the separate "Configure Gamepads" dialog)
	// ======================================================
	wxPanel* OptionsDialog::CreateGamepadsPage(wxWindow* parent)
	{
		const int padding = 10;
		wxPanel* page = new wxPanel(parent);
		wxBoxSizer* pageSizer = new wxBoxSizer(wxVERTICAL);

		// Player 1 / Player 2 side by side rather than stacked: stacking two
		// 6-row sections vertically made the Gamepads tab roughly twice as
		// tall as it needed to be - and since every tab is sized to match
		// the tallest one (see the constructor), that height was forced onto
		// the whole dialog, not just this tab.
		wxBoxSizer* playersRow = new wxBoxSizer(wxHORIZONTAL);

		wxBoxSizer* p1_section = CreatePlayerSection(page, 0, ID_DEVICE_COMBO_P1);
		playersRow->Add(p1_section, 1, wxEXPAND | wxALL, 5);

		wxBoxSizer* p2_section = CreatePlayerSection(page, 1, ID_DEVICE_COMBO_P2);
		playersRow->Add(p2_section, 1, wxEXPAND | wxALL, 5);

		pageSizer->Add(playersRow, 1, wxEXPAND | wxALL, 5);

		resetGamepadBtn_ = AddRestoreButton(page, pageSizer, ID_RESET_GAMEPAD, _("Restore Gamepad Defaults"), padding);

		page->SetSizerAndFit(pageSizer);
		return page;
	}

	wxBoxSizer* OptionsDialog::CreatePlayerSection(wxWindow* parent, int player_idx, int combo_id) {
		auto* section = new wxStaticBoxSizer(wxVERTICAL, parent,
			wxString::Format(wxT("Player %d"), player_idx + 1));
		wxWindow* box = section->GetStaticBox();

		// Device selection combobox
		m_players[player_idx].device_combo = new wxComboBox(box, combo_id);
		section->Add(m_players[player_idx].device_combo, 0, wxEXPAND | wxALL, 5);

		// Mapping table
		auto* grid = new wxFlexGridSizer(6, 3, 5, 10);
		grid->AddGrowableCol(1, 1); // extra width goes to the "current source" column

		static const char* input_names[] = {"UP", "DOWN", "LEFT", "RIGHT", "FIRE1", "FIRE2"};
		int base_btn_id;

		if (player_idx == 0) {
			base_btn_id = ID_CAPTURE_BTN_UP_P1;
		} else {
			base_btn_id = ID_CAPTURE_BTN_UP_P2;
		}

		// Reserve enough width up front for the longest possible source name
		// ("LEFT_STICK_RIGHT"/"RIGHT_STICK_RIGHT", see SourceTypeToString())
		// and for the longest button caption ("Capturing..."), so that later
		// SetLabel() calls from UpdateMappingLabels()/StartCaptureMode()
		// never grow past the column width and overlap their neighbour -
		// which is what produced the garbled "DPAD_DOWNpture" text.
		const int label_min_w = box->GetTextExtent(wxT("RIGHT_STICK_RIGHT")).GetWidth() + 10;
		const int btn_min_w = box->GetTextExtent(wxT("Capturing...")).GetWidth() + 24;

		for (int i = 0; i < 6; ++i) {
			grid->Add(new wxStaticText(box, wxID_ANY, wxString::FromUTF8(input_names[i])), 0, wxALIGN_CENTER_VERTICAL);

			// Current source label — stretches to cell width, but
			// cannot become narrower than label_min_w (see comment above)
			wxStaticText* label = new wxStaticText(box, wxID_ANY, GetMappingLabelText(player_idx, i));
			label->SetMinSize(wxSize(label_min_w, -1));
			m_players[player_idx].source_labels[i] = label;
			grid->Add(label, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);

			// Capture button
			wxButton* btn = new wxButton(box, base_btn_id + i, wxString::FromUTF8("Capture"));
			btn->SetMinSize(wxSize(btn_min_w, -1));
			m_players[player_idx].capture_buttons[i] = btn;
			grid->Add(btn);
		}

		section->Add(grid, 1, wxEXPAND | wxALL, 5);
		return section;
	}

	void OptionsDialog::RefreshDeviceList() {
		// EnumerateDevices() returns SDL_IsGameController() devices only, so its
		// .index values (real SDL joystick indices) can have gaps and are NOT
		// the same as the device's position in the returned vector. Cache the
		// list so combobox positions can always be translated back to a real
		// device index (see OnDeviceSelected).
		m_devices = GamepadBackend().EnumerateDevices();

		for (int player = 0; player < 2; ++player) {
			// Clear()+Append() below rebuilds the combobox from scratch, which
			// would blow away whatever the user was mid-typing/selecting; only
			// called at startup and when OnDevicePollTimer() notices the
			// actual set of connected controllers changed, not on every poll.
			m_players[player].device_combo->Clear();
			m_players[player].device_combo->Append(wxString::FromUTF8("None"));

			for (const auto& dev : m_devices) {
				m_players[player].device_combo->Append(dev.name);
			}

			// Identify "this player's device" by GUID, not by the numeric
			// index: SDL reassigns indices as controllers are plugged and
			// unplugged, so a stale index can silently point at the wrong -
			// or no - controller once the connected set changes. The GUID
			// is stable for a given controller model across reconnects.
			int selection = 0; // "None"
			const std::string& guid = gamepadProfiles_[player].device_guid;
			if (!guid.empty()) {
				for (size_t i = 0; i < m_devices.size(); ++i) {
					if (m_devices[i].guid == guid) {
						selection = static_cast<int>(i) + 1;
						break;
					}
				}
			}

			if (selection > 0) {
				int device_index = m_devices[selection - 1].index;
				gamepadProfiles_[player].host_device_index = device_index;
				GamepadBackend().RefreshDeviceState(device_index);
			} else {
				// Either no device assigned at all, or the assigned GUID
				// isn't among the currently connected devices - nothing to
				// talk to right now. The GUID itself (if any) is left
				// untouched so the device re-binds automatically once it's
				// plugged back in (see OnDevicePollTimer).
				gamepadProfiles_[player].host_device_index = -1;
			}

			m_players[player].device_combo->SetSelection(selection);
		}
	}

	bool OptionsDialog::DeviceListChanged() const {
		auto current = GamepadBackend().EnumerateDevices();
		if (current.size() != m_devices.size()) return true;
		for (size_t i = 0; i < current.size(); ++i) {
			if (current[i].index != m_devices[i].index || current[i].name != m_devices[i].name) {
				return true;
			}
		}
		return false;
	}

	wxString OptionsDialog::GetMappingLabelText(int player_idx, int input_idx) const {
		static const EEmulatedJoystickInput inputs[] = {
			EEmulatedJoystickInput::UP,
			EEmulatedJoystickInput::DOWN,
			EEmulatedJoystickInput::LEFT,
			EEmulatedJoystickInput::RIGHT,
			EEmulatedJoystickInput::FIRE1,
			EEmulatedJoystickInput::FIRE2
		};

		const auto& profile = gamepadProfiles_[player_idx];
		auto it = profile.input_map.find(inputs[input_idx]);
		if (it != profile.input_map.end()) {
			return wxString(SourceTypeToString(it->second.source_type));
		}
		return wxString::FromUTF8("Not set");
	}

	void OptionsDialog::UpdateMappingLabels(int player_idx) {
		for (int i = 0; i < 6; ++i) {
			m_players[player_idx].source_labels[i]->SetLabel(GetMappingLabelText(player_idx, i));
		}
	}

	void OptionsDialog::OnDeviceSelected(wxCommandEvent& event) {
		int combo_id = event.GetId();
		int player_idx = (combo_id == ID_DEVICE_COMBO_P1) ? 0 : 1;

		wxComboBox* combo = m_players[player_idx].device_combo;
		int selection = combo->GetSelection();

		if (selection <= 0 || selection > static_cast<int>(m_devices.size())) {
			gamepadProfiles_[player_idx].host_device_index = -1;
			gamepadProfiles_[player_idx].device_guid.clear();
		} else {
			// Translate combobox position back to the real SDL device index via
			// the cached device list (position and index can differ - see
			// RefreshDeviceList).
			int device_index = m_devices[selection - 1].index;
			gamepadProfiles_[player_idx].host_device_index = device_index;
			// The GUID, not the index, is what actually gets persisted as
			// this player's device identity (see SaveProfileToOptions /
			// ResolveDeviceIndexForGuid) - the index alone wouldn't survive
			// a reconnect or restart.
			gamepadProfiles_[player_idx].device_guid = m_devices[selection - 1].guid;
			// Just wakes the device up so its live state is fresh for
			// capture below; doesn't touch xOptions (that happens on OK,
			// like every other tab).
			GamepadBackend().RefreshDeviceState(device_index);
		}
	}

	void OptionsDialog::OnCaptureClick(wxCommandEvent& event) {
		int btn_id = event.GetId();

		int player_idx, input_idx;

		if (btn_id >= ID_CAPTURE_BTN_UP_P1 && btn_id <= ID_CAPTURE_BTN_FIRE2_P1) {
			player_idx = 0;
			input_idx = btn_id - ID_CAPTURE_BTN_UP_P1;
		} else {
			player_idx = 1;
			input_idx = btn_id - ID_CAPTURE_BTN_UP_P2;
		}

		static const EEmulatedJoystickInput inputs[] = {
			EEmulatedJoystickInput::UP,
			EEmulatedJoystickInput::DOWN,
			EEmulatedJoystickInput::LEFT,
			EEmulatedJoystickInput::RIGHT,
			EEmulatedJoystickInput::FIRE1,
			EEmulatedJoystickInput::FIRE2
		};

		StartCaptureMode(player_idx, inputs[input_idx]);
	}

	void OptionsDialog::StartCaptureMode(int player_idx, EEmulatedJoystickInput input) {
		// Starting a new capture while a previous one is still pending (user
		// clicked another Capture button without waiting for the first to
		// finish) must clean up the old button first - otherwise it's left
		// permanently stuck showing "Capturing..." and disabled, since
		// m_capturing_player/m_capturing_input get overwritten below and
		// nothing else would ever reset it.
		if (m_capturing_player >= 0) {
			StopCaptureMode();
		}

		m_capturing_player = player_idx;
		m_capturing_input = input;

		int btn_idx = static_cast<int>(input);
		wxButton* btn = m_players[player_idx].capture_buttons[btn_idx];
		btn->SetLabel(wxString::FromUTF8("Capturing..."));
		btn->Disable();

		if (!m_capture_timer->IsRunning()) {
			m_capture_timer->Start(50);
		}
	}

	void OptionsDialog::StopCaptureMode() {
		if (m_capturing_player >= 0) {
			int btn_idx = static_cast<int>(m_capturing_input);
			wxButton* btn = m_players[m_capturing_player].capture_buttons[btn_idx];
			btn->SetLabel(wxString::FromUTF8("Capture"));
			btn->Enable();
		}

		if (m_capture_timer->IsRunning()) {
			m_capture_timer->Stop();
		}

		m_capturing_player = -1;
	}

	void OptionsDialog::OnTimer(wxTimerEvent& event) {
		auto devices = GamepadBackend().EnumerateDevices();

		static const std::vector<std::pair<EHostSourceType, std::function<bool(const GamepadState&)>>> checks = {
			{EHostSourceType::BUTTON_A,            [](const GamepadState& s) { return s.a; }},
			{EHostSourceType::BUTTON_B,            [](const GamepadState& s) { return s.b; }},
			{EHostSourceType::BUTTON_X,            [](const GamepadState& s) { return s.x; }},
			{EHostSourceType::BUTTON_Y,            [](const GamepadState& s) { return s.y; }},
			{EHostSourceType::BUTTON_BACK,         [](const GamepadState& s) { return s.back; }},
			{EHostSourceType::BUTTON_START,        [](const GamepadState& s) { return s.start; }},
			{EHostSourceType::BUTTON_LEFTSTICK,    [](const GamepadState& s) { return s.leftstick; }},
			{EHostSourceType::BUTTON_RIGHTSTICK,   [](const GamepadState& s) { return s.rightstick; }},
			{EHostSourceType::BUTTON_LEFTSHOULDER, [](const GamepadState& s) { return s.leftshoulder; }},
			{EHostSourceType::BUTTON_RIGHTSHOULDER,[](const GamepadState& s) { return s.rightshoulder; }},
			{EHostSourceType::HAT_UP,              [](const GamepadState& s) { return s.IsHatUp(); }},
			{EHostSourceType::HAT_DOWN,            [](const GamepadState& s) { return s.IsHatDown(); }},
			{EHostSourceType::HAT_LEFT,            [](const GamepadState& s) { return s.IsHatLeft(); }},
			{EHostSourceType::HAT_RIGHT,           [](const GamepadState& s) { return s.IsHatRight(); }},
			{EHostSourceType::AXIS_LEFT_X_POS,     [](const GamepadState& s) { return s.GetAxisWithDeadzone(s.leftX) > 0.5f; }},
			{EHostSourceType::AXIS_LEFT_X_NEG,     [](const GamepadState& s) { return s.GetAxisWithDeadzone(s.leftX) < -0.5f; }},
			{EHostSourceType::AXIS_LEFT_Y_POS,     [](const GamepadState& s) { return s.GetAxisWithDeadzone(s.leftY) > 0.5f; }},
			{EHostSourceType::AXIS_LEFT_Y_NEG,     [](const GamepadState& s) { return s.GetAxisWithDeadzone(s.leftY) < -0.5f; }},
			{EHostSourceType::AXIS_RIGHT_X_POS,    [](const GamepadState& s) { return s.GetAxisWithDeadzone(s.rightX) > 0.5f; }},
			{EHostSourceType::AXIS_RIGHT_X_NEG,    [](const GamepadState& s) { return s.GetAxisWithDeadzone(s.rightX) < -0.5f; }},
			{EHostSourceType::AXIS_RIGHT_Y_POS,    [](const GamepadState& s) { return s.GetAxisWithDeadzone(s.rightY) > 0.5f; }},
			{EHostSourceType::AXIS_RIGHT_Y_NEG,    [](const GamepadState& s) { return s.GetAxisWithDeadzone(s.rightY) < -0.5f; }},
			{EHostSourceType::TRIGGER_LEFT,        [](const GamepadState& s) { return s.triggerLeft > 0.5f; }},
			{EHostSourceType::TRIGGER_RIGHT,       [](const GamepadState& s) { return s.triggerRight > 0.5f; }},
		};

		// Only check the device assigned to the capturing player.
		// EnumerateDevices returns all SDL-recognized controllers, many of which
		// are not open/connected; GetState() returns an empty state for those.
		int cap_dev = -1;
		int assigned_device = gamepadProfiles_[m_capturing_player].host_device_index;
		for (const auto& device : devices) {
			if (device.index == assigned_device) {
				cap_dev = device.index;
				break;
			}
		}
		if (cap_dev < 0) return;

		// Actively refresh this device's state ourselves instead of relying on
		// GLCanvas's own polling timer (GLCanvas::OnGamepadPoll) to have done it.
		// That timer keeps the shared GamepadState fresh during normal play, but
		// this tab can be shown through a chain of nested modal dialogs
		// (Frame::OnOptions -> OptionsDialog::ShowModal), and whether a
		// *different* window's wxTimer keeps firing throughout a nested modal
		// loop is not something to rely on - if it stalls, GetState() below
		// would keep returning a stale (or empty) state and capture would never
		// trigger no matter what the user presses. RefreshDeviceState() calls
		// SDL_GameControllerUpdate() and reads button/axis state directly, so
		// it works regardless of whether PollEvents() is running elsewhere.
		GamepadBackend().RefreshDeviceState(cap_dev);

		const auto& state = GamepadBackend().GetState(cap_dev);

		for (const auto& check : checks) {
			bool active = check.second(state);

			if (active) {
				JoystickMappingEntry entry;
				entry.source_type = check.first;
				if (check.first >= EHostSourceType::AXIS_LEFT_X_POS &&
					check.first <= EHostSourceType::TRIGGER_RIGHT) {
					entry.threshold = 0.5f;
				}

				// Buffered locally, same as every other option — only
				// written to xOptions when OK is pressed.
				gamepadProfiles_[m_capturing_player].input_map[m_capturing_input] = entry;

				UpdateMappingLabels(m_capturing_player);

				StopCaptureMode();
				return;
			}
		}
	}

	void OptionsDialog::OnResetGamepad(wxCommandEvent& event) {
		if (m_capturing_player >= 0) {
			StopCaptureMode();
		}

		// Same as OnResetAudio/Video/Input/Drive: only touches the local,
		// buffered state and the widgets that reflect it. Nothing is written
		// to xOptions until OK is pressed.
		for (int player = 0; player < 2; ++player) {
			gamepadProfiles_[player] = JoystickProfile(); // host_device_index = -1, empty input_map
		}

		RefreshDeviceList();
		UpdateMappingLabels(0);
		UpdateMappingLabels(1);
	}

	void OptionsDialog::OnCancelBtn(wxCommandEvent& event) {
		// Nothing on the Gamepads tab is written to xOptions until OK is
		// pressed (device selection and captured mappings are buffered in
		// gamepadProfiles_, same as every other tab's local state), so
		// Cancel has nothing to roll back — just stop an in-progress capture
		// so its timer doesn't outlive the dialog.
		if (m_capturing_player >= 0) {
			StopCaptureMode();
		}

		EndModal(wxID_CANCEL);
	}

	void OptionsDialog::OnDevicePollTimer(wxTimerEvent& event) {
		if (!DeviceListChanged()) {
			return;
		}

		// Remember what was connected before refreshing, so a controller
		// that just appeared can be told apart from ones that were already
		// there.
		std::vector<WxGamepadBackend::DeviceInfo> old_devices = m_devices;

		RefreshDeviceList();

		if (m_capturing_player < 0) return;
		JoystickProfile& capturing_profile = gamepadProfiles_[m_capturing_player];

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

				for (size_t i = 0; i < m_devices.size(); ++i) {
					if (m_devices[i].guid == new_device->guid) {
						m_players[m_capturing_player].device_combo->SetSelection(static_cast<int>(i) + 1);
						break;
					}
				}
			} else {
				StopCaptureMode();
			}
		} else if (capturing_profile.host_device_index < 0) {
			// A device *was* assigned, but RefreshDeviceList() just
			// couldn't find it among the currently connected devices - it
			// was unplugged mid-capture. The capture can never complete
			// against a device that isn't there; cancel it instead of
			// leaving the button stuck showing "Capturing..." forever.
			StopCaptureMode();
		}
		// else: still capturing against a device that's still connected -
		// nothing to do, OnTimer() keeps polling it normally.
	}

	JoystickProfile OptionsDialog::LoadProfileFromOptions(int player_idx) const {
		JoystickProfile profile;

		std::string mapping_data = OpJoystickMappingData(player_idx);
		DeserializeProfile(mapping_data, profile);

		// The persisted numeric device_index is only a fallback hint now
		// (see ResolveDeviceIndexForGuid) — the GUID embedded in the mapping
		// string is the real identity, and is re-resolved against whatever
		// is actually connected right now rather than trusted blindly.
		int hinted_index = OpHostGamepadDevice(player_idx);
		std::string resolved_guid;
		profile.host_device_index = ResolveDeviceIndexForGuid(profile.device_guid, hinted_index, &resolved_guid);
		profile.device_guid = resolved_guid;

		return profile;
	}

	void OptionsDialog::SaveProfileToOptions(int player_idx, const JoystickProfile& profile) const {
		// The numeric host_gamepad_device_N option is intentionally left
		// alone here: once a profile has a device_guid, that option is dead
		// weight for it (nothing ever reads it in that case - see
		// LoadProfileFromOptions). Rewriting it every time the dialog is
		// used would just keep duplicating the same identity in two places
		// in the saved config for no benefit. It's still written elsewhere
		// (the --gamepad-device command-line flag, wx_frame.cpp) for the one
		// case where only a raw index is available and there's no GUID to
		// resolve it from.
		std::string profile_str = SerializeProfile(profile); // embeds device_guid + mapping
		OpJoystickMappingData(player_idx, profile_str);
	}
#endif // USE_SDL2_GAMEPAD

} // namespace xPlatform

#endif//USE_WXWIDGETS
