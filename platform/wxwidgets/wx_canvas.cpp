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

#include "../../options_common.h"
#include "../../devices/video_snapshot.h"

#include <array>
#include <cstring>
#include <utility>
#include <wx/wx.h>
#include <wx/glcanvas.h>
#include <wx/display.h>
#include "wx_joystick.h"
#ifdef USE_SDL2_GAMEPAD
#include "wx_gamepad.h"
#include "joystick_mapper.h"
#endif

namespace xPlatform
{

void OnLoopSound();
void TranslateKey(int& key, dword& flags);

void VsyncGL(bool on);
void initGlew();
void initGraphics(int scr_width, int scr_height);
void cleanupGraphics();
bool DrawGL(int vport_x, int vport_y, int vport_width, int vport_height,
    const VideoSnapshot& snap);

wxWindow* CreateMouseCapture(wxWindow* parent);

extern const wxEventType evtMouseCapture = wxNewEventType();
extern const wxEventType evtSetStatusText = wxNewEventType();
extern const wxEventType evtExitFullScreen = wxNewEventType();

//=============================================================================
//	GLCanvas
//-----------------------------------------------------------------------------
class GLCanvas : public wxGLCanvas
{
	typedef wxGLCanvas eInherited;
public:
	GLCanvas(wxWindow* parent);
	virtual ~GLCanvas();

	void PostStatusText(const wxString& text)
	{
		wxCommandEvent ev(evtSetStatusText);
		ev.SetString(text);
		wxQueueEvent(GetParent(), ev.Clone());
	}

#ifdef USE_SDL2_GAMEPAD
	// Re-reads host_device_index/input mapping for both players from
	// xOptions into m_profiles. Called once from the constructor, and again
	// whenever OptionsDialog::OnOK() has just written new gamepad settings,
	// so a just-assigned device/mapping takes effect immediately instead of
	// only after the emulator is restarted (m_profiles was previously only
	// ever populated at construction time).
	void ReloadGamepadProfiles();
#endif

private:
	void OnPaint(wxPaintEvent& event);
	void Paint(wxDC& dc);
	void OnIdle(wxIdleEvent& event);
	void OnEraseBackground(wxEraseEvent& event) {}
	void OnKeydown(wxKeyEvent& event);
	void OnKeyup(wxKeyEvent& event);
	void OnKillFocus(wxFocusEvent& event);
	void OnMouseKey(wxMouseEvent& event);
	void OnMouseCapture(wxCommandEvent& event);
	void OnJoystickEvent(wxJoystickEvent& event);
#ifdef USE_SDL2_GAMEPAD
	void OnGamepadPoll(wxTimerEvent& event);
#endif

	static std::pair<int, int> getMaxDisplayResolution();

private:
	static int canvas_attr[];
	DECLARE_EVENT_TABLE()

