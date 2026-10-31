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

#include <utility>
#include <array>
#include <chrono>

#include "../platform.h"
#ifdef USE_WXWIDGETS
#include <wx/wx.h>
#endif

#include "../../ui/ui.h"
#include "../../tools/profiler.h"
#include "../../tools/options.h"
#include "../../options_common.h"
#include "../../devices/video_snapshot.h"

#ifdef USE_GL

#ifdef _WINDOWS
#include <windows.h>
#endif//_WINDOWS

#include <GL/glew.h>

constexpr int slines_cnt = 256;
constexpr int sline_len = 512;
// Texture height while the extended raster (see SetExtendedRaster() below) is
// shown: 304 lines do not fit into 256. Everything else keeps the 512x256 texture.
constexpr int slines_cnt_ext = 320;

PROFILER_DECLARE(draw_p);
PROFILER_DECLARE(draw);

namespace xPlatform
{

    template <class T>
    void reportError(const char* title, const T* message, bool exit_on_error = false)
    {
#ifdef USE_WXWIDGETS
        wxMessageBox(wxString::Format("%s\n\n%s", title, message),
            title, wxOK | wxICON_ERROR);
#else
        fprintf(stderr, "%s: %s\n", title, message);
#endif
        if (exit_on_error)
            exit(EXIT_FAILURE);
    }

    void initGlew()
    {
        glewExperimental = GL_TRUE;
        GLenum err = glewInit();
        if (err != GLEW_OK)
            reportError("GLEW Initialization Failed", glewGetErrorString(err));
    }

    static struct eOptionZoom : public xOptions::eOptionInt
    {
        virtual const char* Name() const { return "zoom"; }
        virtual const char** Values() const
        {
            // The order is the numbering the options of the platforms that share this
            // file go by (wxWidgets checks its menu by 0, 1, 2), so "overscan" is last.
            // Change() cycles through the first three: the ones wxWidgets has.
            static const char* values[] = { "fill screen", "small border", "no border", "overscan", NULL };
            return values;
        }
        virtual void Change(bool next = true) { eOptionInt::Change(0, 3, next); }
        virtual int Order() const { return 35; }
        float Zoom() const
        {
            switch (*this)
            {
            case 1:  return 300.0f / 256.0f;
            case 2:  return 320.0f / 256.0f;
            default: return DEFAULT_ZOOM_VALUE;
            }
        }
    } op_zoom;

    static float opZoom() { return op_zoom.Zoom(); }

    // to deletion
    static struct eOptionFiltering : public xOptions::eOptionBool
    {
        eOptionFiltering() { Set(DEFAULT_FILTERING); }
        virtual const char* Name() const { return "filtering"; }
        virtual int Order() const { return 36; }
    } op_filtering;

    static struct eOptionGigascreen : public xOptions::eOptionBool
    {
        eOptionGigascreen() { Set(DEFAULT_GIGASCREEN); }
        virtual const char* Name() const { return "gigascreen"; }
        virtual int Order() const { return 38; }
    } op_gigascreen;

    static struct eOptionScanlines : public xOptions::eOptionBool
    {
        eOptionScanlines() { Set(DEFAULT_SCANLINES); }
        virtual const char* Name() const { return "scanlines"; }
        virtual int Order() const { return 39; }
    } op_scanlines;

#ifndef DEFAULT_LIGHTWEIGHT_SHADER
#define DEFAULT_LIGHTWEIGHT_SHADER false   // (normally defined with the other DEFAULT_* values)
#endif

    // The lightweight shader: the simple single-pass renderer - nearest-neighbour
    // scaling and no PAL effects (Gigascreen, scanlines and the phosphor mask still
    // work). Off: the full-quality shader - smooth scaling (an exact box filter,
    // see Src() in full_fragment_src) and the PAL effects.
    static struct eOptionLightweightShader : public xOptions::eOptionBool
    {
        eOptionLightweightShader() { Set(DEFAULT_LIGHTWEIGHT_SHADER); }
        virtual const char* Name() const { return "lightweight shader"; }
        virtual int Order() const { return 46; }
    } op_lightweight_shader;

    // -------------------------------------------------------------------------
    // CachedUniform
    // -------------------------------------------------------------------------

    template<typename T>
    struct CachedUniform
    {
        GLint location = -1;
        T     value{};
        bool  valid = false;
        bool  first_update = true;

        void set(GLint loc) { location = loc; valid = true; }

        void update(const T& new_val)
        {
            if (first_update || value != new_val)
            {
                value = new_val;
                if (location != -1)
                    setUniform(new_val);
                valid = true;
                first_update = false;
            }
        }

    private:
        void setUniform(const float& v) { glUniform1f(location, v); }
        void setUniform(const int& v) { glUniform1i(location, v); }
        void setUniform(const std::array<float, 2>& v) { glUniform2f(location, v[0], v[1]); }
    };

    // -------------------------------------------------------------------------
    // GL objects
    // -------------------------------------------------------------------------

    // tex_id_for[0] -> tex1[] CPU buffer, tex_id_for[1] -> tex2[] CPU buffer.
    static GLuint tex_id_for[2] = { 0, 0 };
    static GLuint ebo{};
    static GLuint vao1{}, vbo1{};   // the quad that shows the picture (the w x h corner of the texture)
    static GLuint full_shader{}, lw_shader{};

