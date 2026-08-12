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

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <wx/display.h>
#include <wx/glcanvas.h>
#include <wx/thread.h>
#include <wx/wx.h>
#include "wx_joystick.h"
#ifdef USE_SDL2_GAMEPAD
#include "wx_gamepad.h"
#include "joystick_mapper.h"
#endif
#include "../../devices/video_snapshot.h"

#if !defined(__WXMSW__)
#include <GL/glx.h>
#endif

namespace xPlatform
{

void OnLoopSound();
void TranslateKey(int& key, dword& flags);

void VsyncGL(bool on);
void initGlew();
bool IsNvidiaGPU();
void initGraphics(int scr_width, int scr_height);
void cleanupGraphics();
struct VideoSnapshot;
bool DrawGL(int vport_width, int vport_height, const VideoSnapshot& snap);

wxWindow* CreateMouseCapture(wxWindow* parent);

extern const wxEventType evtMouseCapture   = wxNewEventType();
extern const wxEventType evtSetStatusText  = wxNewEventType();
extern const wxEventType evtExitFullScreen = wxNewEventType();
extern const wxEventType evtCheckNvidiaWarning = wxNewEventType();

// =============================================================================
//  ViewportCache
//
//  Stores the canvas client dimensions in a single 64-bit atomic so the
//  render thread always reads a consistent (w, h) pair.  Packing both values
//  into one word eliminates the tearing window that exists when two separate
//  atomics are stored/loaded independently (a reader could observe the new w
//  but the old h between the two release stores).
//
//  Encoding: bits 63-32 = height (int32_t), bits 31-0 = width (int32_t).
//
//  Written exclusively from the main thread (constructor and OnSize).
//  Read from any thread without a lock.
// =============================================================================
class ViewportCache
{
public:
    void store(int w, int h) noexcept
    {
        m_wh.store(pack(w, h), std::memory_order_release);
    }

    wxSize load() const noexcept
    {
        const uint64_t v = m_wh.load(std::memory_order_acquire);
        return { static_cast<int>(static_cast<int32_t>(v & 0xFFFFFFFFu)),
                 static_cast<int>(static_cast<int32_t>(v >> 32)) };
    }

private:
    static uint64_t pack(int w, int h) noexcept
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(h)) << 32)
             |  static_cast<uint64_t>(static_cast<uint32_t>(w));
    }

    std::atomic<uint64_t> m_wh{ 0 };
};

// =============================================================================
//  RenderSync
//
//  Owns the pause/resume handshake between the main thread and the render
//  thread, plus the interruptible inter-frame sleep.
//
//  Main-thread API  : Pause() / Resume()  (ref-counted, nestable)
//  Render-thread API: MaybePause()        at the top of each frame loop
//                     SleepUntil() / SleepFor()  between frames
//  Shutdown API     : RequestStop()       unblocks everything without
//                     touching the pause ref-count
//
//  --- Pause/MaybePause protocol ---
//
//  Pause() holds m_mutex continuously from the moment it sets m_paused
//  through the wait on m_idle.  This closes the race window where a
//  concurrent Resume() could clear m_paused before Pause() reaches its
//  wait, causing Pause() to block forever waiting for an m_idle that will
//  never be set.
//
//  m_idle lifecycle (all accesses under m_mutex):
//    false  ->  MaybePause() sets true and notifies the main thread,
//               then waits on m_cv_thread.
//    true   ->  Pause() wakes, resets m_idle to false, then returns.
//
//  Resetting m_idle inside Pause() (not inside MaybePause()) is essential
//  for ref-counted nesting: a second nested Pause() call must block again
//  rather than see the stale true left by the previous round and return
//  prematurely.
// =============================================================================
class RenderSync
{
public:
    // -------------------------------------------------------------------------
    //  Main-thread side
    // -------------------------------------------------------------------------

