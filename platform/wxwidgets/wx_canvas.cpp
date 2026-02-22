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

#include <utility>
#include <atomic>
#include <chrono>
#include <wx/wx.h>
#include <wx/glcanvas.h>
#include <wx/display.h>
#include <wx/thread.h>
#include "wx_joystick.h"

namespace xPlatform
{

    void OnLoopSound();
    void TranslateKey(int& key, dword& flags);

    void VsyncGL(bool on);
    void initGlew();
    void initGraphics(int scr_width, int scr_height);
    void cleanupGraphics();
    bool DrawGL(int vport_width, int vport_height);

    wxWindow* CreateMouseCapture(wxWindow* parent);

    extern const wxEventType evtMouseCapture = wxNewEventType();
    extern const wxEventType evtSetStatusText = wxNewEventType();
    extern const wxEventType evtExitFullScreen = wxNewEventType();

    // evtSetStatusText (defined above) is reused for render-thread status posts.
    // No new event type is needed — the frame already handles it.

    class GLCanvas;

    // =============================================================================
    //  RenderThread
    //
    //  Owns the GL context for its lifetime after Start() is called.
    //  The main thread creates the context and passes it in; the render thread
    //  calls SetCurrent() once and never releases it during normal operation.
    //
    //  Frame pacing: the emulator ticks at a fixed EMULATOR_FPS (50 Hz).
    //  After each drawn frame we sleep the remaining budget.  When vsync is on,
    //  SwapBuffers() already blocked for the vblank and the sleep is near-zero.
    // =============================================================================
    class RenderThread : public wxThread
    {
    public:
        // ZX Spectrum PAL frame rate — the rate at which OnLoop() must be called.
        // This is independent of the display refresh rate.
        static constexpr int EMULATOR_FPS = 50;

        RenderThread(GLCanvas* canvas, wxGLContext* ctx,
            int init_w, int init_h)
            : wxThread(wxTHREAD_JOINABLE)
            , m_canvas(canvas)
            , m_ctx(ctx)
            , m_init_w(init_w)
            , m_init_h(init_h)
            , m_running(true)
        {
        }

        // Thread-safe: called from main thread to request a clean stop.
        void RequestStop() { m_running.store(false, std::memory_order_relaxed); }

    protected:
        virtual ExitCode Entry() override;

    private:
        void PostStatus(const wxString& text);

        GLCanvas* m_canvas;
        wxGLContext* m_ctx;
        int               m_init_w, m_init_h;   // resolution for initGraphics()
        std::atomic<bool> m_running;
    };

    // =============================================================================
    //  GLCanvas
    // =============================================================================
    class GLCanvas : public wxGLCanvas
    {
        typedef wxGLCanvas eInherited;
    public:
        GLCanvas(wxWindow* parent);
        virtual ~GLCanvas();

        // Called by RenderThread to acquire the GL context on the render thread.
        // Must only be called once, before any GL work begins.
        void MakeCurrentOnRenderThread() { m_ctx->SetCurrent(*this); }

        // Called by RenderThread after each drawn frame.
        void SwapGL() { SwapBuffers(); }

        // Thread-safe viewport size read (render thread calls this every frame).
        wxSize GetViewportSize() const
        {
            // GetClientSize() is documented as safe to call from non-UI threads
            // on all platforms wxWidgets supports; it reads a cached value.
            return GetClientSize();
        }

        // Post a status string to the parent frame from any thread.
        // Reuses evtSetStatusText which the frame already handles.
        void PostStatusText(const wxString& text)
        {
            wxCommandEvent ev(evtSetStatusText);
            ev.SetString(text);
            wxQueueEvent(GetParent(), ev.Clone());
        }

    private:
        // -------------------------------------------------------------------------
        // OnPaint: still needed so wxWidgets doesn't fill the window with garbage
        // on expose events.  The render thread is doing the real drawing; here we
        // just validate the region so the OS stops sending WM_PAINT/Expose.
        // -------------------------------------------------------------------------
        void OnPaint(wxPaintEvent& /*event*/)
        {
            wxPaintDC dc(this); // must be created to validate the paint region
            // Do NOT call SetCurrent() or any GL here — the context belongs to
            // the render thread.  The next render-thread frame will repaint.
        }

        void OnEraseBackground(wxEraseEvent& /*event*/) {} // prevent flicker