    // Full-quality uniforms
    CachedUniform<std::array<float, 2>> u_scale_cached;
    CachedUniform<float>               u_blend_factor_cached;
    CachedUniform<int>                 u_texture1_cached;
    CachedUniform<int>                 u_texture2_cached;
    CachedUniform<std::array<float, 2>> u_tex_size_cached;
    CachedUniform<std::array<float, 2>> u_src_size_cached;
    CachedUniform<int>                 u_show_scanlines_cached;
    CachedUniform<int>                 u_mask_scale_cached;
    CachedUniform<int>                 u_enable_pal_effects_cached;
    CachedUniform<float>               u_pal_strength_cached;
    CachedUniform<float>               u_beam_spread_cached;
    CachedUniform<float>               u_slines_cnt_cached;

    // Lightweight uniforms
    CachedUniform<std::array<float, 2>> u_lw_scale;
    CachedUniform<float>               u_lw_blend_factor;
    CachedUniform<int>                 u_lw_texture1;
    CachedUniform<int>                 u_lw_texture2;
    CachedUniform<int>                 u_lw_show_scanlines;
    CachedUniform<int>                 u_lw_mask_scale;
    CachedUniform<float>               u_lw_slines_cnt;

    // Driver-call reduction
    static GLuint current_program = 0;
    static int    src_filter_applied = 0;   // filter of the source textures: 0 = GL_NEAREST (as created), 1 = GL_LINEAR

    static constexpr int MAX_CACHED_UNITS = 8;
    static GLuint bound_tex[MAX_CACHED_UNITS] = {};  // zeroed = no texture bound

    // Returns the array index for a GL texture unit enum, or -1 if out of range.
    inline int TexUnitIndex(GLenum unit)
    {
        int idx = static_cast<int>(unit - GL_TEXTURE0);
        return (idx >= 0 && idx < MAX_CACHED_UNITS) ? idx : -1;
    }

    // Bind a texture to a unit, updating the cache.  No-op if already bound.
    // glActiveTexture is only called when a bind is actually needed, avoiding
    // an unnecessary active-unit switch on every cached hit.
    inline void BindTextureUnit(GLenum unit, GLuint tex)
    {
        int idx = TexUnitIndex(unit);
        if (idx < 0) return;
        if (bound_tex[idx] != tex)
        {
            glActiveTexture(unit);
            glBindTexture(GL_TEXTURE_2D, tex);
            bound_tex[idx] = tex;
        }
    }

    // Mark all cached texture bindings as unknown.  Call this after any
    // raw glBindTexture / texture respecification that bypasses
    // BindTextureUnit(), so the next BindTextureUnit() call will re-bind.
    inline void InvalidateTextureCache()
    {
        for (int i = 0; i < MAX_CACHED_UNITS; ++i)
            bound_tex[i] = 0;
    }

    // Upload CPU pixel data to a texture on TEXTURE0.
    // Forces TEXTURE0 to be active regardless of cache state, then routes the
    // bind through BindTextureUnit so the cache stays consistent.
    // The explicit glActiveTexture call is required because glTexSubImage2D
    // operates on the currently *active* unit — if BindTextureUnit's cache
    // reports a hit and returns early, we must still ensure the active unit
    // is TEXTURE0 before the upload.
    inline void UploadTexture(GLuint texture, const void* src,
        GLsizei width, GLsizei height)
    {
        glActiveTexture(GL_TEXTURE0);   // ensure active unit is TEXTURE0
        BindTextureUnit(GL_TEXTURE0, texture);  // bind (no-op if already bound)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
            width, height, GL_RGBA, GL_UNSIGNED_BYTE, src);
    }

    // The source textures are sampled with GL_NEAREST by the lightweight shader, and
    // with GL_LINEAR by the full-quality one: its shader does the scaling filter
    // itself and needs the bilinear fetch to do it (see Src() in full_fragment_src).
    // Switches both textures when the path changes; a no-op otherwise.
    static void SetSourceFilter(int filter)
    {
        if (filter == src_filter_applied)
            return;
        const GLint f = filter ? GL_LINEAR : GL_NEAREST;
        for (int i = 0; i < 2; ++i)
        {
            BindTextureUnit(GL_TEXTURE0, tex_id_for[i]);
            glActiveTexture(GL_TEXTURE0);   // glTexParameteri works on the active unit
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        }
        src_filter_applied = filter;
    }

    // -------------------------------------------------------------------------

    // The picture that is shown: src_w x src_h source pixels in a texture that is
    // sline_len x tex_lines. Normally the 320x240 raster of the emulation core in
    // 512x256; SetExtendedRaster() switches to a bigger one (and back).
    static int tex_lines = slines_cnt, src_w = 320, src_h = 240;
    static const byte* ext_data = nullptr;           // the extended raster of this frame
    static int ext_w = 0, ext_h = 0;

    static dword tex1[sline_len * slines_cnt_ext], * p_tex1 = tex1;
    static dword tex2[sline_len * slines_cnt_ext], * p_tex2 = tex2;
    static int   video_frame_last = -1;
    static bool  giga_was_enabled = false;

#ifdef USE_SDL2_DESKTOP
    // sdl2_desktop/sdl2_desktop_video.cpp - true only while fullscreen with
    // "Native refresh" on and a matching display mode actually applied.
    // Forward-declared here rather than pulled in via a shared header,
    // matching how this codebase already wires up other cross-file calls
    // (see e.g. ResizeToOrgSizeMultiple() in sdl2_desktop_menu.cpp).
    bool FieldRateSyncActive();
#endif

    // Layout-change tracking for glClear.
    // pending_clear starts true so HandleLayoutClear clears both swap-chain
    // buffers on the first two frames, preventing driver-default gray at startup.
    static int  last_vport_width = -1;
    static int  last_vport_height = -1;
    static int  last_zoom = -1;
    static bool pending_clear = true;

    // -------------------------------------------------------------------------
    // Shaders
    // -------------------------------------------------------------------------

    const char* vertex_src =
        R"(