    // Block until the render thread acknowledges the pause and the GPU
    // pipeline is fully drained.  Ref-counted so nested calls are safe.
    void Pause()
    {
        // Hold m_mutex through the entire wait.  Without continuous ownership
        // there is a window between releasing lock #1 (after setting m_paused)
        // and acquiring lock #2 (before waiting) during which Resume() could
        // fire and clear the flag — causing this call to wait on m_idle forever.
        std::unique_lock<std::mutex> lk(m_mutex);
        ++m_pause_count;
        m_paused.store(true, std::memory_order_release);

        // Wake the render thread if it is in the inter-frame sleep so it
        // reaches MaybePause() without waiting up to ~19 ms.
        // notify_one() is safe to call while holding m_mutex.
        m_sleep_cv.notify_one();

        m_cv_main.wait(lk, [this] { return m_idle; });

        // Reset m_idle while still holding the lock.  A subsequent nested
        // Pause() call will then correctly block again rather than seeing the
        // stale true and returning without waiting for the render thread.
        m_idle = false;
    }

    // Release one level of the pause ref-count.  Resumes the render thread
    // when the count reaches zero.
    void Resume()
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_pause_count > 0)
                --m_pause_count;
            if (m_pause_count == 0)
                m_paused.store(false, std::memory_order_release);
        }
        m_cv_thread.notify_one();
    }

    // -------------------------------------------------------------------------
    //  Render-thread side
    // -------------------------------------------------------------------------

    // Called at the top of each render loop iteration.
    // Returns immediately when not paused.  When paused:
    //   1. Calls glFinish() to drain the GPU pipeline.
    //   2. Sets m_idle and signals the main thread.
    //   3. Sleeps until Resume() or RequestStop() is called.
    void MaybePause()
    {
        if (!m_paused.load(std::memory_order_acquire))
            return;

        std::unique_lock<std::mutex> lk(m_mutex);

        // Re-check under the lock: Pause() may have been called and
        // Resume()d between the atomic check above and acquiring m_mutex.
        if (!m_paused.load(std::memory_order_relaxed))
            return;

        // Drain in-flight GPU commands before telling the main thread we are
        // idle, so it never mutates shader uniforms while the GPU is still
        // consuming the previous frame.
        glFinish();

        m_idle = true;
        m_cv_main.notify_one();  // wake Pause() caller

        m_cv_thread.wait(lk, [this]
        {
            return !m_paused.load(std::memory_order_relaxed)
                || m_stop.load(std::memory_order_relaxed);
        });
        // Do NOT reset m_idle here.  Pause() resets it after waking so that
        // a second nested Pause() correctly re-waits for this thread.
    }

    // Interruptible sleep used between frames.
    // Wakes immediately if the thread is paused or stopped.
    template<class TimePoint>
    void SleepUntil(TimePoint wake_at)
    {
        std::unique_lock<std::mutex> lk(m_sleep_mutex);
        m_sleep_cv.wait_until(lk, wake_at, [this]
        {
            return m_paused.load(std::memory_order_relaxed)
                || m_stop.load(std::memory_order_relaxed);
        });
    }

    void SleepFor(std::chrono::milliseconds ms)
    {
        std::unique_lock<std::mutex> lk(m_sleep_mutex);
        m_sleep_cv.wait_for(lk, ms, [this]
        {
            return m_paused.load(std::memory_order_relaxed)
                || m_stop.load(std::memory_order_relaxed);
        });
    }

    // -------------------------------------------------------------------------
    //  Shutdown
    // -------------------------------------------------------------------------

    // Unblocks MaybePause() and any active sleep without modifying the pause
    // ref-count.  Safe to call from any thread.
    void RequestStop()
    {
        m_stop.store(true, std::memory_order_release);
        m_sleep_cv.notify_one();
        m_cv_thread.notify_one();
    }

    // -------------------------------------------------------------------------
    //  Informational queries — do not use as synchronisation primitives
    // -------------------------------------------------------------------------
    bool IsPaused()        const noexcept { return m_paused.load(std::memory_order_relaxed); }
    bool IsStopRequested() const noexcept { return m_stop.load(std::memory_order_relaxed);   }