        void OnKeydown(wxKeyEvent& event);
        void OnKeyup(wxKeyEvent& event);
        void OnKillFocus(wxFocusEvent& event);
        void OnMouseKey(wxMouseEvent& event);
        void OnMouseCapture(wxCommandEvent& event);
        void OnJoystickEvent(wxJoystickEvent& event);

        static std::pair<int, int> getMaxDisplayResolution();

        static int canvas_attr[];
        DECLARE_EVENT_TABLE()

        wxGLContext* m_ctx = nullptr;
        RenderThread* m_thread = nullptr;
        wxWindow* mouse_capture = nullptr;
        eWxJoystick* joysticks[2] = { nullptr, nullptr };
    };

    int GLCanvas::canvas_attr[] = { WX_GL_RGBA, WX_GL_DOUBLEBUFFER, 0 };

    BEGIN_EVENT_TABLE(GLCanvas, wxGLCanvas)
        EVT_PAINT(GLCanvas::OnPaint)
        EVT_ERASE_BACKGROUND(GLCanvas::OnEraseBackground)
        // OnIdle is intentionally absent — the render thread drives the loop.
        EVT_KEY_DOWN(GLCanvas::OnKeydown)
        EVT_KEY_UP(GLCanvas::OnKeyup)
        EVT_LEFT_DOWN(GLCanvas::OnMouseKey)
        EVT_KILL_FOCUS(GLCanvas::OnKillFocus)
        EVT_COMMAND(wxID_ANY, evtMouseCapture, GLCanvas::OnMouseCapture)
        EVT_JOYSTICK_EVENTS(GLCanvas::OnJoystickEvent)
        END_EVENT_TABLE()

        // =============================================================================
        //  GLCanvas::GLCanvas
        // =============================================================================
        GLCanvas::GLCanvas(wxWindow* parent)
        : eInherited(parent, wxID_ANY, canvas_attr)
    {
        // --- Create GL context (main thread, not yet current) ---
        wxGLContextAttrs ctx_attrs;
        ctx_attrs.PlatformDefaults().CoreProfile().OGLVersion(3, 0).EndList();
        m_ctx = new wxGLContext(this, nullptr, &ctx_attrs);
        if (!m_ctx->IsOK())
        {
            wxMessageBox("An OpenGL 3.0 capable driver is required.\nThe app will end now.",
                "OpenGL version error", wxOK | wxICON_INFORMATION, this);
            delete m_ctx;
            m_ctx = nullptr;
            return;
        }

        // --- Determine FBO / init resolution (same logic as original) ---
        auto [init_w, init_h] = getMaxDisplayResolution();

        // --- Start render thread ---
        // The thread calls SetCurrent() itself; we must NOT call it here.
        m_thread = new RenderThread(this, m_ctx, init_w, init_h);
        if (m_thread->Create() != wxTHREAD_NO_ERROR ||
            m_thread->Run() != wxTHREAD_NO_ERROR)
        {
            wxMessageBox("Failed to start render thread.", "Error",
                wxOK | wxICON_ERROR, this);
            delete m_thread;
            m_thread = nullptr;
        }

        // --- Joysticks (unchanged from original) ---
        joysticks[0] = new eWxJoystick(this, wxJOYSTICK1);
        joysticks[1] = new eWxJoystick(this, wxJOYSTICK2);
    }

    // =============================================================================
    //  GLCanvas::~GLCanvas
    // =============================================================================
    GLCanvas::~GLCanvas()
    {
        // Signal the render thread and block until it exits.
        // This guarantees cleanupGraphics() has run and no GL calls are in flight
        // before we delete the context below.
        if (m_thread)
        {
            m_thread->RequestStop();
            m_thread->Wait(); // blocks; render thread calls cleanupGraphics() then exits
            delete m_thread;
            m_thread = nullptr;
        }

        // Safe to delete the context now — the thread that owned it has exited.
        delete m_ctx;
        m_ctx = nullptr;

        delete joysticks[0];
        delete joysticks[1];
    }

