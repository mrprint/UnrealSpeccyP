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
#include <mutex>
#include <condition_variable>
#include <wx/wx.h>
#include <wx/glcanvas.h>
#include <wx/display.h>
#include <wx/thread.h>
#include "wx_joystick.h"

#if !defined(__WXMSW__)
#include <GL/glx.h>
#endif

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

        void RequestStop();

    protected:
        virtual ExitCode Entry() override;

    private:
        void PostStatus(const wxString& text);

        GLCanvas* m_canvas;
        wxGLContext* m_ctx;
        int               m_init_w, m_init_h;
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

        void MakeCurrentOnRenderThread() { m_ctx->SetCurrent(*this); }
        void SwapGL() { SwapBuffers(); }

        // Thread-safe viewport size: updated on the main thread via EVT_SIZE
        // (and once at construction), read by the render thread each frame.
        // wxWindow::GetClientSize() must only be called from the main thread;
        // calling it from the render thread races with window resize events on GTK.
        wxSize GetViewportSize() const
        {
            return wxSize(
                m_viewport_w.load(std::memory_order_acquire),
                m_viewport_h.load(std::memory_order_acquire));
        }

        void UpdateViewportSize()
        {
            wxSize sz = GetClientSize();
            m_viewport_w.store(sz.x, std::memory_order_release);
            m_viewport_h.store(sz.y, std::memory_order_release);
        }

        // Interruptible sleep: PauseRender() notifies this to wake the render
        // thread immediately instead of waiting up to ~19ms for the sleep to expire.
        std::mutex              m_sleep_mutex;
        std::condition_variable m_sleep_cv;

        // Emulator state mutex.
        // Held by the render thread during OnLoop()/OnLoopSound()/VideoData(),
        // and by the main thread for any Handler() call or option Apply() that
        // touches emulator internals (file load, reset, key, tape, options).
        // Recursive so re-entrant handler callbacks are safe.
        std::recursive_mutex    m_emu_mutex;
        void LockEmu()   { m_emu_mutex.lock();   }
        void UnlockEmu() { m_emu_mutex.unlock(); }

        std::atomic<bool>       m_render_paused{ false };

        void PostStatusText(const wxString& text)
        {
            wxCommandEvent ev(evtSetStatusText);
            ev.SetString(text);
            wxQueueEvent(GetParent(), ev.Clone());
        }

        // Render thread polls this before calling SetCurrent().
        // Returns true only after the window has been fully realized on screen.
        bool IsShownForGL() const { return m_shown.load(std::memory_order_acquire); }

        // ---------------------------------------------------------------------------
        // PauseRender / ResumeRender
        //
        // Called from the MAIN thread before mutating any render option, and
        // released afterwards.  The render thread checks m_render_paused at the
        // start of each loop iteration and blocks there, so by the time
        // PauseRender() returns the GPU command queue has been drained and the
        // render thread is idle.  Safe to call multiple times (ref-counted).
        // ---------------------------------------------------------------------------
        void PauseRender()
        {
            {
                std::lock_guard<std::mutex> lk(m_render_mutex);
                ++m_render_pause_count;
                m_render_paused.store(true, std::memory_order_release);
            }
            // Wake the render thread if it is in the interruptible sleep
            // so it reaches RenderThreadMaybePause() without delay.
            m_sleep_cv.notify_one();
            std::unique_lock<std::mutex> lk(m_render_mutex);
            m_render_cv_main.wait(lk, [this] { return m_render_idle; });
        }

        void ResumeRender()
        {
            {
                std::lock_guard<std::mutex> lk(m_render_mutex);
                if (m_render_pause_count > 0)
                    --m_render_pause_count;
                if (m_render_pause_count == 0)
                    m_render_paused.store(false, std::memory_order_release);
            }
            m_render_cv_thread.notify_one();
        }

        // Called by the render thread at the top of each loop iteration.
        // Blocks while paused and signals the main thread that we are idle.
        //
        // glFinish() is called before signalling idle so that any in-flight GPU
        // commands from the previous DrawGL()/SwapGL() are fully retired before
        // the main thread starts mutating options.  Without this, options could
        // change while the GPU is still consuming the previous frame's uniforms.
        void RenderThreadMaybePause()
        {
            if (!m_render_paused.load(std::memory_order_acquire))
                return;

            std::unique_lock<std::mutex> lk(m_render_mutex);
            if (!m_render_paused.load(std::memory_order_relaxed))
                return;

            // Drain the GPU pipeline before telling the main thread we are idle.
            glFinish();

            m_render_idle = true;
            m_render_cv_main.notify_one();      // wake PauseRender() caller
            // Sleep until ResumeRender() clears the pause OR RequestStop() sets
            // m_stop_requested (so shutdown is never blocked by a pending pause).
            m_render_cv_thread.wait(lk, [this]
                {
                    return !m_render_paused.load(std::memory_order_relaxed)
                        || m_stop_requested.load(std::memory_order_relaxed);
                });
            m_render_idle = false;
        }

        // Called by RenderThread::RequestStop() to unblock a paused render
        // thread without touching the pause ref-count.
        void RequestStopWhilePaused()
        {
            m_stop_requested.store(true, std::memory_order_release);
            m_sleep_cv.notify_one();
            m_render_cv_thread.notify_one();
        }

    private:
        void OnPaint(wxPaintEvent& /*event*/)
        {
            wxPaintDC dc(this); // must be created to validate the paint region
            // Signal the render thread that the window is now fully realized.
            // EVT_PAINT is guaranteed to fire only after the window is mapped
            // on screen, making it the most reliable trigger for SetCurrent().
            // The render thread must NOT call any GL here — just set the flag.
            m_shown.store(true, std::memory_order_release);
        }

        void OnEraseBackground(wxEraseEvent& /*event*/) {}

        void OnSize(wxSizeEvent& event);
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

        // Set to true by OnPaint() when the window is first painted/mapped.
        // The render thread waits on this before calling SetCurrent().
        std::atomic<bool> m_shown{ false };

        // Render-pause mechanism.
        // m_render_paused: set by main thread, read by render thread each loop.
        // m_render_pause_count: ref-count so nested pauses are safe.
        // m_render_idle: set by render thread when it has acknowledged the pause.
        // m_render_cv_main: wakes PauseRender() once the render thread is idle.
        // m_render_cv_thread: wakes the render thread when ResumeRender() is called.
        std::mutex              m_render_mutex;
        std::condition_variable m_render_cv_main;
        std::condition_variable m_render_cv_thread;
        std::atomic<bool>       m_stop_requested{ false }; // set by RequestStopWhilePaused()
        int                     m_render_pause_count{ 0 };
        bool                    m_render_idle{ false };

        // Viewport dimensions cached atomically so the render thread can read
        // them without calling wxWindow::GetClientSize() (which is not
        // thread-safe on GTK).  Written by OnSize() and the constructor,
        // both of which run on the main thread.
        std::atomic<int>        m_viewport_w{ 0 };
        std::atomic<int>        m_viewport_h{ 0 };
    };

    void RenderThread::RequestStop()
    {
        m_running.store(false, std::memory_order_relaxed);
        // Wake the render thread if it is currently blocked inside
        // RenderThreadMaybePause().  We must NOT call ResumeRender() here
        // because that decrements m_render_pause_count and would corrupt
        // the ref-count when no matching PauseRender() is in flight.
        // A separate flag avoids the ownership problem entirely.
        m_canvas->RequestStopWhilePaused();
    }

    int GLCanvas::canvas_attr[] = { WX_GL_RGBA, WX_GL_DOUBLEBUFFER, 0 };

    BEGIN_EVENT_TABLE(GLCanvas, wxGLCanvas)
        EVT_SIZE(GLCanvas::OnSize)
        EVT_PAINT(GLCanvas::OnPaint)
        EVT_ERASE_BACKGROUND(GLCanvas::OnEraseBackground)
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

        auto [init_w, init_h] = getMaxDisplayResolution();

        // The render thread will wait in Entry() until IsShownForGL() returns
        // true (i.e. until OnShow fires), so it is safe to start it here even
        // though the window is not yet visible.
        m_thread = new RenderThread(this, m_ctx, init_w, init_h);
        if (m_thread->Create() != wxTHREAD_NO_ERROR ||
            m_thread->Run() != wxTHREAD_NO_ERROR)
        {
            wxMessageBox("Failed to start render thread.", "Error",
                wxOK | wxICON_ERROR, this);
            delete m_thread;
            m_thread = nullptr;
        }

        // Seed the atomic viewport cache so the render thread has a valid
        // size from its very first frame (before the first EVT_SIZE fires).
        UpdateViewportSize();

        joysticks[0] = new eWxJoystick(this, wxJOYSTICK1);
        joysticks[1] = new eWxJoystick(this, wxJOYSTICK2);
    }

    // =============================================================================
    //  GLCanvas::~GLCanvas
    // =============================================================================
    GLCanvas::~GLCanvas()
    {
        if (m_thread)
        {
            m_thread->RequestStop();
            // Also unblock the thread if it is still waiting for the window to
            // be shown (e.g. the app is closed before the frame is ever shown).
            m_shown.store(true, std::memory_order_release);
            m_thread->Wait();
            delete m_thread;
            m_thread = nullptr;
        }

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
        // Wait until the canvas window is fully realized on screen before
        // calling SetCurrent().  On X11/GLX, SetCurrent() asserts that the
        // underlying XWindow exists and is mapped; calling it too early
        // (before the first Expose/Map event) triggers the
        // "window must be shown" assertion in wxGLContext::SetCurrent().
        while (m_running.load(std::memory_order_relaxed) && !m_canvas->IsShownForGL())
            wxMilliSleep(10);

        // If we were stopped before the window ever appeared, exit cleanly.
        if (!m_running.load(std::memory_order_relaxed))
            return 0;

        // Acquire the GL context on this thread. All GL calls must stay here.
        m_canvas->MakeCurrentOnRenderThread();

        initGlew();
        initGraphics(m_init_w, m_init_h);

        bool vsync_active = false;

        using Clock = std::chrono::steady_clock;
        using Ms = std::chrono::duration<double, std::milli>;
        using NsInt = std::chrono::nanoseconds;

        static constexpr long long EMULATOR_PERIOD_NS = 1000000000LL / EMULATOR_FPS;

        auto now = Clock::now();
        auto next_emulator_tick = now;
        auto next_render_tick = now;

        while (m_running.load(std::memory_order_relaxed))
        {
            // Block here (cheaply) while the main thread is mutating render options.
            // SwapBuffers() has already returned so the GPU pipeline is idle.
            m_canvas->RenderThreadMaybePause();

            if (!m_running.load(std::memory_order_relaxed))
                break;

            if (OpQuit())
            {
                wxQueueEvent(m_canvas->GetParent(), new wxCloseEvent(wxEVT_CLOSE_WINDOW));
                break;
            }

            now = Clock::now();

            // Hold the emulator mutex for the duration of OnLoop(),
            // OnLoopSound(), and the VideoData/VideoFrame reads inside DrawGL().
            // The main thread acquires the same mutex before any Handler() call
            // or option Apply() that touches emulator internals, ensuring the
            // two threads never operate on emulator state concurrently.
            bool do_emu_tick;
            bool full_speed;
            bool did_draw = false;
            {
                std::lock_guard<std::recursive_mutex> emu_lk(m_canvas->m_emu_mutex);
                full_speed = Handler()->FullSpeed();
                do_emu_tick = full_speed || (now >= next_emulator_tick);

                {
                    bool want_vsync = !full_speed;
                    if (vsync_active != want_vsync)
                    {
                        vsync_active = want_vsync;
#ifdef _LINUX
                        Display* dpy = glXGetCurrentDisplay();
                        if (dpy) XLockDisplay(dpy);
                        VsyncGL(vsync_active);
                        if (dpy) XUnlockDisplay(dpy);
#else
                        VsyncGL(vsync_active);
#endif
                    }
                }

                if (do_emu_tick)
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
                        if (next_emulator_tick < now)
                            next_emulator_tick = now + NsInt(EMULATOR_PERIOD_NS);
                    }
                }

                bool should_render = vsync_active || full_speed || (now >= next_render_tick);
                did_draw = false;
                if (should_render && m_running.load(std::memory_order_relaxed))
                {
                    wxSize sz = m_canvas->GetViewportSize();
                    if (sz.x > 0 && sz.y > 0)
                        // DrawGL reads VideoData/VideoFrame — inside emu lock.
                        did_draw = DrawGL(sz.x, sz.y);

                    if (!vsync_active && !full_speed)
                    {
                        next_render_tick += NsInt(EMULATOR_PERIOD_NS);
                        if (next_render_tick < now)
                            next_render_tick = now + NsInt(EMULATOR_PERIOD_NS);
                    }
                }
            } // emu_lk released here

            // SwapGL (and potential vsync stall) runs outside the emu lock
            // so the main thread is never blocked during vblank wait.
            if (did_draw && m_running.load(std::memory_order_relaxed))
                m_canvas->SwapGL();

            // Interruptible sleep: wakes immediately if paused or stopped.
            {
                std::unique_lock<std::mutex> lk(m_canvas->m_sleep_mutex);
                if (!full_speed)
                {
                    now = Clock::now();
                    auto next_event = next_emulator_tick;
                    if (!vsync_active && next_render_tick < next_event)
                        next_event = next_render_tick;

                    if (next_event > now)
                    {
                        auto wake_at = next_event - NsInt(1000000LL);
                        m_canvas->m_sleep_cv.wait_until(lk, wake_at,
                            [this] {
                                return m_canvas->m_render_paused.load(
                                    std::memory_order_relaxed)
                                    || !m_running.load(std::memory_order_relaxed);
                            });
                    }
                }
                else
                {
                    m_canvas->m_sleep_cv.wait_for(lk, std::chrono::milliseconds(1),
                        [this] {
                            return m_canvas->m_render_paused.load(
                                std::memory_order_relaxed)
                                || !m_running.load(std::memory_order_relaxed);
                        });
                }
            }
        }

        cleanupGraphics();

        // Release the GL context from this thread before the main thread
        // deletes it in ~GLCanvas().  We use raw OpenGL calls which are
        // available after initGlew() has run.  This prevents GLXBadContextTag
        // on Linux when the main thread destroys the context while it is
        // still marked current on the render thread.