private:
    std::mutex              m_mutex;       // guards m_pause_count, m_idle, and m_paused stores
    std::mutex              m_sleep_mutex; // guards the inter-frame sleep CV
    std::condition_variable m_cv_main;     // wakes Pause() caller when render thread is idle
    std::condition_variable m_cv_thread;   // wakes render thread on Resume() / RequestStop()
    std::condition_variable m_sleep_cv;    // wakes render thread's inter-frame sleep

    std::atomic<bool>  m_paused{ false };
    std::atomic<bool>  m_stop  { false };
    int                m_pause_count{ 0 }; // guarded by m_mutex
    bool               m_idle{ false };    // guarded by m_mutex
};

// =============================================================================
//  InputRouter
//
//  Translates raw wxWidgets input events and forwards them to Handler() while
//  holding the emulator mutex.  Joystick routing requires pointers to the two
//  eWxJoystick objects that GLCanvas owns; they are passed per-call so this
//  class holds no owning references and has no lifetime coupling to them.
//
//  ESC is NOT handled here — GLCanvas::OnKeydown intercepts it first because
//  its effect depends on mouse-capture / fullscreen state.
// =============================================================================
class InputRouter
{
public:
    explicit InputRouter(std::mutex& emu_mutex)
        : m_emu_mutex(emu_mutex)
    {}

    void OnKeydown(wxKeyEvent& event)
    {
        int   key   = event.GetKeyCode();
        dword flags = KF_DOWN | OpJoyKeyFlags();
        if (event.AltDown())   flags |= KF_ALT;
        if (event.ShiftDown()) flags |= KF_SHIFT;
        TranslateKey(key, flags);
        std::lock_guard<std::mutex> lk(m_emu_mutex);
        Handler()->OnKey(key, flags);
    }

    void OnKeyup(wxKeyEvent& event)
    {
        int   key   = event.GetKeyCode();
        dword flags = 0;
        if (event.AltDown())   flags |= KF_ALT;
        if (event.ShiftDown()) flags |= KF_SHIFT;
        TranslateKey(key, flags);
        std::lock_guard<std::mutex> lk(m_emu_mutex);
        Handler()->OnKey(key, OpJoyKeyFlags());
    }

    void OnJoystickEvent(wxJoystickEvent& event,
                         eWxJoystick* js1, eWxJoystick* js2)
    {
        switch (event.GetJoystick())
        {
        case wxJOYSTICK1: if (js1) js1->OnEvent(event); break;
        case wxJOYSTICK2: if (js2) js2->OnEvent(event); break;
        }
    }

private:
    std::mutex& m_emu_mutex;
};

// Forward declaration needed by RenderThread before GLCanvas is defined.
class GLCanvas;

// =============================================================================
//  RenderThread
//
//  Owns the GL context for its lifetime after Entry() starts.
//  The main thread creates the wxGLContext and passes it in; this thread
//  calls SetCurrent() once and retains it for the duration of the loop.
//
//  Frame pacing: the emulator ticks at a fixed EMULATOR_FPS (50 Hz).
//  After each drawn frame the remaining budget is consumed via RenderSync.
//  When vsync is on, SwapBuffers() has already blocked for the vblank and
//  the sleep wakes immediately.
// =============================================================================
class RenderThread : public wxThread
{
public:
    static constexpr int EMULATOR_FPS = 50;

    RenderThread(GLCanvas* canvas, wxGLContext* ctx, int init_w, int init_h)
        : wxThread(wxTHREAD_JOINABLE)
        , m_canvas(canvas)
        , m_ctx(ctx)
        , m_init_w(init_w)
        , m_init_h(init_h)
        , m_running(true)
    {}

    void RequestStop();

protected:
    ExitCode Entry() override;

private:
    GLCanvas*         m_canvas;
    wxGLContext*      m_ctx;     // non-owning; lifetime managed by GLCanvas
    int               m_init_w;
    int               m_init_h;
    std::atomic<bool> m_running;
};

