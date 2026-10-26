#include "../platform.h"

#ifdef USE_SDL2_DESKTOP

#include <cstring>
#include <vector>

#include "sdl2_desktop_fullborder.h"
#include "../../speccy.h"
#include "../../devices/ula.h"

namespace xPlatform
{

int ZoomMode(); // platform/gl/draw.cpp: the zoom option

namespace
{

// The rasters: the border in pixels and lines to the left of and above the paper,
// and the size. What the ULA draws is the 256x192 paper in them.
struct eRaster { int left, top, width, height; };
const eRaster rasters[] =
{
	{ 40, 48, 340, 284 },	// fill screen
	{ 48, 64, 352, 304 },	// overscan
};
const int PAPER_W = 256, PAPER_H = 192;

// -1: the ULA's own 320x240 raster. Else an index in rasters[].
int RasterOf(int zoom)
{
	switch(zoom)
	{
	case ZOOM_FILL_SCREEN:	return 0;
	case ZOOM_OVERSCAN:		return 1;
	default:				return -1;
	}
}

int active = -1;                        // the raster the ULA draws now
bool stale = true;                      // the ULA's table may not be the one this module put there
int installed_line_tacts = 0, installed_paper_start = 0;

// What the ULA draws into: the whole time of a line, 2 pixels a tact. Then the
// border at the end of a line, the time the beam is off and the border at the
// start of the next one are one stretch of time and of memory, and a single
// segment of the table - that is how it fits into the table (a 352 pixel line
// needs 4 segments, the table has room for 960 in all). The picture is the
// corner of it; the rest of every line is the time the beam is off.
std::vector<byte> raw;
int pitch = 0;
std::vector<byte> dense;                // the picture, tightly packed, for the GL code

// The protected members of the ULA that are needed, as pointers to members (naming
// a protected member through a derived class is the standard way to get one; no
// cast, nothing of this class is ever instantiated). Its protected types and
// constants can be named here, too, which is why the work with the table is done
// by this class.
struct eUlaAccess : public eUla
{
	static int eUla::* LineTacts() { return &eUlaAccess::line_tacts; }
	static int eUla::* PaperStart() { return &eUlaAccess::paper_start; }
	static byte* eUla::* Screen() { return &eUlaAccess::screen; }
	static void (eUla::* CreateTimingsFn())() { return &eUlaAccess::CreateTimings; }
	typedef eTiming (eUla::* TimingsPtr)[4 * S_HEIGHT];
	static TimingsPtr Timings() { return &eUlaAccess::timings; }
	typedef int (eUla::* Table256Ptr)[256];
	static Table256Ptr ScrTab() { return &eUlaAccess::scrtab; }
	static Table256Ptr AtrTab() { return &eUlaAccess::atrtab; }
	static int eUla::* PrevT() { return &eUlaAccess::prev_t; }
	static eTiming* eUla::* Current() { return &eUlaAccess::timing; }

	// The table of a raster, the same segments as CreateTimings() makes but for the
	// raster above, from the line length and the paper start the ULA has now.
	static void Install(eUla* ula, const eRaster& r)
	{
		const int line_tacts = ula->*LineTacts();
		const int paper_start = ula->*PaperStart();
		pitch = 2 * line_tacts;
		raw.assign((size_t)pitch * r.height, 0);
		dense.assign((size_t)r.width * r.height, 0);

		// The tact of the first pixel of the raster. May be before the interrupt (a frame
		// with fewer lines before the paper than r.top): what is before it is not drawn.
		const int t0 = paper_start - r.top * line_tacts - r.left / 2;
		byte* base = raw.data();
		eTiming* tm = ula->*Timings();
		const int* scrtab = ula->*ScrTab();
		const int* atrtab = ula->*AtrTab();
		auto at = [&](int tact) { return base + 2 * (tact - t0); };

		int idx = 0;
		tm[idx++].Set(0, eTiming::Z_SHADOW); // to skip the non visible area
		const int first = t0 > 0 ? t0 : 0;
		tm[idx++].Set(first, eTiming::Z_BORDER, at(first)); // everything above the paper
		for(int i = 0; i < PAPER_H; ++i)
		{
			const int t = paper_start + i * line_tacts;
			tm[idx++].Set(t, eTiming::Z_PAPER, at(t), scrtab[i], atrtab[i]);
			const int tb = t + PAPER_W / 2; // the paper is 256 pixels, 2 a tact: 128 tacts
			tm[idx++].Set(tb, eTiming::Z_BORDER, at(tb)); // its right border, the blanking, the next left border
		}
		tm[idx++].Set(t0 + r.height * line_tacts, eTiming::Z_SHADOW); // the end of the raster: nothing is drawn past it
		tm[idx].Set(0x7fffffff, eTiming::Z_SHADOW);

		ula->*Current() = tm;
		ula->*PrevT() = 0;
		installed_line_tacts = line_tacts;
		installed_paper_start = paper_start;
	}

	// The ULA's own table again.
	static void Restore(eUla* ula)
	{
		(ula->*CreateTimingsFn())();
		ula->*Current() = ula->*Timings();
		ula->*PrevT() = 0;
	}

	// The picture out of the raster, and the middle of it - the 320x240 raster the
	// core has - into the ULA's own screen buffer.
	static void Extract(eUla* ula, const eRaster& r)
	{
		for(int y = 0; y < r.height; ++y)
			memcpy(&dense[(size_t)y * r.width], &raw[(size_t)y * pitch], r.width);
		const int core_x = r.left - (S_WIDTH - PAPER_W) / 2;
		const int core_y = r.top - (S_HEIGHT - PAPER_H) / 2;
		byte* core = ula->*Screen();
		for(int y = 0; y < S_HEIGHT; ++y)
			memcpy(core + y * S_WIDTH, &raw[(size_t)(core_y + y) * pitch + core_x], S_WIDTH);
	}
};

}//namespace

void InvalidateFullBorder() { stale = true; }

bool FullBorderSize(int* w, int* h)
{
	if(active < 0)
		return false;
	*w = rasters[active].width;
	*h = rasters[active].height;
	return true;
}

void SyncFullBorder() { SyncFullBorder(Handler()->Speccy(), ZoomMode()); }

void SyncFullBorder(eSpeccy* speccy, int zoom)
{
	const int want = RasterOf(zoom);
	eUla* ula = speccy->Device<eUla>();
	if(want >= 0)
	{
		if(active != want || stale
			|| ula->*eUlaAccess::LineTacts() != installed_line_tacts
			|| ula->*eUlaAccess::PaperStart() != installed_paper_start)
		{
			eUlaAccess::Install(ula, rasters[want]);
			active = want;
			stale = false;
		}
	}
	else if(active >= 0)
	{
		eUlaAccess::Restore(ula);
		active = -1;
		stale = false;
	}
}

const byte* FullBorderRaster(int* w, int* h) { return FullBorderRaster(Handler()->Speccy(), w, h); }

const byte* FullBorderRaster(eSpeccy* speccy, int* w, int* h)
{
	if(!FullBorderSize(w, h))
		return nullptr;
	eUlaAccess::Extract(speccy->Device<eUla>(), rasters[active]);
	return dense.data();
}

}//namespace xPlatform

#endif//USE_SDL2_DESKTOP