    wxGLContext* gl_context;
	wxWindow* mouse_capture;
	eWxJoystick* joysticks[2];
	bool gl_initialized;

#ifdef USE_SDL2_GAMEPAD
	// SDL2 gamepad support
	wxTimer m_gamepad_timer;
	JoystickMapper m_joystick_mapper;
	std::array<JoystickProfile, 2> m_profiles;
#endif
};
int GLCanvas::canvas_attr[] = { WX_GL_RGBA, WX_GL_DOUBLEBUFFER, 0 };

//=============================================================================
//	EVENT_TABLE
//-----------------------------------------------------------------------------
BEGIN_EVENT_TABLE(GLCanvas, wxGLCanvas)
	EVT_PAINT(GLCanvas::OnPaint)
	EVT_ERASE_BACKGROUND(GLCanvas::OnEraseBackground)
	EVT_IDLE(GLCanvas::OnIdle)
	EVT_KEY_DOWN(GLCanvas::OnKeydown)
	EVT_KEY_UP(GLCanvas::OnKeyup)
	EVT_LEFT_DOWN(GLCanvas::OnMouseKey)
	EVT_KILL_FOCUS(GLCanvas::OnKillFocus)
	EVT_COMMAND(wxID_ANY, evtMouseCapture, GLCanvas::OnMouseCapture)
	EVT_JOYSTICK_EVENTS(GLCanvas::OnJoystickEvent)
#ifdef USE_SDL2_GAMEPAD
	EVT_TIMER(wxID_ANY, GLCanvas::OnGamepadPoll)
#endif
END_EVENT_TABLE()

//=============================================================================
//	GLCanvas::GLCanvas
//-----------------------------------------------------------------------------
GLCanvas::GLCanvas(wxWindow* parent) : eInherited(parent, wxID_ANY, canvas_attr), mouse_capture(NULL), gl_initialized(false)
{
	wxGLContextAttrs ctx_attrs;
	ctx_attrs.PlatformDefaults().CoreProfile().OGLVersion(3, 0).EndList();
    gl_context = new wxGLContext(this, nullptr, &ctx_attrs);
	if (!gl_context->IsOK())
	{
		wxMessageBox("An OpenGL 3.0 capable driver is required.\nThe app will end now.",
			"OpenGL version error", wxOK | wxICON_INFORMATION, this);
		delete gl_context;
		gl_context = nullptr;
	}
	joysticks[0] = new eWxJoystick(this, wxJOYSTICK1);
	joysticks[1] = new eWxJoystick(this, wxJOYSTICK2);

#ifdef USE_SDL2_GAMEPAD
	// Load saved profiles from options
	ReloadGamepadProfiles();

	// wxTimer's default constructor does NOT set an owner window, and
	// without one it never posts a wxTimerEvent anywhere - the underlying
	// OS timer still ticks, but GLCanvas::OnGamepadPoll (bound below via
	// EVT_TIMER) is simply never invoked, so gamepad input is read only
	// once at startup and never again during actual gameplay. SetOwner()
	// must be called before Start() so ticks are routed to this window.
	m_gamepad_timer.SetOwner(this);
	m_gamepad_timer.Start(16);  // ~60 FPS polling
#endif
}
//=============================================================================
//	GLCanvas::~GLCanvas
//-----------------------------------------------------------------------------
GLCanvas::~GLCanvas()
{
	cleanupGraphics();
	delete joysticks[0];
	delete joysticks[1];
    delete gl_context;
}
//=============================================================================
//	GLCanvas::OnPaint
//-----------------------------------------------------------------------------
void GLCanvas::OnPaint(wxPaintEvent& event)
{
	wxPaintDC dc(this);
	Paint(dc);
}
//=============================================================================
//	GLCanvas::Paint
//-----------------------------------------------------------------------------
void GLCanvas::Paint(wxDC& dc)
{
	if (!IsShown() || !gl_context)
		return;
	int w, h;
	GetClientSize(&w, &h);
    SetCurrent(*gl_context);
	if (!gl_initialized)
	{
		initGlew();
		auto resolution = getMaxDisplayResolution();
		initGraphics(resolution.first, resolution.second);
		gl_initialized = true;
	}

	// Build the snapshot synchronously right here: single-threaded, so
	// emulation (OnIdle -> OnLoop) and rendering never run concurrently and
	// no locking is needed. DrawGL() itself stays a pure GL function that
	// touches only what's in the snapshot.
	VideoSnapshot snap;
	memcpy(snap.video, Handler()->VideoData(), sizeof(snap.video));
	snap.frame = Handler()->VideoFrame();
#ifdef USE_UI
	const byte* data_ui = (const byte*)Handler()->VideoDataUI();
	snap.has_ui = data_ui != nullptr;
	if (snap.has_ui)
		memcpy(snap.video_ui, data_ui, sizeof(snap.video_ui));
#endif

	if (DrawGL(0, 0, w, h, snap))
		SwapBuffers();
}
//=============================================================================
//	GLCanvas::OnIdle
//-----------------------------------------------------------------------------
void GLCanvas::OnIdle(wxIdleEvent& event)
{
	if(OpQuit())
	{
		GetParent()->Close(true);
		return;
	}
	const char* err = Handler()->OnLoop();
	if(err)
	{
		wxCommandEvent ev(evtSetStatusText);
		ev.SetString(wxConvertMB2WX(err));
		ProcessEvent(ev);
	}
	OnLoopSound();
	{
		wxClientDC dc(this);
		static bool vsync = false;
		bool s = !Handler()->FullSpeed();
		if(vsync != s)
		{
			vsync = s;
			VsyncGL(vsync);
		}
		Paint(dc);
	}
	if(!Handler()->FullSpeed())
		wxMilliSleep(3);
	event.RequestMore();
}
//=============================================================================
//	GLCanvas::OnKeydown
//-----------------------------------------------------------------------------
void GLCanvas::OnKeydown(wxKeyEvent& event)
{
	int key = event.GetKeyCode();
	if(key == WXK_ESCAPE)
	{
		if(mouse_capture)
		{
			mouse_capture->Close();
		}
		else
		{
			wxCommandEvent ev(evtExitFullScreen);
			wxPostEvent(this, ev);
		}
		return;
	}
	dword flags = KF_DOWN|OpJoyKeyFlags();
	if(event.AltDown())		flags |= KF_ALT;
	if(event.ShiftDown())	flags |= KF_SHIFT;
	TranslateKey(key, flags);
	Handler()->OnKey(key, flags);
}
//=============================================================================
//	GLCanvas::OnKeyup
//-----------------------------------------------------------------------------
void GLCanvas::OnKeyup(wxKeyEvent& event)
{
	int key = event.GetKeyCode();
	dword flags = 0;
	if(event.AltDown())		flags |= KF_ALT;
	if(event.ShiftDown())	flags |= KF_SHIFT;
	TranslateKey(key, flags);
	Handler()->OnKey(key, OpJoyKeyFlags());
}
//=============================================================================
//	GLCanvas::OnMouseKey
//-----------------------------------------------------------------------------
void GLCanvas::OnMouseKey(wxMouseEvent& event)
{
	event.Skip();
#ifndef _MAC
	if(!mouse_capture)
		mouse_capture = CreateMouseCapture(this);
#endif//_MAC
}
//=============================================================================
//	GLCanvas::OnKillFocus
//-----------------------------------------------------------------------------
void GLCanvas::OnKillFocus(wxFocusEvent& event)
{
	SAFE_CALL(mouse_capture)->Close();
}
//=============================================================================
//	GLCanvas::OnMouseCapture
//-----------------------------------------------------------------------------
void GLCanvas::OnMouseCapture(wxCommandEvent& event)
{
	event.Skip();
	if(!event.GetId())
		mouse_capture = NULL;
}
//=============================================================================
//	GLCanvas::OnJoystickEvent
//-----------------------------------------------------------------------------
void GLCanvas::OnJoystickEvent(wxJoystickEvent& event)
{
	switch(event.GetJoystick())
	{
	case wxJOYSTICK1: joysticks[0]->OnEvent(event); break;
	case wxJOYSTICK2: joysticks[1]->OnEvent(event); break;
	}
}

#ifdef USE_SDL2_GAMEPAD
//=============================================================================
//	GLCanvas::ReloadGamepadProfiles
//-----------------------------------------------------------------------------
void GLCanvas::ReloadGamepadProfiles()
{
    for (int i = 0; i < 2; ++i) {
        std::string mapping_data = OpJoystickMappingData(i);
        DeserializeProfile(mapping_data, m_profiles[i]);

        // Device identity is the GUID embedded in the mapping string, not
        // the persisted numeric index (which SDL can reassign across
        // reconnects/restarts) - resolve it against whatever is actually
        // connected right now. See ResolveDeviceIndexForGuid() for details,
        // including the one-time migration path for profiles saved before
        // GUID-based tracking existed.
        int hinted_index = OpHostGamepadDevice(i);
        m_profiles[i].host_device_index = ResolveDeviceIndexForGuid(m_profiles[i].device_guid, hinted_index);

        if (m_profiles[i].IsEnabled()) {
            GamepadBackend().RefreshDeviceState(m_profiles[i].host_device_index);
        }
    }
}

//=============================================================================
//	GLCanvas::OnGamepadPoll
//-----------------------------------------------------------------------------
void GLCanvas::OnGamepadPoll(wxTimerEvent& /*event*/)
{
    GamepadBackend().PollEvents(
        [this](int device_index) {
            // Device added — update profile if it's assigned
            for (int i = 0; i < 2; ++i) {
                if (m_profiles[i].IsEnabled() && m_profiles[i].host_device_index == device_index) {
                    GamepadBackend().RefreshDeviceState(device_index);
                }
            }
        },
        [this](int device_index) {
            (void)device_index; // device removed
        }
    );

    // Process events for each player
    for (int player = 0; player < 2; ++player) {
        auto& profile = m_profiles[player];
        if (!profile.IsEnabled()) continue;

        const auto& state = GamepadBackend().GetState(profile.host_device_index);
        auto key_events = m_joystick_mapper.ProcessEvent(profile, player, state, profile.host_device_index);

        for (const auto& ke : key_events) {
            dword flags = OpJoyKeyFlags();
            if (ke.is_down) flags |= KF_DOWN;
            Handler()->OnKey(ke.key, flags);
        }
    }
}
#endif

//=============================================================================
//	LightweightShadersMessage
//-----------------------------------------------------------------------------
static GLCanvas* g_canvas = nullptr;

void LightweightShadersMessage(bool prev_use_lightweight, bool use_lightweight)
{
	if (prev_use_lightweight == use_lightweight) return;
	if (!g_canvas) return;
	g_canvas->PostStatusText(use_lightweight
		? "Lightweight shader enabled"
		: "Full-quality shader enabled");
}

//=============================================================================
//	CreateGLCanvas
//-----------------------------------------------------------------------------
wxWindow* CreateGLCanvas(wxWindow* parent)
{
	GLCanvas* canvas = new GLCanvas(parent);
	g_canvas = canvas;
	return canvas;
}

#ifdef USE_SDL2_GAMEPAD
void ReloadGamepadProfiles() { if (g_canvas) g_canvas->ReloadGamepadProfiles(); }
#endif

//=============================================================================

std::pair<int, int> GLCanvas::getMaxDisplayResolution()
{
	int displayCount = wxDisplay::GetCount();
	if (displayCount == 0)
	{
		return { -1, -1 };
	}

	int maxWidth = -1;
	int maxHeight = -1;

	for (int i = 0; i < displayCount; ++i)
	{
		wxDisplay display(i);
		if (!display.IsOk())
			continue;

		wxRect geometry = display.GetGeometry();
		int width = geometry.GetWidth();
		int height = geometry.GetHeight();

		if (width * height > maxWidth * maxHeight)
		{
			maxWidth = width;
			maxHeight = height;
		}
	}

	if (maxWidth == -1 || maxHeight == -1)
		return { -1, -1 };

	return { maxWidth, maxHeight };
}

}
//namespace xPlatform

#endif//USE_WXWIDGETS