// =============================================================================
//  GLCanvas
//
//  Thin compositor.  Owns the GL context, render thread, input peripherals,
//  and the emulator mutex.  Non-trivial behaviour is delegated to the four
//  helper classes above.
//
//  Member declaration order is load-bearing for construction order:
//    m_emu_mutex must be fully constructed before m_input (holds a reference).
//    m_sync and m_viewport have no inter-member dependencies.
//    m_thread's unique_ptr deleter joins the thread before anything else is
//    destroyed, because m_thread is declared first in the private section and
//    therefore destroyed last — but the custom RenderThreadDeleter calls
//    RequestStop()+Wait() before delete, ensuring the thread exits before
//    m_sync/m_emu_mutex/m_ctx are torn down.
//
//  m_emu_mutex is std::mutex (not recursive_mutex).  The render thread holds
//  it only for OnLoop() + OnLoopSound(); DrawGL() runs after the lock is
//  released.  The main thread acquires it via ScopedEmuLock for Handler()
//  calls.  Neither side may re-acquire the lock on the same thread.
//
//  m_emu_mutex and m_sync are public so RenderThread::Entry() can access them
//  directly without a proliferation of forwarding methods on GLCanvas.
//  g_canvas is file-static, so this does not constitute a public API.
// =============================================================================
class GLCanvas : public wxGLCanvas
{
    using eInherited = wxGLCanvas;

public:
    explicit GLCanvas(wxWindow* parent);
    ~GLCanvas() override;

    GLCanvas(const GLCanvas&)            = delete;
    GLCanvas& operator=(const GLCanvas&) = delete;

    // ---- Called by RenderThread --------------------------------------------

    void MakeCurrentOnRenderThread() { m_ctx->SetCurrent(*this); }
    void SwapGL()                    { SwapBuffers(); }

    bool IsShownForGL() const noexcept
    {
        return m_shown.load(std::memory_order_acquire);
    }

    wxSize GetViewportSize() const noexcept { return m_viewport.load(); }

    void PostStatusText(const wxString& text)
    {
        wxCommandEvent ev(evtSetStatusText);
        ev.SetString(text);
        wxQueueEvent(GetParent(), ev.Clone());
    }

    // ---- Render-pause API (forwarded from free functions in wx_frame.cpp) --

    void PauseRender()  { m_sync.Pause();  }
    void ResumeRender() { m_sync.Resume(); }

    // ---- Emulator-mutex API ------------------------------------------------

    void LockEmu()   { m_emu_mutex.lock();   }
    void UnlockEmu() { m_emu_mutex.unlock(); }

#ifdef USE_SDL2_GAMEPAD
    // Re-reads host_device_index/input mapping for both players from
    // xOptions into m_profiles. Called once from the constructor, and again
    // whenever OptionsDialog::OnOK() has just written new gamepad settings,
    // so a just-assigned device/mapping takes effect immediately instead of
    // only after the emulator is restarted (m_profiles was previously only
    // ever populated at construction time). Runs on the main thread only —
    // same as OnGamepadPoll(), which is the only other reader/writer of
    // m_profiles — so no locking is needed here.
    void ReloadGamepadProfiles();
#endif

    // Public: accessed directly by RenderThread (file-private via g_canvas).
    RenderSync  m_sync;
    std::mutex  m_emu_mutex;  // guards Handler() calls and emulator state

private:
    // ---- wx event handlers -------------------------------------------------
    void OnPaint(wxPaintEvent& event);
    void OnEraseBackground(wxEraseEvent& /*event*/) {}
    void OnSize(wxSizeEvent& event);
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

    static int canvas_attr[];
    DECLARE_EVENT_TABLE()

    // ---- Owning members ----------------------------------------------------

    // wxGLContext is not a wxObject subclass; plain delete is correct.
    std::unique_ptr<wxGLContext> m_ctx;

    // RenderThread must be joined before any other member is destroyed.
    // The custom deleter calls RequestStop() + Wait() + delete in that order,
    // which guarantees the thread has exited before m_sync / m_emu_mutex /
    // m_ctx are destroyed.
    struct RenderThreadDeleter
    {
        void operator()(RenderThread* t) const noexcept
        {
            if (!t) return;
            t->RequestStop();
            t->Wait();
            delete t;
        }
    };
    std::unique_ptr<RenderThread, RenderThreadDeleter> m_thread;