    // =============================================================================
    //  RenderThread::Entry
    // =============================================================================
    wxThread::ExitCode RenderThread::Entry()
    {
        // Acquire the GL context on this thread.  All GL calls from here on must
        // stay on this thread; the main thread must not call any GL after this.
        m_canvas->MakeCurrentOnRenderThread();

        // GLEW and graphics init — same as original Paint(), but done once here.
        initGlew();
        initGraphics(m_init_w, m_init_h);

        bool vsync_active = false;

        using Clock = std::chrono::steady_clock;
        using Ms = std::chrono::duration<double, std::milli>;
        using NsInt = std::chrono::nanoseconds;

        // ---------------------------------------------------------------------------
        // Two independent accumulators:
        //
        //   next_emulator_tick — when the next OnLoop() call is due.
        //                        Always advances by exactly EMULATOR_PERIOD_NS
        //                        (20 ms), regardless of display refresh rate.
        //                        This keeps the Z80 running at a stable 50 Hz.
        //
        //   next_render_tick   — when the next DrawGL() call is due.
        //                        Only used when vsync is OFF; when vsync is ON,
        //                        SwapBuffers() provides the pacing naturally.
        //
        // Decoupling is essential: on a 60 Hz display with vsync on, DrawGL runs
        // at 60 Hz while OnLoop must still run at 50 Hz.  Tying them together
        // would either run the Z80 at 60 Hz (too fast) or drop render frames.
        // ---------------------------------------------------------------------------
        static constexpr long long EMULATOR_PERIOD_NS = 1000000000LL / EMULATOR_FPS; // 20 ms

        auto now = Clock::now();
        auto next_emulator_tick = now;
        auto next_render_tick = now;

        while (m_running.load(std::memory_order_relaxed))
        {
            // --- Quit check ---
            if (OpQuit())
            {
                wxQueueEvent(m_canvas->GetParent(), new wxCloseEvent(wxEVT_CLOSE_WINDOW));
                break;
            }

            now = Clock::now();
            const bool full_speed = Handler()->FullSpeed();

            // --- VSync: only call the driver when the desired state changes ---
            {
                bool want_vsync = !full_speed;
                if (vsync_active != want_vsync)
                {
                    vsync_active = want_vsync;
                    VsyncGL(vsync_active);
                }
            }

            // --- Emulator tick(s) ---
            // In normal mode: tick once per 20 ms.  Skip if not yet due.
            // In full-speed mode: tick as fast as possible (no rate cap).
            if (full_speed || now >= next_emulator_tick)
            {
                const char* err = Handler()->OnLoop();
                if (err)
                {
                    wxCommandEvent ev(evtSetStatusText);
                    ev.SetString(wxConvertMB2WX(err));
                    wxQueueEvent(m_canvas->GetParent(), ev.Clone());
                }
                OnLoopSound();

                if (!full_speed)
                {
                    next_emulator_tick += NsInt(EMULATOR_PERIOD_NS);
                    // If we've fallen more than one period behind (e.g. system
                    // was suspended), reset rather than scheduling a burst of
                    // catch-up ticks that would overload the sound buffer.
                    if (next_emulator_tick < now)
                        next_emulator_tick = now + NsInt(EMULATOR_PERIOD_NS);
                }
            }

            // --- Render ---
            // When vsync is on: render every iteration — SwapBuffers() provides
            // the cadence (60 Hz, 144 Hz, etc.) without any extra sleep.
            // When vsync is off: render only when the render accumulator is due,
            // so we don't busy-spin submitting frames faster than the emulator
            // can produce new pixel data.
            bool should_render = vsync_active || full_speed || (now >= next_render_tick);
            if (should_render)
            {
                wxSize sz = m_canvas->GetViewportSize();
                if (sz.x > 0 && sz.y > 0)
                {
                    if (DrawGL(sz.x, sz.y))
                        m_canvas->SwapGL();
                }

                if (!vsync_active && !full_speed)
                {
                    next_render_tick += NsInt(EMULATOR_PERIOD_NS);
                    if (next_render_tick < now)
                        next_render_tick = now + NsInt(EMULATOR_PERIOD_NS);
                }
            }

            // --- Sleep ---
            // Compute when the next event of any kind is due and sleep until then.
            // This prevents busy-spinning while still waking up on time.
            if (!full_speed)
            {
                now = Clock::now();
                auto next_event = next_emulator_tick;
                if (!vsync_active && next_render_tick < next_event)
                    next_event = next_render_tick;

                if (next_event > now)
                {
                    auto sleep_ns = Ms(next_event - now).count();
                    // Leave a 1 ms margin to account for OS scheduler latency.
                    if (sleep_ns > 1.0)
                        wxMilliSleep(static_cast<unsigned long>(sleep_ns - 1.0));
                }
            }
            else
            {
                // Full-speed: yield so we don't monopolise the core.
                // wxMilliSleep(1) is more reliable than (0) on Windows where
                // Sleep(0) only yields to equal-or-higher priority threads.
                wxMilliSleep(1);
            }
        }

        cleanupGraphics();
        return 0;
    }