#version 130
in vec2 aPos;
in vec2 aTexCoord;
out vec2 TexCoord;
uniform vec2 scale;
void main()
{
    gl_Position = vec4(aPos * scale, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

    // -------------------------------------------------------------------------
    // Full-quality single-pass shader: scaling, then PAL effects, phosphor mask and
    // scanlines.
    //
    // Scaling. The picture is a few hundred pixels wide and is shown several times
    // bigger, usually by a non-integer factor. Plain nearest-neighbour scaling then
    // makes some pixels wider than others (and they shimmer when the picture moves);
    // plain bilinear blurs every pixel. What looks right is the nearest-neighbour
    // picture averaged over the area of each SCREEN pixel: pixels stay crisp, and
    // only where a screen pixel straddles the edge of two source pixels is it a mix
    // of the two - in proportion to how much of it each covers. Src() computes that
    // exactly: the covered part of the neighbouring texel along each axis is a
    // linear ramp of width w (the size of a screen pixel in texels, from fwidth()),
    // and a bilinear fetch at the right position returns that blend in one go. (That
    // is the average that upscaling the picture into a much larger intermediate
    // texture and filtering it back down only approximates - blurrier, and far more
    // expensive in memory and in work.)
    //
    // Everything below works in texels of the picture. p is the position, in them.
    // For a screen pixel bigger than a texel (a window smaller than the picture)
    // the ramp is wider than the fetch can blend and it degrades to plain bilinear.
    // -------------------------------------------------------------------------
    const char* full_fragment_src =
        R"(
#version 130

in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D texture1;   // the picture, GL_LINEAR
uniform sampler2D texture2;   // the previous frame (Gigascreen)
uniform float blendFactor;    // 0.5 with Gigascreen, 0.0 without
uniform vec2 texSize;         // the texture, in texels: 512 x 256 (x 320 in the extended raster)
uniform vec2 srcSize;         // the picture, in texels: 320 x 240 (the extended raster is bigger)

uniform int showScanlines;
uniform int enablePalEffects;

uniform float palStrength;    // 0.0 - 1.0 (PAL effect intensity)
uniform float beamSpread;     // 0.0 - 1.0 (CRT beam spread amount; clamped to [0,1] for mix)
uniform int maskScale;        // Phosphor mask column width (pixels per phase)

// Precomputed constants (for readability and optimization)
const float PI = 3.141592653589793;          // Precise Pi value
uniform float slinesCnt;                     // scanline waves per frame: 256 for the 320x240 picture
const float SLINE_LEN = 512.0;               // Horizontal scan line length

// Luminance (Y) calculation coefficients (BT.601 standard)
const vec3 LUMINANCE_COEFF = vec3(0.299, 0.587, 0.114);

// PAL Dot Crawl: Vertical phase-shifted interference
const float DOT_CRAWL_SUB_MULT = 0.5;         // Subcarrier frequency multiplier
const float DOT_CRAWL_FRAME_SPEED = 0.3;      // Frame animation speed
const float DOT_CRAWL_STRENGTH = 0.02;        // Base crawl intensity

// PAL Chroma Bandwidth Limit: Simulate low-pass filter on U/V channels
const float CHROMA_RADIUS = 0.5;               // Effective filter radius (source pixels)

const float CHROMA_WEIGHT_LEFT_1_5 = 0.08;    // Left 1.5x step weight
const float CHROMA_WEIGHT_LEFT_0_75 = 0.22;   // Left 0.75x step weight
const float CHROMA_WEIGHT_CENTER = 0.40;      // Center sample weight
const float CHROMA_WEIGHT_RIGHT_0_75 = 0.22;  // Right 0.75x step weight
const float CHROMA_WEIGHT_RIGHT_1_5 = 0.08;   // Right 1.5x step weight

// Green Dot Artifact: PAL-specific red/green tint distortion
const float GREEN_DOT_MIX = 0.36;              // Strength of green dot effect

// Phase Modulation: Color phase interference (simplified from original A-B ops)
const float PHASE_MOD_X_FREQ = 0.5;            // Horizontal frequency multiplier
const float PHASE_MOD_FRAME_SPEED = 0.2;       // Frame animation speed
const float PHASE_MOD_STRENGTH = 0.04;         // Base phase intensity
const vec3 PHASE_MOD_PATTERN = vec3(0.6, -0.4, -1.0); // Combined offset vector

// Luminance-Chrominance Crosstalk: Luma leakage into chroma channels
const float CROSSTALK_STRENGTH = 0.05;         // Base crosstalk intensity

// CRT Beam Spread: Horizontal blur simulation
const float BEAM_SPREAD_SCALE = 1.5;           // Scale uniform to sample distance (source pixels)

// CRT Phosphor Mask: UKTV-style columnar shading
const vec3 MASK_BASE = vec3(0.90, 0.95, 0.85);      // Base mask color per phase
const vec3 MASK_AMPLITUDE = vec3(0.10, 0.05, 0.15); // Sine wave amplitude per channel
const vec2 MASK_LUMINANCE_RANGE = vec2(0.2, 0.8);   // Luma range for mask strength
const float MASK_MIX_STRENGTH = 0.35;               // Overall mask intensity

// Scanlines: Vertical intensity variation
const vec2 SCANLINE_PARAMS = vec2(0.95, 0.05); // Min value + amplitude (peak-to-peak)

// White Balance: PAL RGB correction to match expected output
const vec3 WHITE_BALANCE = vec3(1.0, 0.985, 1.03);

// The picture at p (in texels): the nearest-neighbour picture averaged over a screen
// pixel of w texels (see above). The fetch is clamped to the picture, so that its
// edges do not blend with the unused rest of the texture.
vec3 Src(vec2 p, vec2 w)
{
    vec2 i = floor(p - 0.5);                                  // texels i and i+1 share the nearest edge
    vec2 t = clamp((p - (i + 1.0)) / w + 0.5, 0.0, 1.0);      // how much of texel i+1 the pixel covers
    vec2 q = clamp(i + 0.5 + t, vec2(0.5), srcSize - 0.5);    // the bilinear position that blends (1-t, t)
    vec2 uv = q / texSize;
    vec3 c = texture(texture1, uv).rgb;
    if (blendFactor > 0.0)
        c = mix(c, texture(texture2, uv).rgb, blendFactor);
    return c;
}

// YUV <-> RGB conversion (PAL-specific coefficients from original code)
vec3 rgb2yuv(vec3 rgb) {
    return vec3(
        dot(rgb, LUMINANCE_COEFF),                 // Y: Luma
        dot(rgb, vec3(-0.14713, -0.28886, 0.436)), // U: Blue-luminance
        dot(rgb, vec3(0.615, -0.51499, -0.10001))  // V: Red-luminance
    );
}

vec3 yuv2rgb(vec3 yuv) {
    return vec3(
        yuv.x + 1.13983 * yuv.z,                   // R = Y + 1.14*V
        yuv.x - 0.39465 * yuv.y - 0.58060 * yuv.z, // G = Y - 0.39*U - 0.58*V
        yuv.x + 2.03211 * yuv.y                    // B = Y + 2.03*U
    );
}

void main() {
    vec2 p = TexCoord * texSize;               // this pixel, in texels of the picture
    vec2 w = max(fwidth(p), vec2(1e-4));       // the size of a screen pixel, in texels
    vec4 color = vec4(Src(p, w), 1.0);
    vec2 fragPos = gl_FragCoord.xy;            // Fragment position for mask/line math

    if (enablePalEffects != 0) {
        // --- PAL Chroma Processing ---
        vec3 yuv = rgb2yuv(color.rgb);         // Convert initial color to YUV

        // Chroma Bandwidth Limit: Low-pass filter on U/V to simulate PAL's limited bandwidth
        vec2 offset1_5 = vec2(CHROMA_RADIUS * 1.5, 0.0);          // ±1.5x step (left/right), in texels
        vec2 offset0_75 = vec2(CHROMA_RADIUS * 0.75, 0.0);        // ±0.75x step

        // Sample neighbors and filter chroma (Y is unused—only U/V matter here)
        vec3 yuvL1_5 = rgb2yuv(Src(p - offset1_5, w));
        vec3 yuvL0_75 = rgb2yuv(Src(p - offset0_75, w));
        vec3 yuvR0_75 = rgb2yuv(Src(p + offset0_75, w));
        vec3 yuvR1_5 = rgb2yuv(Src(p + offset1_5, w));

        vec3 yuvCenter = yuv;
        vec2 filteredChroma =
            yuvL1_5.yz * CHROMA_WEIGHT_LEFT_1_5 +
            yuvL0_75.yz * CHROMA_WEIGHT_LEFT_0_75 +
            yuvCenter.yz * CHROMA_WEIGHT_CENTER +
            yuvR0_75.yz * CHROMA_WEIGHT_RIGHT_0_75 +
            yuvR1_5.yz * CHROMA_WEIGHT_RIGHT_1_5;

        float chromaMix = clamp(palStrength * 3.0, 0.0, 1.0);     // Limit mix to 0-1
        yuv.yz = mix(yuv.yz, filteredChroma, chromaMix);          // Blend original/filtered chroma

        vec3 palRgb = yuv2rgb(yuv);                               // Convert back to RGB

        // Green Dot Artifact: Red from green (PAL-specific tint)
        vec3 greenDotTint = vec3(palRgb.g * 1.1, palRgb.g, palRgb.b * 0.9);
        palRgb = mix(palRgb, greenDotTint, GREEN_DOT_MIX * palStrength);

        // Luminance-Chrominance Crosstalk: Leak luma into chroma channels
        float luma = dot(palRgb, LUMINANCE_COEFF);
        vec3 crosstalk = (luma - palRgb) * CROSSTALK_STRENGTH * palStrength;
        palRgb += crosstalk;

        float spreadDist = beamSpread * BEAM_SPREAD_SCALE;        // in texels
        vec3 left  = Src(p - vec2(spreadDist, 0.0), w);
        vec3 right = Src(p + vec2(spreadDist, 0.0), w);
        float sideWeight   = beamSpread * 0.275;
        float centerWeight = 1.0 - sideWeight * 2.0;
        color.rgb = palRgb * centerWeight + (left + right) * sideWeight;
    }

    // --- CRT Phosphor Mask: UKTV-style columnar shading ---
    if (maskScale > 0)
    {
        float maskCol = floor(fragPos.x / maskScale);            // Which phase this pixel is in
        float angle = maskCol * 2.0 * PI / 3.0;                  // 120° phase shifts per channel
        vec3 maskTint = MASK_BASE + MASK_AMPLITUDE * sin(vec3(   // Sine wave per RGB (phase-shifted)
            angle,
            angle + 2.0 * PI / 3.0,                              // Green: +120°
            angle + 4.0 * PI / 3.0                               // Blue: +240°
        ));

        float maskStrength = smoothstep(                         // Stronger in bright areas
            MASK_LUMINANCE_RANGE.x,
            MASK_LUMINANCE_RANGE.y,
            dot(color.rgb, LUMINANCE_COEFF)
        );

        color.rgb *= mix(vec3(1.0), maskTint, maskStrength * MASK_MIX_STRENGTH);
    }

    // --- Scanlines: Vertical intensity variation ---
    if (showScanlines != 0) {
        float scanFreq = slinesCnt * 2.0 * PI;              // Full sine waves per frame
        float top = 1.0 - p.y / srcSize.y;                  // 1 at the top of the picture, 0 at the bottom
        color.rgb *= SCANLINE_PARAMS.x +
                     SCANLINE_PARAMS.y * sin(top * scanFreq);
    }

    // --- Final White Balance: Correct PAL RGB output ---
    if (maskScale > 0)
    {
        color.rgb *= WHITE_BALANCE;
    }

    FragColor = color;
}
)";

    // -------------------------------------------------------------------------
    // Lightweight single-pass shader.
    //
    // Nearest-neighbour scaling, no PAL YUV pipeline.  One draw call: Speccy
    // texture (with optional gigascreen blend) → screen, with scanlines and
    // phosphor mask.
    //
    // Phosphor mask is in screen space (gl_FragCoord) for physically consistent
    // column width.  srcScreenWidth is the screen-pixel span of the 320 source
    // columns after zoom + aspect correction, uploaded each frame from the CPU.
    // -------------------------------------------------------------------------
    const char* lw_fragment_src =
        R"(
#version 130

in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D texture1;
uniform sampler2D texture2;
uniform float blendFactor;
uniform int   showScanlines;
uniform int   maskScale; // same value as the full-quality pass — screen pixels per phase column

uniform float slinesCnt; // texture height in lines (256; 320 in the extended raster)
const vec3  LUMINANCE  = vec3(0.299, 0.587, 0.114);

// tri() gives a sharp V-shape — more CRT-like than sine, zero transcendental cost.
// Scanline range: [SCAN_BASE - SCAN_AMP, SCAN_BASE + SCAN_AMP] = [0.79, 1.00]
const float SCAN_BASE = 0.895;
const float SCAN_AMP  = 0.105;

float tri(float x) { return abs(fract(x) * 2.0 - 1.0); }

void main()
{
    vec3 color = mix(
        texture(texture1, TexCoord).rgb,
        texture(texture2, TexCoord).rgb,
        blendFactor);

    // Scanlines in normalised texture space — resolution independent.
    if (showScanlines != 0)
        color *= SCAN_BASE + SCAN_AMP * tri(TexCoord.y * slinesCnt);

    if (maskScale > 0)
    {
        const float PI = 3.141592653589793;
        float maskCol = floor(gl_FragCoord.x / float(maskScale));
        float angle   = maskCol * 2.0 * PI / 3.0;

        vec3 maskTint = vec3(0.90, 0.95, 0.85) + vec3(0.10, 0.05, 0.15) * sin(vec3(
            angle,
            angle + 2.0 * PI / 3.0,
            angle + 4.0 * PI / 3.0));

        float lum      = dot(color, LUMINANCE);
        float strength = smoothstep(0.2, 0.8, lum);
        color *= mix(vec3(1.0), maskTint, strength * 0.35);
        color *= vec3(1.0, 0.985, 1.03);
    }

    FragColor = vec4(color, 1.0);
}
)";

    // -------------------------------------------------------------------------
    // GL helpers
    // -------------------------------------------------------------------------

    GLuint compileShader(GLenum type, const char* src)
    {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);
        GLint ok;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[512];
            glGetShaderInfoLog(shader, 512, nullptr, log);
            reportError(type == GL_VERTEX_SHADER ? "Vertex Shader Error"
                : "Fragment Shader Error", log);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    GLuint linkProgram(GLuint vert, GLuint frag)
    {
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vert);
        glAttachShader(prog, frag);
        glBindAttribLocation(prog, 0, "aPos");
        glBindAttribLocation(prog, 1, "aTexCoord");
        glLinkProgram(prog);
        GLint ok;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[512];
            glGetProgramInfoLog(prog, 512, nullptr, log);
            reportError("Program Link Error", log);
            glDeleteProgram(prog);
            return 0;
        }
        return prog;
    }

    inline void UseProgram(GLuint program)
    {
        if (current_program != program)
        {
            glUseProgram(program);
            current_program = program;
        }
    }

    // Issue glClear only when the viewport or zoom mode changes.
    void HandleLayoutClear(int vport_width, int vport_height)
    {
        int  cur_zoom = static_cast<int>(op_zoom);
        bool layout_changed = (vport_width != last_vport_width ||
            vport_height != last_vport_height ||
            cur_zoom != last_zoom);
        if (layout_changed)
        {
            glClear(GL_COLOR_BUFFER_BIT);
            pending_clear = true;
            last_vport_width = vport_width;
            last_vport_height = vport_height;
            last_zoom = cur_zoom;
        }
        else if (pending_clear)
        {
            glClear(GL_COLOR_BUFFER_BIT);
            pending_clear = false;
        }
    }

    // -------------------------------------------------------------------------
    // initGraphics
    // -------------------------------------------------------------------------

    // (The arguments are the size of the biggest display; they used to size an
    // intermediate texture that no longer exists. Left for the callers.)
    void initGraphics(int /*scr_width*/, int /*scr_height*/)
    {
        // Guarantee black clear color from the very first frame.
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

        // Source textures (Speccy frame data).
        // tex_id_for[0] → tex1[] CPU buffer, tex_id_for[1] → tex2[] CPU buffer.
        // Both CPU buffers and both GPU textures are zeroed (black) so that the
        // very first frame never blends with uninitialized VRAM (which shows as gray).
        memset(tex1, 0, sizeof(tex1));
        memset(tex2, 0, sizeof(tex2));
        for (int i = 0; i < 2; ++i)
        {
            glGenTextures(1, &tex_id_for[i]);
            glBindTexture(GL_TEXTURE_2D, tex_id_for[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                sline_len, slines_cnt, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                i == 0 ? tex1 : tex2);  // upload zeroed data instead of nullptr
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        src_filter_applied = 0;   // as created; the full-quality path switches both to GL_LINEAR

        // Raw glBindTexture calls above (during texture creation) bypassed the
        // BindTextureUnit cache, so invalidate it now before normal rendering begins.
        InvalidateTextureCache();

        // vao1: cropped quad — maps only the 320x240 visible area of the 512x256 texture.
        // Used by both shaders (ApplyRasterGeometry() respecifies it for a bigger picture).
        float vertices_cropped[] = {
            -1.0f,  1.0f, 0.0f,             0.0f,
            -1.0f, -1.0f, 0.0f,             240.0f / 256.0f,
             1.0f, -1.0f, 320.0f / 512.0f,  240.0f / 256.0f,
             1.0f,  1.0f, 320.0f / 512.0f,  0.0f
        };

        unsigned int indices[] = { 0, 1, 2, 0, 2, 3 };

        glGenBuffers(1, &ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        glGenVertexArrays(1, &vao1);
        glGenBuffers(1, &vbo1);
        glBindVertexArray(vao1);
        glBindBuffer(GL_ARRAY_BUFFER, vbo1);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices_cropped), vertices_cropped, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
            (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);

        // Compile shaders
        GLuint vs = compileShader(GL_VERTEX_SHADER, vertex_src);
        GLuint fs_full = compileShader(GL_FRAGMENT_SHADER, full_fragment_src);
        GLuint fs_lw = compileShader(GL_FRAGMENT_SHADER, lw_fragment_src);

        full_shader = linkProgram(vs, fs_full);
        lw_shader = linkProgram(vs, fs_lw);

        glDeleteShader(vs);
        glDeleteShader(fs_full);
        glDeleteShader(fs_lw);

        // Full-quality uniforms
        glUseProgram(full_shader);
        u_scale_cached.set(glGetUniformLocation(full_shader, "scale"));
        u_blend_factor_cached.set(glGetUniformLocation(full_shader, "blendFactor"));
        u_texture1_cached.set(glGetUniformLocation(full_shader, "texture1"));
        u_texture2_cached.set(glGetUniformLocation(full_shader, "texture2"));
        u_tex_size_cached.set(glGetUniformLocation(full_shader, "texSize"));
        u_src_size_cached.set(glGetUniformLocation(full_shader, "srcSize"));
        u_show_scanlines_cached.set(glGetUniformLocation(full_shader, "showScanlines"));
        u_mask_scale_cached.set(glGetUniformLocation(full_shader, "maskScale"));
        u_enable_pal_effects_cached.set(glGetUniformLocation(full_shader, "enablePalEffects"));
        u_pal_strength_cached.set(glGetUniformLocation(full_shader, "palStrength"));
        u_beam_spread_cached.set(glGetUniformLocation(full_shader, "beamSpread"));
        u_slines_cnt_cached.set(glGetUniformLocation(full_shader, "slinesCnt"));

        if (u_scale_cached.location == -1 ||
            u_blend_factor_cached.location == -1 ||
            u_texture1_cached.location == -1 ||
            u_texture2_cached.location == -1 ||
            u_tex_size_cached.location == -1 ||
            u_src_size_cached.location == -1 ||
            u_show_scanlines_cached.location == -1 ||
            u_mask_scale_cached.location == -1 ||
            u_enable_pal_effects_cached.location == -1 ||
            u_pal_strength_cached.location == -1 ||
            u_beam_spread_cached.location == -1)
        {
            xPlatform::reportError("Shader Uniform Error",
                "Failed to get uniform locations (full-quality).");
        }

        // Lightweight uniforms
        glUseProgram(lw_shader);
        u_lw_scale.set(glGetUniformLocation(lw_shader, "scale"));
        u_lw_blend_factor.set(glGetUniformLocation(lw_shader, "blendFactor"));
        u_lw_texture1.set(glGetUniformLocation(lw_shader, "texture1"));
        u_lw_texture2.set(glGetUniformLocation(lw_shader, "texture2"));
        u_lw_show_scanlines.set(glGetUniformLocation(lw_shader, "showScanlines"));
        u_lw_mask_scale.set(glGetUniformLocation(lw_shader, "maskScale"));
        u_lw_slines_cnt.set(glGetUniformLocation(lw_shader, "slinesCnt"));

        if (u_lw_scale.location == -1 ||
            u_lw_blend_factor.location == -1 ||
            u_lw_texture1.location == -1 ||
            u_lw_texture2.location == -1 ||
            u_lw_show_scanlines.location == -1 ||
            u_lw_mask_scale.location == -1)
        {
            xPlatform::reportError("Shader Uniform Error",
                "Failed to get uniform locations (lightweight).");
        }

        // Restore program cache — the last glUseProgram during init was lw_shader.
        // This keeps current_program consistent so UseProgram() doesn't miss the
        // first real draw call's program switch.
        current_program = lw_shader;
    }

    // -------------------------------------------------------------------------
    // ApplyRasterGeometry - the picture is w x h source pixels from now on.
    // Rare (it follows the zoom mode), so the textures and the quad are simply
    // respecified. w x h = 320x240 is the original geometry: the same texture,
    // quad, aspect and shader constants as before this existed.
    // -------------------------------------------------------------------------
    static void ApplyRasterGeometry(int w, int h)
    {
        if (w == src_w && h == src_h)
            return;
        src_w = w;
        src_h = h;
        tex_lines = (h > slines_cnt) ? slines_cnt_ext : slines_cnt;

        // Black, so that nothing of the old layout shows (the CPU buffers are
        // what gigascreen blends with, too).
        memset(tex1, 0, sizeof(tex1));
        memset(tex2, 0, sizeof(tex2));
        for (int i = 0; i < 2; ++i)
        {
            glBindTexture(GL_TEXTURE_2D, tex_id_for[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sline_len, tex_lines, 0,
                GL_RGBA, GL_UNSIGNED_BYTE, i == 0 ? tex1 : tex2);
        }

        // The quad that shows the picture: the w x h corner of the texture.
        const float vertices_cropped[] = {
            -1.0f,  1.0f, 0.0f,                     0.0f,
            -1.0f, -1.0f, 0.0f,                     (float)h / tex_lines,
             1.0f, -1.0f, (float)w / sline_len,     (float)h / tex_lines,
             1.0f,  1.0f, (float)w / sline_len,     0.0f
        };
        glBindBuffer(GL_ARRAY_BUFFER, vbo1);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices_cropped), vertices_cropped);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        InvalidateTextureCache();   // raw glBindTexture above
        pending_clear = true;
    }

    // -------------------------------------------------------------------------
    // cleanupGraphics
    // -------------------------------------------------------------------------

    void cleanupGraphics()
    {
        // 1. Delete the VAO first: it holds references to the VBO and the EBO.
        glDeleteVertexArrays(1, &vao1);

        // 2. The VBO and the EBO are now unreferenced.
        glDeleteBuffers(1, &vbo1);
        glDeleteBuffers(1, &ebo);

        // 3. Textures.
        glDeleteTextures(2, tex_id_for);

        // 4. Shader programs (individual shaders were detached after linking).
        glDeleteProgram(full_shader);
        glDeleteProgram(lw_shader);

        // Reset the software caches so a potential re-init starts clean.
        current_program = 0;
        InvalidateTextureCache();
    }

#ifdef USE_BIG_ENDIAN
#define RGBX(r, g, b) (((r) << 24)|((g) << 16)|((b) << 8))
#else
#define RGBX(r, g, b) (((b) << 16)|((g) << 8)|(r))
#endif

    //=============================================================================
    //  eCachedColors
    //=============================================================================

    static struct eCachedColors
    {
        eCachedColors()
        {
            const byte brightness = 200;
            const byte bright_intensity = 55;
            for (int c = 0; c < 16; ++c)
            {
                byte i = c & 8 ? brightness + bright_intensity : brightness;
                byte b = c & 1 ? i : 0;
                byte r = c & 2 ? i : 0;
                byte g = c & 4 ? i : 0;
                items[c] = RGBX(r, g, b);
            }
        }
        dword items[16];
    } color_cache;

    //=============================================================================
    //  DrawGL
    //=============================================================================

    void LightweightShadersMessage(bool prev_use_lightweight, bool use_lightweight);

    // =============================================================================
    //  DrawGL  — pure GL function, no emulator access
    //
    //  Previously called Handler()->VideoData() / VideoFrame() / VideoDataUI()
    //  directly, creating a data race when the emulator began writing the next
    //  frame after OnLoop() returned.  Now receives a pre-built VideoSnapshot
    //  (copied under the emulator mutex in RenderThread::Entry) and reads only
    //  from it.  All logic below the pixel-conversion loop is unchanged.
    //
    //  vport_x / vport_y: offset of the viewport within the window's drawable
    //  area.  In platforms with a single composited window (sdl2_desktop),
    //  these exclude the menu bar and status bar so DrawGL only renders into
    //  the non-UI region, preventing overlap between ImGui UI elements and
    //  the emulator image.  Platforms without overlaid UI pass 0, 0.
    // =============================================================================
    bool DrawGL(int vport_x, int vport_y, int vport_width, int vport_height,
        const VideoSnapshot& snap)
    {
        PROFILER_BEGIN(draw_p);

        bool giga_enabled = op_gigascreen;
#ifdef USE_SDL2_DESKTOP
        // A field-rate-synced fullscreen mode is actually running - Prefer
        // Native refresh takes priority over Gigascreen rather than the other
        // way round, so ignore Gigascreen for as long as that holds. Only
        // this condition suppresses it; windowed mode and plain (non-PAL-
        // synced) fullscreen leave Gigascreen exactly as configured.
        if (FieldRateSyncActive())
            giga_enabled = false;
#endif
        giga_was_enabled = giga_enabled;

        const bool ext = ext_data != nullptr;
        ApplyRasterGeometry(ext ? ext_w : 320, ext ? ext_h : 240);

        if (giga_enabled && video_frame_last != snap.frame)
            std::swap(p_tex1, p_tex2);
        video_frame_last = snap.frame;

        // CPU-side colour conversion into p_tex1.
        // Reads only from the snapshot — no emulator memory touched here.
        const byte* data = ext ? ext_data : snap.video;
        dword* p = p_tex1;

        if (ext)
        {
            for (int y = 0; y < src_h; ++y)
            {
                for (int x = 0; x < src_w; ++x)
                    *p++ = color_cache.items[*data++];
                p += sline_len - src_w;
            }
        }
        else
    #ifdef USE_UI
        if (snap.has_ui)
        {
            const byte* data_ui = snap.video_ui;
            for (int y = 0; y < 240; ++y)
            {
                for (int x = 0; x < 320; ++x)
                {
                    xUi::eRGBAColor c_ui = xUi::palette[*data_ui++];
                    xUi::eRGBAColor c    = color_cache.items[*data++];
                    *p++ = RGBX((c.r >> c_ui.a) + c_ui.r,
                                (c.g >> c_ui.a) + c_ui.g,
                                (c.b >> c_ui.a) + c_ui.b);
                }
                p += 512 - 320;
            }
        }
        else
    #endif//USE_UI
        {
            for (int y = 0; y < 240; ++y)
            {
                for (int x = 0; x < 320; ++x)
                    *p++ = color_cache.items[*data++];
                p += 512 - 320;
            }
        }
        PROFILER_END(draw_p);

        PROFILER_SECTION(draw);

        // --- Track Shader Type Changes ---
        const bool use_lightweight = op_lightweight_shader;
        static bool prev_use_lightweight = false;
        LightweightShadersMessage(prev_use_lightweight, use_lightweight);
        prev_use_lightweight = use_lightweight;

        // Aspect-correction scale (identical for both render paths)
        const float aspect_src = (float)src_w / (float)src_h;
        const float aspect_dst = (float)vport_width / (float)vport_height;
        float scale_x = 1.0f, scale_y = 1.0f;
        if (aspect_dst > aspect_src)
            scale_x = aspect_src / aspect_dst;
        else
            scale_y = aspect_dst / aspect_src;

        // Resolve which GL texture IDs correspond to the current p_tex1/p_tex2
        // pointers.  std::swap() only exchanges pointers, so the mapping is:
        //   buf1_idx: slot whose CPU buffer was just written (always upload)
        //   buf2_idx: slot whose CPU buffer is the previous frame (already in VRAM)
        int    buf1_idx = (p_tex1 == tex1) ? 0 : 1;
        int    buf2_idx = 1 - buf1_idx;
        GLuint gl_tex1 = tex_id_for[buf1_idx];   // needs upload this frame
        GLuint gl_tex2 = tex_id_for[buf2_idx];   // already resident in VRAM

        // =====================================================================
        //  LIGHTWEIGHT PATH
        //  Nearest-neighbour scaling, no PAL pipeline.  Single draw call
        //  directly to screen. Gigascreen, scanlines, and screen-space
        //  phosphor mask still work.
        // =====================================================================
        if (use_lightweight)
        {
            SetSourceFilter(0);
            glViewport(vport_x, vport_y, vport_width, vport_height);
            HandleLayoutClear(vport_width, vport_height);

            UseProgram(lw_shader);

            // Upload only the freshly written buffer; p_tex2 is already in VRAM.
            UploadTexture(gl_tex1, p_tex1, sline_len, tex_lines);
            // UploadTexture() leaves TEXTURE0 active and gl_tex1 bound via the cache.

            u_lw_texture1.update(0);
            u_lw_blend_factor.update(0.0f);

            if (giga_enabled)
            {
                // gl_tex2 was uploaded last frame — just bind, no transfer.
                BindTextureUnit(GL_TEXTURE1, gl_tex2);
                u_lw_texture2.update(1);
                u_lw_blend_factor.update(0.5f);
            }

            u_lw_scale.update({ scale_x * opZoom(), scale_y * opZoom() });
            u_lw_slines_cnt.update((float)tex_lines);
            u_lw_show_scanlines.update(
                op_scanlines && vport_height > (src_h * 8) / 5 ? 1 : 0);
            u_lw_mask_scale.update(OpMaskScale());

            glBindVertexArray(vao1); // cropped quad — only the 320x240 visible area
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
            return true;
        }

        // =====================================================================
        //  FULL-QUALITY PATH
        //  One pass: the picture is scaled with an exact box filter (crisp pixels,
        //  no shimmer at any size), then PAL effects, mask and scanlines. See
        //  full_fragment_src.
        // =====================================================================
        SetSourceFilter(1);
        glViewport(vport_x, vport_y, vport_width, vport_height);
        HandleLayoutClear(vport_width, vport_height);

        UseProgram(full_shader);

        // Upload only the freshly written buffer; p_tex2 is already in VRAM.
        UploadTexture(gl_tex1, p_tex1, sline_len, tex_lines);
        // UploadTexture() leaves TEXTURE0 active and gl_tex1 bound via the cache.
        u_texture1_cached.update(0);
        u_blend_factor_cached.update(giga_enabled ? 0.5f : 0.0f);

        if (giga_enabled)
        {
            // gl_tex2 was uploaded last frame — just bind, no transfer.
            BindTextureUnit(GL_TEXTURE1, gl_tex2);
            u_texture2_cached.update(1);
        }

        u_scale_cached.update({ scale_x * opZoom(), scale_y * opZoom() });
        u_tex_size_cached.update({ float(sline_len), float(tex_lines) });
        u_src_size_cached.update({ float(src_w), float(src_h) });
        u_show_scanlines_cached.update(
            op_scanlines && vport_height > (src_h * 8) / 5 ? 1 : 0);
        u_mask_scale_cached.update(OpMaskScale());
        u_enable_pal_effects_cached.update(OpPalEffects() ? 1 : 0);
        u_pal_strength_cached.update(static_cast<float>(OpPalStrength()) / 100.0f);
        u_beam_spread_cached.update(static_cast<float>(OpBeamSpread()) / 100.0f);
        u_slines_cnt_cached.update(src_h == 240 ? (float)slines_cnt : (float)src_h);

        glBindVertexArray(vao1); // the quad that shows the picture
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        return true;
    }

    // -------------------------------------------------------------------------
    // The zoom mode: 0 "fill screen", 1 "small border", 2 "no border", 3 "overscan".
    // The platform asks which, to give the first and the last the border they show
    // (see sdl2_desktop_fullborder.cpp).
    // -------------------------------------------------------------------------
    int ZoomMode()
    {
        return static_cast<int>(op_zoom);
    }

    // The picture of the next DrawGL() calls is this w x h raster, one palette index
    // per pixel, tightly packed - instead of the 320x240 one in the snapshot. Valid
    // until it is called again; nullptr goes back to the snapshot.
    void SetExtendedRaster(const byte* data, int w, int h)
    {
        ext_data = data;
        ext_w = w;
        ext_h = h;
    }

}//namespace xPlatform

#endif//USE_GL