    // Joystick objects: owned, not wx-window-managed.
    std::array<std::unique_ptr<eWxJoystick>, 2> m_joysticks;

#ifdef USE_SDL2_GAMEPAD
    // SDL2 gamepad support
    wxTimer m_gamepad_timer;
    JoystickMapper m_joystick_mapper;
    std::array<JoystickProfile, 2> m_profiles;
#endif

    // ---- Value / non-owning members ----------------------------------------

    // Declared after m_emu_mutex (which is public, above) so construction
    // order is correct: InputRouter holds a reference to m_emu_mutex.
    ViewportCache m_viewport;
    InputRouter   m_input{ m_emu_mutex };

    // Non-owning: the mouse-capture child window is closed/destroyed by wx.
    wxWindow* m_mouse_capture = nullptr;

    // Set true by OnPaint() once the window is mapped on screen.
    std::atomic<bool> m_shown{ false };
};

// =============================================================================
//  RenderThread::RequestStop
// =============================================================================
void RenderThread::RequestStop()
{
    m_running.store(false, std::memory_order_relaxed);
    // Unblock MaybePause() or SleepUntil() without touching the pause
    // ref-count.  No matching Pause() is in flight from this path.
    m_canvas->m_sync.RequestStop();
}

// =============================================================================
//  GLCanvas — static data / event table
// =============================================================================
int GLCanvas::canvas_attr[] = { WX_GL_RGBA, WX_GL_DOUBLEBUFFER, 0 };

BEGIN_EVENT_TABLE(GLCanvas, wxGLCanvas)
    EVT_SIZE            (GLCanvas::OnSize)
    EVT_PAINT           (GLCanvas::OnPaint)
    EVT_ERASE_BACKGROUND(GLCanvas::OnEraseBackground)
    EVT_KEY_DOWN        (GLCanvas::OnKeydown)
    EVT_KEY_UP          (GLCanvas::OnKeyup)
    EVT_LEFT_DOWN       (GLCanvas::OnMouseKey)
    EVT_KILL_FOCUS      (GLCanvas::OnKillFocus)
    EVT_COMMAND(wxID_ANY, evtMouseCapture, GLCanvas::OnMouseCapture)
    EVT_JOYSTICK_EVENTS (GLCanvas::OnJoystickEvent)
#ifdef USE_SDL2_GAMEPAD
    EVT_TIMER(wxID_ANY, GLCanvas::OnGamepadPoll)
#endif
END_EVENT_TABLE()

