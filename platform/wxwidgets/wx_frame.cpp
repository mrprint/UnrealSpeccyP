/*
Portable ZX-Spectrum emulator.
Copyright (C) 2001-2013 SMT, Dexus, Alone Coder, deathsoft, djdron, scor

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

#include "../platform.h"

#ifdef USE_WXWIDGETS

#include "../../tools/options.h"
#include "../../options_common.h"
#include "wx_cmdline.h"
#include "wx_optionsdialog.h"

#include <wx/wx.h>
#include <wx/dnd.h>
#include <wx/aboutdlg.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>

namespace xPlatform
{

    void InitSound();
    void DoneSound();

    void initGraphics(int scr_width, int scr_height);
    void cleanupGraphics();

    wxWindow* CreateGLCanvas(wxWindow* parent);

    static struct eOptionWindowState : public xOptions::eOptionString
    {
        eOptionWindowState() { customizable = false; }
        virtual const char* Name() const { return "window state"; }
        const char* FormatStr() const { return "position(%d, %d); client_size(%d, %d)"; }
    } op_window_state;

    static struct eOptionFullScreen : public xOptions::eOptionBool
    {
        eOptionFullScreen() { customizable = false; }
        virtual const char* Name() const { return "full screen"; }
    } op_full_screen;

    extern const wxEventType evtMouseCapture;
    extern const wxEventType evtSetStatusText;
    extern const wxEventType evtExitFullScreen;

#ifndef _MAC
    struct DropFilesTarget : public wxFileDropTarget
    {
        virtual bool OnDropFiles(wxCoord x, wxCoord y, const wxArrayString& filenames)
        {
            if (filenames.empty())
                return false;
            ScopedEmuLock emu_guard;
            return Handler()->OnOpenFile(wxConvertWX2MB(filenames[0].c_str()));
        }
    };
#endif//_MAC

    //=============================================================================
    //  Frame
    //-----------------------------------------------------------------------------
    class Frame : public wxFrame
    {
    public:
        Frame(const wxString& title, const wxPoint& pos, const eCmdLine& cmdline);
        virtual ~Frame();
        void ShowFullScreen(bool on);

    private:
        void OnReset(wxCommandEvent& event);
        void OnQuit(wxCommandEvent& event);
        void OnAbout(wxCommandEvent& event);
        void OnOpenFile(wxCommandEvent& event);
        void OnSaveFile(wxCommandEvent& event);
        void SetFullScreen(bool on);
        void OnExitFullScreen(wxCommandEvent& event);
        void OnFullScreenToggle(wxCommandEvent& event);
        void OnResize(wxCommandEvent& event);
        void OnViewMode(wxCommandEvent& event);
        void OnViewGigascreenToggle(wxCommandEvent& event);
        void OnViewScanlinesToggle(wxCommandEvent& event);
        void OnViewPalEffectsToggle(wxCommandEvent& event);
        void OnTapeToggle(wxCommandEvent& event);
        void OnTapeFastToggle(wxCommandEvent& event);
        void OnPauseToggle(wxCommandEvent& event);
        void OnTrueSpeedToggle(wxCommandEvent& event);
        void OnMode48kToggle(wxCommandEvent& event);
        void OnResetToServiceRomToggle(wxCommandEvent& event);
        void OnAutoPlayImageToggle(wxCommandEvent& event);
        void OnMouseCapture(wxCommandEvent& event);
        void OnSetStatusText(wxCommandEvent& event);
        void OnQuickLoad(wxCommandEvent& event);
        void OnQuickSave(wxCommandEvent& event);
        void OnMinimize(wxCommandEvent& event);
        void OnZoom(wxCommandEvent& event);
        void OnOptions(wxCommandEvent& event);
        void OnClose(wxCloseEvent& event);

        void UpdateViewZoomMenu();
        bool UpdateBoolOption(wxMenuItem* o, const char* name, bool toggle = false) const;
        bool RestoreWindowState();
        void StoreWindowState() const;

        enum
        {
            ID_Reset = 1, ID_ResetToServiceRomToggle, ID_Size200, ID_Size300, ID_Minimize, ID_Zoom,
            ID_ViewFillScreen, ID_ViewSmallBorder, ID_ViewNoBorder,
            ID_ViewGigascreenToggle, ID_ViewScanlinesToggle, ID_FullScreenToggle,
            ID_TapeToggle, ID_TapeFastToggle, ID_AutoPlayImageToggle,
            ID_PauseToggle, ID_TrueSpeedToggle, ID_Mode48kToggle,
            ID_QuickSave, ID_QuickLoad,
            ID_Options,
            ID_ViewPalEffectsToggle
        };

        struct eViewMenuItems
        {
            wxMenuItem* fill_screen;
            wxMenuItem* small_border;
            wxMenuItem* no_border;
            wxMenuItem* gigascreen;
            wxMenuItem* scanlines;
            wxMenuItem* pal_effects;
        };

        eViewMenuItems menu_view;
        wxMenuItem* menu_pause;
        wxMenuItem* menu_true_speed;
        wxMenuItem* menu_mode_48k;
        wxMenuItem* menu_tape_fast;
        wxMenuItem* menu_reset_to_service_rom;
        wxMenuItem* menu_auto_play_image;
        wxMenuItem* menu_quick_save;

    private:
        DECLARE_EVENT_TABLE()

        wxWindow* gl_canvas;
        const wxSize   org_size;
    };

    //=============================================================================
    //  EVENT_TABLE
    //-----------------------------------------------------------------------------
    BEGIN_EVENT_TABLE(Frame, wxFrame)
        EVT_CLOSE(Frame::OnClose)
        EVT_MENU(wxID_EXIT, Frame::OnQuit)
        EVT_MENU(wxID_ABOUT, Frame::OnAbout)
        EVT_MENU(wxID_OPEN, Frame::OnOpenFile)
        EVT_MENU(wxID_SAVE, Frame::OnSaveFile)
        EVT_MENU(Frame::ID_Reset, Frame::OnReset)
        EVT_MENU(Frame::ID_Minimize, Frame::OnMinimize)
        EVT_MENU(Frame::ID_Zoom, Frame::OnZoom)
        EVT_MENU(wxID_ZOOM_100, Frame::OnResize)
        EVT_MENU(Frame::ID_Size200, Frame::OnResize)
        EVT_MENU(Frame::ID_Size300, Frame::OnResize)
        EVT_MENU(Frame::ID_ViewFillScreen, Frame::OnViewMode)
        EVT_MENU(Frame::ID_ViewSmallBorder, Frame::OnViewMode)
        EVT_MENU(Frame::ID_ViewNoBorder, Frame::OnViewMode)
        EVT_MENU(Frame::ID_ViewGigascreenToggle, Frame::OnViewGigascreenToggle)
        EVT_MENU(Frame::ID_ViewScanlinesToggle, Frame::OnViewScanlinesToggle)
        EVT_MENU(Frame::ID_ViewPalEffectsToggle, Frame::OnViewPalEffectsToggle)
        EVT_MENU(Frame::ID_FullScreenToggle, Frame::OnFullScreenToggle)
        EVT_MENU(Frame::ID_TapeToggle, Frame::OnTapeToggle)
        EVT_MENU(Frame::ID_TapeFastToggle, Frame::OnTapeFastToggle)
        EVT_MENU(Frame::ID_PauseToggle, Frame::OnPauseToggle)
        EVT_MENU(Frame::ID_TrueSpeedToggle, Frame::OnTrueSpeedToggle)
        EVT_MENU(Frame::ID_Mode48kToggle, Frame::OnMode48kToggle)
        EVT_MENU(Frame::ID_ResetToServiceRomToggle, Frame::OnResetToServiceRomToggle)
        EVT_MENU(Frame::ID_AutoPlayImageToggle, Frame::OnAutoPlayImageToggle)
        EVT_MENU(Frame::ID_QuickLoad, Frame::OnQuickLoad)
        EVT_MENU(Frame::ID_QuickSave, Frame::OnQuickSave)
        EVT_COMMAND(wxID_ANY, evtMouseCapture, Frame::OnMouseCapture)
        EVT_COMMAND(wxID_ANY, evtSetStatusText, Frame::OnSetStatusText)
        EVT_COMMAND(wxID_ANY, evtExitFullScreen, Frame::OnExitFullScreen)
        EVT_MENU(Frame::ID_Options, Frame::OnOptions)
        END_EVENT_TABLE()

#ifndef _MAC
#define SHORTCUT_OPEN               "F3"
#define SHORTCUT_SAVE               "F2"
#define SHORTCUT_QUICK_LOAD         "F4"
#define SHORTCUT_QUICK_SAVE         "F6"
#define SHORTCUT_START_STOP_TAPE    "F5"
#define SHORTCUT_PAUSE              "F7"
#define SHORTCUT_TRUE_SPEED         "F8"
#define SHORTCUT_MODE_48K           "F9"
#define SHORTCUT_RESET              "F12"
#define SHORTCUT_SIZE100            "Ctrl+1"
#define SHORTCUT_SIZE200            "Ctrl+2"
#define SHORTCUT_SIZE300            "Ctrl+3"
#define SHORTCUT_VIEW_FILL_SCREEN   "Ctrl+Shift+1"
#define SHORTCUT_VIEW_SMALL_BORDER  "Ctrl+Shift+2"
#define SHORTCUT_VIEW_NO_BORDER     "Ctrl+Shift+3"
#define SHORTCUT_VIEW_GIGASCREEN    "Ctrl+Shift+G"
#define SHORTCUT_VIEW_SCANLINES     "Ctrl+Shift+S"
#define SHORTCUT_VIEW_PAL           "Ctrl+Shift+P"
#define SHORTCUT_VIEW_FULLSCREEN    "Ctrl+F"
#else//_MAC
#define SHORTCUT_OPEN               "Ctrl+O"
#define SHORTCUT_SAVE               "Ctrl+S"
#define SHORTCUT_QUICK_LOAD         "RawCtrl+Ctrl+O"
#define SHORTCUT_QUICK_SAVE         "RawCtrl+Ctrl+S"
#define SHORTCUT_START_STOP_TAPE    "RawCtrl+T"
#define SHORTCUT_PAUSE              "RawCtrl+P"
#define SHORTCUT_TRUE_SPEED         "RawCtrl+S"
#define SHORTCUT_MODE_48K           "RawCtrl+M"
#define SHORTCUT_RESET              "RawCtrl+R"
#define SHORTCUT_SIZE100            "RawCtrl+1"
#define SHORTCUT_SIZE200            "RawCtrl+2"
#define SHORTCUT_SIZE300            "RawCtrl+3"
#define SHORTCUT_VIEW_FILL_SCREEN   "RawCtrl+Shift+1"
#define SHORTCUT_VIEW_SMALL_BORDER  "RawCtrl+Shift+2"
#define SHORTCUT_VIEW_NO_BORDER     "RawCtrl+Shift+3"
#define SHORTCUT_VIEW_GIGASCREEN    "RawCtrl+G"
#define SHORTCUT_VIEW_SCANLINES     "RawCtrl+S"
#define SHORTCUT_VIEW_PAL           "RawCtrl+P"
#define SHORTCUT_VIEW_FULLSCREEN    "RawCtrl+Ctrl+F"
#endif//_MAC

        //=============================================================================
        //  Frame::Frame
        //-----------------------------------------------------------------------------
        Frame::Frame(const wxString& title, const wxPoint& pos, const eCmdLine& cmdline)
        : wxFrame((wxFrame*)nullptr, -1, title, pos)
        , org_size(320, 240)
    {
#ifdef _WINDOWS
        SetIcon(wxICON(unreal_speccy_portable));
#endif//_WINDOWS
#ifdef _LINUX
        // Resolve the icon path relative to the executable directory so the
        // app can be launched from any working directory.
        wxFileName icon_path(wxStandardPaths::Get().GetExecutablePath());
        icon_path.SetFullName(wxT("unreal_speccy_portable.xpm"));
        wxString icon_file = icon_path.GetFullPath();
        if (wxFileExists(icon_file))
            SetIcon(wxIcon(icon_file, wxBITMAP_TYPE_XPM));
#endif//_LINUX

        wxMenu* menuFile = new wxMenu;
        menuFile->Append(wxID_OPEN, wxString(_("&Open...\t")) + _(SHORTCUT_OPEN));
        menuFile->Append(wxID_SAVE, wxString(_("&Save...\t")) + _(SHORTCUT_SAVE));
        menuFile->AppendSeparator();
        menuFile->Append(ID_QuickLoad, wxString(_("Quick &Load\t")) + _(SHORTCUT_QUICK_LOAD));
        menu_quick_save = menuFile->Append(ID_QuickSave, wxString(_("&Quick Save\t")) + _(SHORTCUT_QUICK_SAVE));
        menu_quick_save->Enable(false);
#ifdef _MAC
        menuFile->Append(wxID_ABOUT, _("About ") + title);
#else//_MAC
        SetDropTarget(new DropFilesTarget);
#endif//_MAC
        menuFile->AppendSeparator();
        menu_auto_play_image = menuFile->Append(ID_AutoPlayImageToggle,
            _("&Auto launch programs"), _(""), wxITEM_CHECK);
        menuFile->AppendSeparator();
        menuFile->Append(wxID_EXIT, _("E&xit"));

        wxMenu* menuDevice = new wxMenu;
        menuDevice->Append(ID_TapeToggle,
            wxString(_("&Start/Stop tape\t")) + _(SHORTCUT_START_STOP_TAPE));
        menu_tape_fast = menuDevice->Append(ID_TapeFastToggle,
            _("Tape &fast"), _(""), wxITEM_CHECK);
        menuDevice->AppendSeparator();
        menu_pause = menuDevice->Append(ID_PauseToggle,
            wxString(_("&Pause\t")) + _(SHORTCUT_PAUSE), _(""), wxITEM_CHECK);
        menu_true_speed = menuDevice->Append(ID_TrueSpeedToggle,
            wxString(_("&True speed\t")) + _(SHORTCUT_TRUE_SPEED), _(""), wxITEM_CHECK);
        menu_mode_48k = menuDevice->Append(ID_Mode48kToggle,
            wxString(_("Mode &48k\t")) + _(SHORTCUT_MODE_48K), _(""), wxITEM_CHECK);
        menu_reset_to_service_rom = menuDevice->Append(ID_ResetToServiceRomToggle,
            _("Reset to service R&OM"), _(""), wxITEM_CHECK);
        menuDevice->Append(ID_Reset,
            wxString(_("&Reset\t")) + _(SHORTCUT_RESET));
        menuDevice->AppendSeparator();
        menuDevice->Append(ID_Options, _("&Options..."));

        wxMenu* menuWindow = new wxMenu;
#ifdef _MAC
        menuWindow->Append(ID_Minimize, _("Minimize\tCtrl+M"));
        menuWindow->Append(ID_Zoom, _("Zoom"));
        menuWindow->AppendSeparator();
#endif//_MAC
        menuWindow->Append(wxID_ZOOM_100,
            wxString(_("Size &100%\t")) + _(SHORTCUT_SIZE100));
        menuWindow->Append(ID_Size200,
            wxString(_("Size &200%\t")) + _(SHORTCUT_SIZE200));
        menuWindow->Append(ID_Size300,
            wxString(_("Size &300%\t")) + _(SHORTCUT_SIZE300));

        wxMenu* menuView = new wxMenu;
        menu_view.fill_screen = menuView->Append(ID_ViewFillScreen,
            wxString(_("Fill screen\t")) + _(SHORTCUT_VIEW_FILL_SCREEN), _(""), wxITEM_CHECK);
        menu_view.small_border = menuView->Append(ID_ViewSmallBorder,
            wxString(_("Small border\t")) + _(SHORTCUT_VIEW_SMALL_BORDER), _(""), wxITEM_CHECK);
        menu_view.no_border = menuView->Append(ID_ViewNoBorder,
            wxString(_("No border\t")) + _(SHORTCUT_VIEW_NO_BORDER), _(""), wxITEM_CHECK);
        menuView->AppendSeparator();
        menu_view.gigascreen = menuView->Append(ID_ViewGigascreenToggle,
            wxString(_("Gigascreen\t")) + _(SHORTCUT_VIEW_GIGASCREEN), _(""), wxITEM_CHECK);
        menu_view.scanlines = menuView->Append(ID_ViewScanlinesToggle,
            wxString(_("CRT scanlines\t")) + _(SHORTCUT_VIEW_SCANLINES), _(""), wxITEM_CHECK);
        menu_view.pal_effects = menuView->Append(ID_ViewPalEffectsToggle,
            wxString(_("PAL effects\t")) + _(SHORTCUT_VIEW_PAL), _(""), wxITEM_CHECK);
        menuView->AppendSeparator();
        menuView->Append(ID_FullScreenToggle,
            wxString(_("&Full screen\t")) + _(SHORTCUT_VIEW_FULLSCREEN));

        wxMenuBar* menuBar = new wxMenuBar;
        menuBar->Append(menuFile, _("File"));
        menuBar->Append(menuView, _("View"));
        menuBar->Append(menuDevice, _("Device"));
        menuBar->Append(menuWindow, _("Window"));
#ifndef _MAC
        wxMenu* menuHelp = new wxMenu;
        menuHelp->Append(wxID_ABOUT, _("&About ") + title);
        menuBar->Append(menuHelp, _("Help"));
#endif//_MAC
        SetMenuBar(menuBar);

        CreateStatusBar();
        SetStatusText(_("Ready..."));

        SetClientSize(org_size);
        SetMinSize(GetSize());

        if (!RestoreWindowState())
            SetClientSize(org_size * 2);

        if (cmdline.size_percent >= 0)
        {
            op_full_screen.Set(false);
            SetClientSize(org_size * cmdline.size_percent / 100);
        }

        gl_canvas = CreateGLCanvas(this);
        gl_canvas->SetFocus();

        xOptions::eOption<bool>* op_true_speed = xOptions::eOption<bool>::Find("true speed");
        if (cmdline.true_speed != eCmdLine::V_DEFAULT && op_true_speed)
        {
            op_true_speed->Set(cmdline.true_speed == eCmdLine::V_ON);
            op_true_speed->Apply();
        }
        if (cmdline.full_screen != eCmdLine::V_DEFAULT)
            op_full_screen.Set(cmdline.full_screen == eCmdLine::V_ON);

        xOptions::eOption<bool>* op_mode_48k = xOptions::eOption<bool>::Find("mode 48k");
        if (cmdline.mode_48k != eCmdLine::V_DEFAULT && op_mode_48k)
        {
            op_mode_48k->Set(cmdline.mode_48k == eCmdLine::V_ON);
            op_mode_48k->Apply();
        }
        if (!cmdline.joystick.empty())
        {
            xOptions::eOption<int>* op_joy = xOptions::eOption<int>::Find("joystick");
            SAFE_CALL(op_joy)->Value(wxConvertWX2MB(cmdline.joystick));
        }

        menu_true_speed->Check(op_true_speed && *op_true_speed);
        menu_mode_48k->Check(op_mode_48k && *op_mode_48k);

        UpdateBoolOption(menu_tape_fast, "fast tape");
        UpdateBoolOption(menu_reset_to_service_rom, "reset to service rom");
        UpdateBoolOption(menu_auto_play_image, "auto play image");
        UpdateBoolOption(menu_view.gigascreen, "gigascreen");
        UpdateBoolOption(menu_view.scanlines, "scanlines");
        UpdateBoolOption(menu_view.pal_effects, "pal effects");
        UpdateViewZoomMenu();

        if (!cmdline.file_to_open.empty())
            Handler()->OnOpenFile(wxConvertWX2MB(cmdline.file_to_open));
    }

    //=============================================================================
    //  Frame::~Frame
    //-----------------------------------------------------------------------------
    Frame::~Frame()
    {
        if (!IsFullScreen())
            StoreWindowState();
    }

    //=============================================================================
    //  Frame::OnClose
    //-----------------------------------------------------------------------------
    void Frame::OnClose(wxCloseEvent& event)
    {
        if (!IsFullScreen())
            StoreWindowState();

        // Destroy the GL canvas (and join the render thread) *synchronously*
        // before Destroy() posts its deferred frame-delete event.  Without this,
        // App::OnExit() → DoneSound() can run while the render thread is still
        // calling OnLoopSound(), causing a use-after-free in the audio buffers.
        if (gl_canvas)
        {
            gl_canvas->Destroy();
            gl_canvas = nullptr;
        }

        Destroy();
    }

    //=============================================================================
    //  Frame::ShowFullScreen
    //-----------------------------------------------------------------------------
    void Frame::ShowFullScreen(bool on)
    {
        if (on)
            StoreWindowState();
#ifdef _MAC
        if (on)
            GetStatusBar()->Hide();
        else
            GetStatusBar()->Show();
#endif//_MAC
        wxFrame::ShowFullScreen(on, wxFULLSCREEN_ALL);
    }

    //=============================================================================
    //  Frame::UpdateBoolOption
    //-----------------------------------------------------------------------------
    bool Frame::UpdateBoolOption(wxMenuItem* o, const char* name, bool toggle) const
    {
        xOptions::eOption<bool>* op = xOptions::eOption<bool>::Find(name);
        if (op && toggle)
            op->Change();
        bool on = op && *op;
        o->Check(on);
        return on;
    }

    //=============================================================================
    //  Frame::OnQuit
    //-----------------------------------------------------------------------------
    void Frame::OnQuit(wxCommandEvent& /*event*/)
    {
        Close(true);
    }

    //=============================================================================
    //  Frame::OnReset
    //-----------------------------------------------------------------------------
    void Frame::OnReset(wxCommandEvent& /*event*/)
    {
        ScopedEmuLock emu_guard;
        if (Handler()->OnAction(A_RESET) == AR_OK)
            SetStatusText(_("Reset OK"));
        else
            SetStatusText(_("Reset FAILED"));
    }

    //=============================================================================
    //  Frame::OnAbout
    //-----------------------------------------------------------------------------
    void Frame::OnAbout(wxCommandEvent& /*event*/)
    {
        ScopedRenderPause render_guard;

        wxAboutDialogInfo info;
        info.SetName(GetTitle());
        info.SetDescription(_("Portable ZX Spectrum emulator."));
        info.SetCopyright(_("Copyright (C) 2001-2020 SMT, Dexus, Alone Coder, deathsoft, djdron, scor."));
#ifndef _MAC
        info.SetVersion(_("0.0.90"));
        info.SetWebSite(_("https://bitbucket.org/djdron/unrealspeccyp"));
        info.SetLicense(_(
            "This program is free software: you can redistribute it and/or modify\n"
            "it under the terms of the GNU General Public License as published by\n"
            "the Free Software Foundation, either version 3 of the License, or\n"
            "(at your option) any later version.\n\n"
            "This program is distributed in the hope that it will be useful,\n"
            "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
            "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
            "GNU General Public License for more details.\n\n"
            "You should have received a copy of the GNU General Public License\n"
            "along with this program.  If not, see <http://www.gnu.org/licenses/>.\n"
        ));
#endif//_MAC
        wxAboutBox(info);
    }

    //=============================================================================
    //  Frame::OnOpenFile
    //-----------------------------------------------------------------------------
    void Frame::OnOpenFile(wxCommandEvent& /*event*/)
    {
        // No ScopedRenderPause here — the file dialog is informational only,
        // the emulator (including tape) continues running while it is open.
        // The render thread touching SwapBuffers during a file dialog is less
        // dangerous than during an options dialog because no GL state changes.
        wxFileDialog fd(this, wxFileSelectorPromptStr,
            wxConvertMB2WX(OpLastFolder()));
        fd.SetWildcard(
            L"Supported files|*.sna;*.z80;*.szx;*.rzx;*.trd;*.scl;*.fdi;*.tap;*.csw;*.tzx;*.zip;"
            L"*.SNA;*.Z80;*.SZX;*.RZX;*.TRD;*.SCL;*.FDI;*.TAP;*.CSW;*.TZX;*.ZIP|"
            L"All files|*.*|"
            L"Snapshot files (*.sna;*.z80;*.szx)|*.sna;*.z80;*.szx;*.SNA;*.Z80;*.SZX|"
            L"Replay files (*.rzx)|*.rzx;*.RZX|"
            L"Disk images (*.trd;*.scl;*.fdi;*.td0;*.udi)|*.trd;*.scl;*.fdi;*.td0;*.udi;*.TRD;*.SCL;*.FDI;*.TD0;*.UDI|"
            L"Tape files (*.tap;*.csw;*.tzx)|*.tap;*.csw;*.tzx;*.TAP;*.CSW;*.TZX|"
            L"ZIP archives (*.zip)|*.zip;*.ZIP"
        );
        if (fd.ShowModal() == wxID_OK)
        {
            // Pause render + lock emulator only for the actual file load.
            ScopedRenderPause render_guard;
            ScopedEmuLock emu_guard;
            if (Handler()->OnOpenFile(wxConvertWX2MB(fd.GetPath().c_str())))
            {
                SetStatusText(_("File open OK"));
                menu_quick_save->Enable(true);
            }
            else
                SetStatusText(_("File open FAILED"));
        }
    }

    //=============================================================================
    //  Frame::OnSaveFile
    //-----------------------------------------------------------------------------
    void Frame::OnSaveFile(wxCommandEvent& /*event*/)
    {
        ScopedRenderPause render_guard;

        // Pause the emulator display while the dialog is open so the screen
        // does not update under the user's feet, but do NOT hold the emu lock
        // across ShowModal — that would block the render thread for the entire
        // duration of the dialog (potentially many seconds).
        {
            ScopedEmuLock emu_guard;
            Handler()->VideoPaused(true);
        }

        wxFileDialog fd(this, wxFileSelectorPromptStr,
            wxConvertMB2WX(OpLastFolder()),
            wxEmptyString,
            wxFileSelectorDefaultWildcardStr,
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        fd.SetWildcard(
            L"Snapshot files (*.sna)|*.sna;*.SNA|"
            L"Screenshot files (*.png)|*.png;*.PNG"
        );
        if (fd.ShowModal() == wxID_OK)
        {
            wxString path = fd.GetPath();
            int fi = fd.GetFilterIndex();
            size_t p = path.length() - 4;
            if (path.length() < 4 || (
                path.rfind(L".sna") != p && path.rfind(L".SNA") != p &&
                path.rfind(L".png") != p && path.rfind(L".PNG") != p))
                path += fi ? L".png" : L".sna";
            ScopedEmuLock emu_guard;
            if (Handler()->OnSaveFile(wxConvertWX2MB(path.c_str())))
                SetStatusText(_("File save OK"));
            else
                SetStatusText(_("File save FAILED"));
        }

        {
            ScopedEmuLock emu_guard;
            Handler()->VideoPaused(false);
        }
    }

    //=============================================================================
    //  Frame::SetFullScreen
    //-----------------------------------------------------------------------------
    void Frame::SetFullScreen(bool on)
    {
        op_full_screen.Set(on);
        if (IsFullScreen() != static_cast<bool>(op_full_screen))
            ShowFullScreen(op_full_screen);
    }

    //=============================================================================
    //  Frame::OnExitFullScreen / OnFullScreenToggle
    //-----------------------------------------------------------------------------
    void Frame::OnExitFullScreen(wxCommandEvent& /*event*/) { SetFullScreen(false); }
    void Frame::OnFullScreenToggle(wxCommandEvent& /*event*/) { SetFullScreen(!op_full_screen); }

    //=============================================================================
    //  Frame::OnResize
    //-----------------------------------------------------------------------------
    void Frame::OnResize(wxCommandEvent& event)
    {
        int size = 1;
        switch (event.GetId())
        {
        case wxID_ZOOM_100: size = 1; break;
        case ID_Size200:    size = 2; break;
        case ID_Size300:    size = 3; break;
        }
        if (IsFullScreen())
        {
            ShowFullScreen(false);
            op_full_screen.Set(false);
        }
        if (IsMaximized())
            Maximize(false);
        SetClientSize(org_size * size);
    }

    //=============================================================================
    //  Frame::OnViewMode
    //-----------------------------------------------------------------------------
    void Frame::OnViewMode(wxCommandEvent& event)
    {
        ScopedRenderPause guard;    // idle the render thread before changing zoom
        using namespace xOptions;
        eOption<int>* op_zoom = eOption<int>::Find("zoom");
        switch (event.GetId())
        {
        case ID_ViewFillScreen:  op_zoom->Set(0); break;
        case ID_ViewSmallBorder: op_zoom->Set(1); break;
        case ID_ViewNoBorder:    op_zoom->Set(2); break;
        }
        UpdateViewZoomMenu();
    }

    //=============================================================================
    //  Frame::OnTapeToggle
    //-----------------------------------------------------------------------------
    void Frame::OnTapeToggle(wxCommandEvent& /*event*/)
    {
        ScopedEmuLock emu_guard;
        switch (Handler()->OnAction(A_TAPE_TOGGLE))
        {
        case AR_TAPE_STARTED:      SetStatusText(_("Tape started"));      break;
        case AR_TAPE_STOPPED:      SetStatusText(_("Tape stopped"));      break;
        case AR_TAPE_NOT_INSERTED: SetStatusText(_("Tape not inserted")); break;
        default: break;
        }
    }

    //=============================================================================
    //  Frame::OnTapeFastToggle
    //-----------------------------------------------------------------------------
    void Frame::OnTapeFastToggle(wxCommandEvent& /*event*/)
    {
        ScopedEmuLock emu_guard;
        using namespace xOptions;
        eOption<bool>* op = eOption<bool>::Find("fast tape");
        SAFE_CALL(op)->Change();
        bool on = op && *op;
        menu_tape_fast->Check(on);
        SetStatusText(on ? _("Fast tape on") : _("Fast tape off"));
    }

    //=============================================================================
    //  Frame::OnPauseToggle
    //-----------------------------------------------------------------------------
    void Frame::OnPauseToggle(wxCommandEvent& /*event*/)
    {
        ScopedEmuLock emu_guard;
        if (menu_pause->IsChecked())
        {
            Handler()->VideoPaused(true);
            SetStatusText(_("Paused..."));
        }
        else
        {
            Handler()->VideoPaused(false);
            SetStatusText(_("Ready..."));
        }
    }

    //=============================================================================
    //  View toggles
    //-----------------------------------------------------------------------------
    void Frame::OnViewGigascreenToggle(wxCommandEvent& /*event*/)
    {
        ScopedRenderPause guard;
        SetStatusText(UpdateBoolOption(menu_view.gigascreen, "gigascreen", true)
            ? _("Gigascreen on") : _("Gigascreen off"));
    }
    void Frame::OnViewScanlinesToggle(wxCommandEvent& /*event*/)
    {
        ScopedRenderPause guard;
        SetStatusText(UpdateBoolOption(menu_view.scanlines, "scanlines", true)
            ? _("CRT scanlines simulation on") : _("CRT scanlines simulation off"));
    }
    void Frame::OnViewPalEffectsToggle(wxCommandEvent& /*event*/)
    {
        ScopedRenderPause guard;
        SetStatusText(UpdateBoolOption(menu_view.pal_effects, "pal effects", true)
            ? _("PAL effects on") : _("PAL effects off"));
    }
    void Frame::OnTrueSpeedToggle(wxCommandEvent& /*event*/)
    {
        ScopedEmuLock emu_guard;
        SetStatusText(UpdateBoolOption(menu_true_speed, "true speed", true)
            ? _("True speed (50Hz mode) on") : _("True speed off"));
    }
    void Frame::OnMode48kToggle(wxCommandEvent& /*event*/)
    {
        ScopedEmuLock emu_guard;
        SetStatusText(UpdateBoolOption(menu_mode_48k, "mode 48k", true)
            ? _("Mode 48k on") : _("Mode 48k off"));
    }
    void Frame::OnResetToServiceRomToggle(wxCommandEvent& /*event*/)
    {
        ScopedEmuLock emu_guard;
        SetStatusText(UpdateBoolOption(menu_reset_to_service_rom, "reset to service rom", true)
            ? _("Reset to service ROM") : _("Reset to usual ROM"));
    }
    void Frame::OnAutoPlayImageToggle(wxCommandEvent& /*event*/)
    {
        ScopedEmuLock emu_guard;
        SetStatusText(UpdateBoolOption(menu_auto_play_image, "auto play image", true)
            ? _("Auto launch on") : _("Auto launch off"));
    }

    //=============================================================================
    //  Frame::OnMouseCapture
    //-----------------------------------------------------------------------------
    void Frame::OnMouseCapture(wxCommandEvent& event)
    {
        SetStatusText(event.GetId()
            ? _("Mouse captured, press ESC to cancel")
            : _("Mouse released"));
    }

    //=============================================================================
    //  Frame::OnSetStatusText
    //-----------------------------------------------------------------------------
    void Frame::OnSetStatusText(wxCommandEvent& event)
    {
        if (event.GetString() == L"rzx_finished")         SetStatusText(_("RZX playback finished"));
        else if (event.GetString() == L"rzx_sync_lost")   SetStatusText(_("RZX error - sync lost"));
        else if (event.GetString() == L"rzx_invalid")     SetStatusText(_("RZX error - invalid data"));
        else if (event.GetString() == L"rzx_unsupported") SetStatusText(_("RZX error - unsupported format"));
        else SetStatusText(event.GetString());
    }

    //=============================================================================
    //  Frame::OnQuickLoad / OnQuickSave
    //-----------------------------------------------------------------------------
    void Frame::OnQuickLoad(wxCommandEvent& /*event*/)
    {
        ScopedEmuLock emu_guard;
        using namespace xOptions;
        eOption<bool>* o = eOption<bool>::Find("load state");
        if (o)
        {
            o->Change();
            SetStatusText(*o ? _("Quick load OK") : _("Quick load FAILED"));
            if (*o) menu_quick_save->Enable(true);
        }
    }
    void Frame::OnQuickSave(wxCommandEvent& /*event*/)
    {
        ScopedEmuLock emu_guard;
        using namespace xOptions;
        eOption<bool>* o = eOption<bool>::Find("save state");
        if (o)
        {
            o->Change();
            SetStatusText(*o ? _("Quick save OK") : _("Quick save FAILED"));
        }
    }

    //=============================================================================
    //  Frame::OnMinimize / OnZoom
    //-----------------------------------------------------------------------------
    void Frame::OnMinimize(wxCommandEvent& /*event*/) { Iconize(); }
    void Frame::OnZoom(wxCommandEvent& /*event*/) { Maximize(!IsMaximized()); }

    //=============================================================================
    //  Frame::UpdateViewZoomMenu
    //-----------------------------------------------------------------------------
    void Frame::UpdateViewZoomMenu()
    {
        using namespace xOptions;
        eOption<int>* op_zoom = eOption<int>::Find("zoom");
        menu_view.fill_screen->Check(*op_zoom == 0);
        menu_view.small_border->Check(*op_zoom == 1);
        menu_view.no_border->Check(*op_zoom == 2);
    }

    //=============================================================================
    //  Frame::RestoreWindowState / StoreWindowState
    //-----------------------------------------------------------------------------
    bool Frame::RestoreWindowState()
    {
        wxPoint position;
        wxSize  client_size;
        if (sscanf(op_window_state, op_window_state.FormatStr(),
            &position.x, &position.y,
            &client_size.x, &client_size.y) == 4)
        {
            SetPosition(position);
            SetClientSize(client_size);
            return true;
        }
        return false;
    }

    void Frame::StoreWindowState() const
    {
        wxPoint position = GetPosition();
        wxSize  client_size = GetClientSize();
        char buf[512];
        sprintf(buf, op_window_state.FormatStr(),
            position.x, position.y,
            client_size.x, client_size.y);
        op_window_state.Value(buf);
    }

    //=============================================================================
    //  Frame::OnOptions
    //-----------------------------------------------------------------------------
    void Frame::OnOptions(wxCommandEvent& /*event*/)
    {
        // Note: while the render thread is blocked in MaybePause(), OnLoop() is
        // also suspended — the emulator does not tick for the duration of the
        // dialog.  This is acceptable: the user is interacting with the dialog,
        // not watching the emulator screen.
        ScopedRenderPause render_guard;

        OptionsDialog dialog(this);
        int result = dialog.ShowModal();

        if (result == wxID_OK)
        {
            // render_guard is still held here — UpdateBoolOption reads options
            // that DrawGL() also reads, so this is safe without a second pause.
            UpdateBoolOption(menu_view.gigascreen, "gigascreen");
            UpdateBoolOption(menu_view.scanlines, "scanlines");
            UpdateBoolOption(menu_view.pal_effects, "pal effects");
        }
        // ~ScopedRenderPause() → Resume()
    }

    //=============================================================================
    //  CreateFrame
    //-----------------------------------------------------------------------------
    Frame* g_main_frame = nullptr;

    wxWindow* CreateFrame(const wxString& title, const wxPoint& pos,
        const eCmdLine& cmdline)
    {
        Frame* frame = new Frame(title, pos, cmdline);
        g_main_frame = frame;
        frame->Show(true);
        if (op_full_screen)
            frame->ShowFullScreen(true);
        return frame;
    }

}//namespace xPlatform

#endif//USE_WXWIDGETS