    // =============================================================================
    //  Input handlers — unchanged from original, still on main thread
    // =============================================================================
    void GLCanvas::OnKeydown(wxKeyEvent& event)
    {
        int key = event.GetKeyCode();
        if (key == WXK_ESCAPE)
        {
            if (mouse_capture)
                mouse_capture->Close();
            else
            {
                wxCommandEvent ev(evtExitFullScreen);
                wxPostEvent(this, ev);
            }
            return;
        }
        dword flags = KF_DOWN | OpJoyKeyFlags();
        if (event.AltDown())    flags |= KF_ALT;
        if (event.ShiftDown())  flags |= KF_SHIFT;
        TranslateKey(key, flags);
        Handler()->OnKey(key, flags);
    }

    void GLCanvas::OnKeyup(wxKeyEvent& event)
    {
        int key = event.GetKeyCode();
        dword flags = 0;
        if (event.AltDown())    flags |= KF_ALT;
        if (event.ShiftDown())  flags |= KF_SHIFT;
        TranslateKey(key, flags);
        Handler()->OnKey(key, OpJoyKeyFlags());
    }

    void GLCanvas::OnMouseKey(wxMouseEvent& event)
    {
        event.Skip();
#ifndef _MAC
        if (!mouse_capture)
            mouse_capture = CreateMouseCapture(this);
#endif//_MAC
    }

    void GLCanvas::OnKillFocus(wxFocusEvent& event)
    {
        SAFE_CALL(mouse_capture)->Close();
    }

    void GLCanvas::OnMouseCapture(wxCommandEvent& event)
    {
        event.Skip();
        if (!event.GetId())
            mouse_capture = nullptr;
    }

    void GLCanvas::OnJoystickEvent(wxJoystickEvent& event)
    {
        switch (event.GetJoystick())
        {
        case wxJOYSTICK1: joysticks[0]->OnEvent(event); break;
        case wxJOYSTICK2: joysticks[1]->OnEvent(event); break;
        }
    }

    // =============================================================================
    //  getMaxDisplayResolution — unchanged from original
    // =============================================================================
    std::pair<int, int> GLCanvas::getMaxDisplayResolution()
    {
        int displayCount = wxDisplay::GetCount();
        if (displayCount == 0)
            return { -1, -1 };

        int maxWidth = -1;
        int maxHeight = -1;
        for (int i = 0; i < displayCount; ++i)
        {
            wxDisplay display(i);
            if (!display.IsOk()) continue;
            wxRect geometry = display.GetGeometry();
            int w = geometry.GetWidth();
            int h = geometry.GetHeight();
            if (w * h > maxWidth * maxHeight)
            {
                maxWidth = w;
                maxHeight = h;
            }
        }
        return (maxWidth == -1) ? std::make_pair(-1, -1)
            : std::make_pair(maxWidth, maxHeight);
    }

    // =============================================================================
    //  LightweightShadersMessage
    //
    //  Called from DrawGL() on the render thread.  Must not touch wxWindow.
    //  Post through the canvas instead.
    // =============================================================================
    static GLCanvas* g_canvas = nullptr; // set in CreateGLCanvas

    void LightweightShadersMessage(bool prev_use_lightweight, bool use_lightweight)
    {
        if (prev_use_lightweight == use_lightweight) return;
        if (!g_canvas) return;
        g_canvas->PostStatusText(use_lightweight
            ? "Lightweight shader enabled"
            : "Full-quality shader enabled");
    }

    // =============================================================================
    //  CreateGLCanvas
    // =============================================================================
    wxWindow* CreateGLCanvas(wxWindow* parent)
    {
        GLCanvas* canvas = new GLCanvas(parent);
        g_canvas = canvas;
        return canvas;
    }

}//namespace xPlatform

#endif//USE_WXWIDGETS