// =============================================================================
//  GLCanvas::GLCanvas
// =============================================================================
GLCanvas::GLCanvas(wxWindow* parent)
    : eInherited(parent, wxID_ANY, canvas_attr)
{
    wxGLContextAttrs ctx_attrs;
    ctx_attrs.PlatformDefaults().CoreProfile().OGLVersion(3, 0).EndList();

    auto ctx = std::make_unique<wxGLContext>(this, nullptr, &ctx_attrs);
    if (!ctx->IsOK())
    {
        wxMessageBox(
            "An OpenGL 3.0 capable driver is required.\nThe app will end now.",
            "OpenGL version error", wxOK | wxICON_INFORMATION, this);
        return; // m_ctx stays null; m_thread is not started
    }
    m_ctx = std::move(ctx);

    // Seed the viewport cache before the first EVT_SIZE fires so the render
    // thread has a valid size from its very first frame.
    {
        const wxSize sz = GetClientSize();
        m_viewport.store(sz.x, sz.y);
    }

    auto [init_w, init_h] = getMaxDisplayResolution();

    // The render thread waits in Entry() until IsShownForGL() returns true,
    // so it is safe to start it here even though the window is not yet visible.
    std::unique_ptr<RenderThread, RenderThreadDeleter> thread(
        new RenderThread(this, m_ctx.get(), init_w, init_h));

    if (thread->Create() != wxTHREAD_NO_ERROR ||
        thread->Run()    != wxTHREAD_NO_ERROR)
    {
        wxMessageBox("Failed to start render thread.", "Error",
            wxOK | wxICON_ERROR, this);
        // RenderThreadDeleter will call RequestStop()+Wait()+delete safely
        // even for a thread that was Create()'d but not Run().
        return;
    }
    m_thread = std::move(thread);

    m_joysticks[0] = std::make_unique<eWxJoystick>(this, wxJOYSTICK1);
    m_joysticks[1] = std::make_unique<eWxJoystick>(this, wxJOYSTICK2);

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

// =============================================================================
//  GLCanvas::~GLCanvas
// =============================================================================
GLCanvas::~GLCanvas()
{
    // Ensure the render thread is not stuck waiting for the window to appear
    // (e.g. app closed before the frame is first shown on screen).
    if (m_thread)
        m_shown.store(true, std::memory_order_release);

    // Destruction order (reverse of declaration):
    //   m_joysticks, m_thread (RenderThreadDeleter joins here),
    //   then m_viewport / m_input / m_shown,
    //   then m_emu_mutex / m_sync (public members),
    //   then m_ctx.
    // The render thread is guaranteed exited before m_sync, m_emu_mutex,
    // and m_ctx are destroyed.
}

// =============================================================================
//  RenderThread::Entry
// =============================================================================
wxThread::ExitCode RenderThread::Entry()
{
    // Wait until the canvas window is fully realized on screen before calling
    // SetCurrent().  On X11/GLX, SetCurrent() asserts that the underlying
    // XWindow is mapped; calling it before the first Expose/Map event triggers
    // the "window must be shown" assertion inside wxGLContext::SetCurrent().
    while (m_running.load(std::memory_order_relaxed) && !m_canvas->IsShownForGL())
        wxMilliSleep(10);

    if (!m_running.load(std::memory_order_relaxed))
        return 0;

    // Acquire the GL context on this thread.  All GL calls must originate here.
    m_canvas->MakeCurrentOnRenderThread();

    initGlew();
    initGraphics(m_init_w, m_init_h);

    // Check for NVIDIA GPU and warn the user if needed.
    // Posted to the main thread because GL calls must stay on the render thread
    // but wx dialogs must run on the main thread.
    if (IsNvidiaGPU())
    {
        wxCommandEvent ev(evtCheckNvidiaWarning);
        wxQueueEvent(m_canvas->GetParent(), ev.Clone());
    }

    bool vsync_active = false;

    using Clock = std::chrono::steady_clock;
    using NsInt = std::chrono::nanoseconds;

    static constexpr long long EMULATOR_PERIOD_NS = 1'000'000'000LL / EMULATOR_FPS;
    static constexpr long long FULL_SPEED_SWAP_PERIOD_NS = 1'000'000'000LL / EMULATOR_FPS;

    auto now               = Clock::now();
    auto next_emulator_tick = now;
    auto next_render_tick   = now;
    auto next_full_speed_swap = now;

    while (m_running.load(std::memory_order_relaxed))
    {
        // Block here (cheaply) while the main thread mutates render options.
        // SwapBuffers() has already returned so the GPU pipeline is idle.
        m_canvas->m_sync.MaybePause();

        if (!m_running.load(std::memory_order_relaxed))
            break;

        if (OpQuit())
        {
            wxQueueEvent(m_canvas->GetParent(),
                         new wxCloseEvent(wxEVT_CLOSE_WINDOW));
            break;
        }

        now = Clock::now();

        // Phase 1 (under lock): run the emulator tick, then snapshot the video
        // buffer before releasing the lock.  DrawGL() reads only from the
        // snapshot, so the emulator is free to write the next frame immediately
        // after the lock drops.
        //
        // std::optional<VideoSnapshot> gives us conditional construction with
        // correct RAII destruction and no heap allocation.  The optional itself
        // is just a bool + aligned storage — T is not constructed until emplace()
        // is called, so the 153 KB arrays are not touched on frames where
        // do_draw is false.
        bool did_draw   = false;
        bool full_speed = false;
        bool do_draw    = false;
        std::optional<VideoSnapshot> snap;
        {
            std::lock_guard<std::mutex> emu_lk(m_canvas->m_emu_mutex);

            full_speed       = Handler()->FullSpeed();
            bool do_emu_tick = full_speed || (now >= next_emulator_tick);

            {
                const bool want_vsync = !full_speed;
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
                if (const char* err = Handler()->OnLoop())
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

            do_draw = vsync_active || full_speed || (now >= next_render_tick);

            if (do_draw && !vsync_active && !full_speed)
            {
                next_render_tick += NsInt(EMULATOR_PERIOD_NS);
                if (next_render_tick < now)
                    next_render_tick = now + NsInt(EMULATOR_PERIOD_NS);
            }

            // Snapshot video state while still holding the lock.
            // eUla::screen is 320*240 = 76,800 bytes; memcpy takes ~20 µs on
            // modern hardware — negligible compared to the ~20 ms frame budget.
            if (do_draw)
            {
                snap.emplace();  // constructs in-place, no heap allocation
                snap->frame = Handler()->VideoFrame();
                memcpy(snap->video, Handler()->VideoData(), sizeof(snap->video));
#ifdef USE_UI
                const void* ui_data = Handler()->VideoDataUI();
                snap->has_ui = (ui_data != nullptr);
                if (snap->has_ui)
                    memcpy(snap->video_ui, ui_data, sizeof(snap->video_ui));
#endif
            }
        } // emu_lk released here — emulator may now begin writing the next frame.

        // Phase 2 (no lock): GPU upload and draw from the snapshot.
        // optional's destructor handles cleanup when snap goes out of scope.
        if (snap && m_running.load(std::memory_order_relaxed))
        {
            const wxSize sz = m_canvas->GetViewportSize();
            if (sz.x > 0 && sz.y > 0)
                did_draw = DrawGL(sz.x, sz.y, *snap);
        }

        // SwapGL (and any vsync stall) runs outside the emu lock so the main
        // thread is never blocked during a vblank wait.
        if (did_draw && m_running.load(std::memory_order_relaxed))
        {
            if (!full_speed)
            {
                // Normal mode: vsync (if active) acts as the rate limiter.
                m_canvas->SwapGL();
            }
            else
            {
                // full_speed mode: throttle SwapBuffers to ~50 fps.
                // Without throttling, unconstrained wglSwapBuffers calls
                // accumulate in the NVIDIA WDDM flip queue faster than the
                // GPU can drain them.  The kernel-mode driver then enters a
                // spin-loop that survives process termination and only clears
                // on logout.
                now = Clock::now();
                if (now >= next_full_speed_swap)
                {
                    m_canvas->SwapGL();
                    next_full_speed_swap = now + NsInt(FULL_SPEED_SWAP_PERIOD_NS);
                }
            }
        }

        // Refresh now after SwapGL — with vsync it may have blocked ~20 ms.
        // Without this, next_emulator_tick is already in the past when we
        // reach the sleep below, sleep is skipped every iteration, and the
        // loop burns 100% of one CPU core.
        now = Clock::now();

        // Interruptible inter-frame sleep: wakes immediately on pause or stop.
        if (!full_speed)
        {
            auto next_event = next_emulator_tick;
            if (!vsync_active && next_render_tick < next_event)
                next_event = next_render_tick;

            if (next_event > now)
                m_canvas->m_sync.SleepUntil(next_event - NsInt(1'000'000LL));
        }
        else
        {
            // Limit to ~250 iterations/sec — fast enough for the emulator,
            // prevents burning the core between SwapBuffers calls.
            m_canvas->m_sync.SleepFor(std::chrono::milliseconds(4));
        }
    }

    cleanupGraphics();

    // Release the GL context from this thread before the main thread destroys
    // it in ~GLCanvas(), preventing GLXBadContextTag on Linux when the context
    // is still marked current on this thread.
#if defined(__WXMSW__)
    // nullptr for both args unbinds the context without touching the HDC
    // (which is owned by wx and must not be released here).
    wglMakeCurrent(nullptr, nullptr);
#else
    glXMakeCurrent(glXGetCurrentDisplay(), None, nullptr);
#endif

    return 0;
}

// =============================================================================
//  GLCanvas — event handlers
// =============================================================================

void GLCanvas::OnPaint(wxPaintEvent& /*event*/)
{
    wxPaintDC dc(this); // must be created to validate the paint region
    // Signal the render thread that the window is now fully realized on screen.
    // EVT_PAINT is guaranteed to fire only after the window is mapped, making
    // it the most reliable trigger point for SetCurrent().
    m_shown.store(true, std::memory_order_release);
}

void GLCanvas::OnSize(wxSizeEvent& event)
{
    const wxSize sz = GetClientSize();
    m_viewport.store(sz.x, sz.y);
    event.Skip();
}

void GLCanvas::OnKeydown(wxKeyEvent& event)
{
    const int key = event.GetKeyCode();
    if (key == WXK_ESCAPE)
    {
        if (m_mouse_capture)
            m_mouse_capture->Close();
        else
        {
            wxCommandEvent ev(evtExitFullScreen);
            wxPostEvent(this, ev);
        }
        return;
    }
    m_input.OnKeydown(event);
}

void GLCanvas::OnKeyup(wxKeyEvent& event)
{
    m_input.OnKeyup(event);
}

void GLCanvas::OnMouseKey(wxMouseEvent& event)
{
    event.Skip();
#ifndef _MAC
    if (!m_mouse_capture)
        m_mouse_capture = CreateMouseCapture(this);
#endif
}

void GLCanvas::OnKillFocus(wxFocusEvent& /*event*/)
{
    SAFE_CALL(m_mouse_capture)->Close();
}

void GLCanvas::OnMouseCapture(wxCommandEvent& event)
{
    event.Skip();
    if (!event.GetId())
        m_mouse_capture = nullptr;
}

void GLCanvas::OnJoystickEvent(wxJoystickEvent& event)
{
    m_input.OnJoystickEvent(event,
        m_joysticks[0].get(), m_joysticks[1].get());
}

#ifdef USE_SDL2_GAMEPAD
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

            std::lock_guard<std::mutex> lk(m_emu_mutex);
            Handler()->OnKey(ke.key, flags);
        }
    }
}
#endif

