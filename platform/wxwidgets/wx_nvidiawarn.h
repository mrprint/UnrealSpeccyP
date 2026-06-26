#pragma once
#ifdef USE_WXWIDGETS

#include <wx/dialog.h>
#include <wx/checkbox.h>

namespace xPlatform {

class NvidiaWarnDialog : public wxDialog
{
public:
    NvidiaWarnDialog(wxWindow* parent);

    bool DontShowAgain() const { return m_checkbox->GetValue(); }

private:
    wxCheckBox* m_checkbox = nullptr;
};

} // namespace xPlatform
#endif
