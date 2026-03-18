#ifndef __VIDEO_SNAPSHOT_H__
#define __VIDEO_SNAPSHOT_H__

#include "../std_types.h" // for byte typedef

// =============================================================================
//  VideoSnapshot
//
//  Plain-data snapshot of everything DrawGL() needs from the emulator.
//  Populated under m_emu_mutex in RenderThread::Entry() immediately after
//  OnLoop() returns, then passed to DrawGL() after the lock is released.
//  DrawGL() no longer calls Handler() at all — it is a pure GL function.
//
//  Both wx_canvas.cpp and the GL implementation file include this header so
//  std::optional<VideoSnapshot> sees a complete type in wx_canvas.cpp and
//  DrawGL()'s parameter type is consistent across translation units.
//
//  Size: 76,800 bytes (video) + optional 76,800 bytes (UI) + 4 bytes (frame).
// =============================================================================
namespace xPlatform
{

struct VideoSnapshot
{
    // 320x240 palette-index pixels — one byte per pixel, same layout as
    // eUla::screen (written by UpdateRay/UpdateRayBorder/UpdateRayPaper).
    byte video[320 * 240];

    // UI overlay — same 320x240 layout.  Only meaningful when has_ui is true.
#ifdef USE_UI
    byte video_ui[320 * 240];
    bool has_ui = false;
#endif

    // eUla::frame counter — used by gigascreen to detect a frame boundary.
    int frame = -1;
};

} // namespace xPlatform

#endif//__VIDEO_SNAPSHOT_H__