// =============================================================================
//  GLCanvas::getMaxDisplayResolution
// =============================================================================
std::pair<int, int> GLCanvas::getMaxDisplayResolution()
{
    const int count = static_cast<int>(wxDisplay::GetCount());
    if (count == 0)
        return { -1, -1 };

    int max_w = -1;
    int max_h = -1;
    for (int i = 0; i < count; ++i)
    {
        wxDisplay d(i);
        if (!d.IsOk()) continue;
        const wxRect g = d.GetGeometry();
        if (g.GetWidth() * g.GetHeight() > max_w * max_h)
        {
            max_w = g.GetWidth();
            max_h = g.GetHeight();
        }
    }
    return (max_w == -1) ? std::make_pair(-1, -1)
                         : std::make_pair(max_w, max_h);
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
//  Public factory / control functions (called from wx_frame.cpp)
// =============================================================================

wxWindow* CreateGLCanvas(wxWindow* parent)
{
    auto* canvas = new GLCanvas(parent);
    g_canvas = canvas;
    return canvas;
}

void PauseGLCanvas()  { if (g_canvas) g_canvas->PauseRender();  }
void ResumeGLCanvas() { if (g_canvas) g_canvas->ResumeRender(); }

void LockEmulator()   { if (g_canvas) g_canvas->LockEmu();   }
void UnlockEmulator() { if (g_canvas) g_canvas->UnlockEmu(); }

#ifdef USE_SDL2_GAMEPAD
void ReloadGamepadProfiles() { if (g_canvas) g_canvas->ReloadGamepadProfiles(); }
#endif

} // namespace xPlatform

#endif // USE_WXWIDGETS
