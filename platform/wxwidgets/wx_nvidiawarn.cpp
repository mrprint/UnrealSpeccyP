#include "../platform.h"
#ifdef USE_WXWIDGETS

#include "wx_nvidiawarn.h"
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>

namespace xPlatform {

NvidiaWarnDialog::NvidiaWarnDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _("NVIDIA Driver Warning"),
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE)
{
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    const int pad = 12;

    wxStaticText* text = new wxStaticText(this, wxID_ANY,
        _("An NVIDIA GPU has been detected.\n\n"
          "NVIDIA's Threaded Optimization feature may cause the application\n"
          "to freeze, especially in fast-forward (true speed) mode.\n\n"
          "If you experience freezes or system hangs, disable it:\n\n"
          "  NVIDIA Control Panel\n"
          "    -> Manage 3D Settings\n"
          "    -> Program Settings -> add this application\n"
          "    -> Threaded Optimization -> Off"));

    sizer->Add(text, 0, wxALL, pad);

    m_checkbox = new wxCheckBox(this, wxID_ANY, _("Don't show this warning again"));
    sizer->Add(m_checkbox, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);

    wxStdDialogButtonSizer* btn = new wxStdDialogButtonSizer();
    btn->Add(new wxButton(this, wxID_OK, _("OK")));
    btn->Realize();
    sizer->Add(btn, 0, wxALIGN_RIGHT | wxALL, pad);

    SetSizerAndFit(sizer);
    CenterOnParent();
}

} // namespace xPlatform
#endif
