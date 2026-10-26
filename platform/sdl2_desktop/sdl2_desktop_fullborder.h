#ifndef __SDL2_DESKTOP_FULLBORDER_H__
#define __SDL2_DESKTOP_FULLBORDER_H__

#pragma once

// =============================================================================
//  More of the border for two of the zoom modes (sdl2_desktop).
//
//  The ULA of the emulation core draws a 320x240 raster: 32 pixels of border at
//  the sides, 24 lines above and below the 256x192 paper. A real picture is
//  bigger, and a demo that plays with the border (multicolor stripes, text in
//  the border) loses a good part of it. So two of the zoom modes get a raster
//  of their own, drawn by the ULA instead of that one:
//
//    "fill screen"   340x284, the paper at (40, 48): 40 / 44 pixels at the sides,
//                    48 lines above and 44 below
//    "overscan"      352x304, the paper at (48, 64): 48 / 48 pixels at the sides,
//                    64 lines above and 48 below - the whole border of a Pentagon
//
//  "small border" and "no border" look into the picture and are not touched: they
//  are the 320x240 raster, as always.
//
//  Nothing of the core is modified: the ULA draws by a table of segments, each
//  with its own destination, built by eUla::CreateTimings(). The platform builds
//  another table (the same segments, a bigger raster, a buffer of its own) and
//  puts it in the ULA's place, and the original one back when the mode ends.
//  Between two frames only, like everything that changes the timing.
// =============================================================================

#include "../platform.h"

namespace xPlatform
{

// The values of the zoom option ("zoom", platform/gl/draw.cpp).
enum { ZOOM_FILL_SCREEN = 0, ZOOM_SMALL_BORDER = 1, ZOOM_NO_BORDER = 2, ZOOM_OVERSCAN = 3 };

// Puts the ULA's table in line with the zoom mode and the timing. Call before a
// frame, between frames; does nothing when everything is as it should be.
void SyncFullBorder();

// The same for a given machine and a given zoom mode (for the tests).
void SyncFullBorder(eSpeccy* speccy, int zoom);

// Something else rebuilt the ULA's table (a change of the emulation timing).
void InvalidateFullBorder();

// The picture of the last emulated frame - one palette index per pixel, tightly
// packed, *w x *h - or nullptr when the zoom mode is one of the 320x240 ones.
// Also refreshes the 320x240 raster that Handler()->VideoData() gives (the middle
// of the bigger one), for everything else that reads it - a screenshot, say.
const byte* FullBorderRaster(int* w, int* h);
const byte* FullBorderRaster(eSpeccy* speccy, int* w, int* h);

// The size of the picture the ULA draws now; false for the 320x240 raster.
bool FullBorderSize(int* w, int* h);

}//namespace xPlatform

#endif//__SDL2_DESKTOP_FULLBORDER_H__