#if defined(__WXMSW__)
        // Release the GL context without leaking the HDC.
        // wglGetCurrentDC() returns the DC that wx passed to wglMakeCurrent()
        // internally; calling ReleaseDC() on it here would be incorrect because
        // wx owns that DC's lifetime.  Passing nullptr for both arguments simply
        // unbinds the context from this thread without touching the DC at all.
        wglMakeCurrent(nullptr, nullptr);
#else
        // On Linux/GLX: passing nullptr for both drawable and context releases it.
        // glXMakeCurrent requires the Display pointer; we stored nothing, so use
        // a null-canvas trick — simply bind a non-existent context to release.
        // The safest portable way with wxWidgets is to call the GLX/WGL directly
        // via the platform GL header which is already included by GLEW.
        glXMakeCurrent(glXGetCurrentDisplay(), None, nullptr);
#endif

        return 0;
    }

    // =============================================================================
    //  Input handlers
    // =============================================================================
    // =============================================================================
    //  GLCanvas::OnSize
    // =============================================================================
    void GLCanvas::OnSize(wxSizeEvent& event)
    {
        // Keep the atomic viewport cache in sync so the render thread always
        // sees the current client dimensions without calling GetClientSize().
        UpdateViewportSize();
        event.Skip(); // let wx do its default resize processing
    }

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
        { std::lock_guard<std::recursive_mutex> lk(m_emu_mutex); Handler()->OnKey(key, flags); }
    }

    void GLCanvas::OnKeyup(wxKeyEvent& event)
    {
        int key = event.GetKeyCode();
        dword flags = 0;
        if (event.AltDown())    flags |= KF_ALT;
        if (event.ShiftDown())  flags |= KF_SHIFT;
        TranslateKey(key, flags);
        { std::lock_guard<std::recursive_mutex> lk(m_emu_mutex); Handler()->OnKey(key, OpJoyKeyFlags()); }
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
    //  getMaxDisplayResolution
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
    // =============================================================================
    static GLCanvas* g_canvas = nullptr;

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

    // =============================================================================
    //  PauseGLCanvas / ResumeGLCanvas
    //
    //  Called from wx_frame.cpp (main thread) before and after mutating any
    //  render option (zoom, scanlines, gigascreen, PAL effects, etc.).
    //  Guarantees the render thread is idle (GPU pipeline drained) for the
    //  duration, preventing mid-frame option changes that stall the driver.
    // =============================================================================
    void PauseGLCanvas()  { if (g_canvas) g_canvas->PauseRender();  }
    void ResumeGLCanvas() { if (g_canvas) g_canvas->ResumeRender(); }

    // Lock/unlock the emulator state mutex from any thread.
    // Use ScopedEmuLock (defined in wx_frame.cpp and wx_optionsdialog.cpp)
    // rather than calling these directly.
    void LockEmulator()   { if (g_canvas) g_canvas->LockEmu();   }
    void UnlockEmulator() { if (g_canvas) g_canvas->UnlockEmu(); }

}//namespace xPlatform

#endif//USE_WXWIDGETS
