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

#include "../platform.h"
#ifdef USE_WXWIDGETS
#include <wx/wx.h>
#endif

#include "../../ui/ui.h"
#include "../../tools/profiler.h"
#include "../../tools/options.h"
#include "../../options_common.h"

#ifdef USE_GL

#ifdef _WINDOWS
#include <windows.h>
#endif//_WINDOWS

#include <GL/glew.h>

constexpr int slines_cnt = 256;
constexpr int sline_len = 512;

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

    static struct eOptionZoom : public xOptions::eOptionEnum
    {
        virtual const char* Name() const { return "zoom"; }
        virtual const char** Values() const
        {
            static const char* values[] = { "fill screen", "small border", "no border", NULL };
            return values;
        }
        virtual void Change(bool next = true) { eOptionEnum::Change(0, 3, next); }
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

    static struct eOptionMipmapping : public xOptions::eOptionBool
    {
        eOptionMipmapping() { Set(DEFAULT_MIPMAPPING); }
        virtual const char* Name() const { return "mipmapping"; }
        virtual int Order() const { return 46; }
    } op_mipmapping;

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

    static GLuint fb_texture{}, texture_id1{}, texture_id2{};
    static GLuint ebo{};
    static GLuint vao1{}, vbo1{};   // cropped quad (320x240 visible area)
    static GLuint vao2{}, vbo2{};   // full NDC quad (FBO / screen pass)
    static GLuint fbo{};
    static GLuint fb_shader{}, screen_shader{}, lw_shader{};

    // Full-quality uniforms
    CachedUniform<float>               u_blend_factor_cached;
    CachedUniform<std::array<float, 2>> u_scale_cached;
    CachedUniform<int>                 u_texture1_cached;
    CachedUniform<int>                 u_texture2_cached;
    CachedUniform<int>                 u_show_scanlines_cached;
    CachedUniform<std::array<float, 2>> u_simple_scale_cached;
    CachedUniform<int>                 u_fb_texture_cached;
    CachedUniform<std::array<float, 2>> u_fb_size_cached;
    CachedUniform<int>                 u_mask_scale_cached;
    CachedUniform<int>                 u_enable_pal_effects_cached;
    CachedUniform<float>               u_pal_strength_cached;
    CachedUniform<float>               u_beam_spread_cached;

    // Lightweight uniforms
    CachedUniform<std::array<float, 2>> u_lw_scale;
    CachedUniform<float>               u_lw_blend_factor;
    CachedUniform<int>                 u_lw_texture1;
    CachedUniform<int>                 u_lw_texture2;
    CachedUniform<int>                 u_lw_show_scanlines;
    CachedUniform<int>                 u_lw_mask_scale; // same value uploaded to both shaders

    // Driver-call reduction
    static GLuint current_program = 0;
    bool          mip_enabled_current = false;
    bool          mip_dirty = true;
    static GLuint bound_tex[2] = { 0, 0 }; // TEXTURE0/TEXTURE1 cache

    static int fb_width = sline_len * 4;
    static int fb_height = slines_cnt * 4;

    static dword tex1[sline_len * slines_cnt], * p_tex1 = tex1;
    static dword tex2[sline_len * slines_cnt], * p_tex2 = tex2;
    static int   video_frame_last = -1;
    static bool  giga_was_enabled = false;

    // Layout-change tracking for glClear
    static int  last_vport_width = -1;
    static int  last_vport_height = -1;
    static int  last_zoom = -1;
    static bool pending_clear = false;

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

    // First pass: gigascreen blend into FBO
    const char* fb_fragment_src =
        R"(
#version 130
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D texture1;
uniform sampler2D texture2;
uniform float blendFactor;
void main()
{
    FragColor = mix(texture(texture1, TexCoord), texture(texture2, TexCoord), blendFactor);
}
)";

    // Second pass: post-processing (PAL, mask, scanlines) on FBO output
    const char* screen_fragment_src =
        R"(
#version 130

in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D fbTexture;
uniform int showScanlines;
uniform int enablePalEffects;

uniform float palStrength;    // 0.0 - 1.0 (PAL effect intensity)
uniform float beamSpread;     // 0.0 - 1.0 (CRT beam spread amount; clamped to [0,1] for mix)
uniform int maskScale;        // Phosphor mask column width (pixels per phase)
uniform vec2 fbSize;          // Size of input framebuffer

// Precomputed constants (for readability and optimization)
const float PI = 3.141592653589793;          // Precise π value
const float SLINES_CNT = 256.0;              // PAL scanline count per frame
const float SLINE_LEN = 512.0;               // Horizontal scan line length

// Luminance (Y) calculation coefficients (BT.601 standard)
const vec3 LUMINANCE_COEFF = vec3(0.299, 0.587, 0.114);

// PAL Dot Crawl: Vertical phase-shifted interference
const float DOT_CRAWL_SUB_MULT = 0.5;         // Subcarrier frequency multiplier
const float DOT_CRAWL_FRAME_SPEED = 0.3;      // Frame animation speed
const float DOT_CRAWL_STRENGTH = 0.02;        // Base crawl intensity

// PAL Chroma Bandwidth Limit: Simulate low-pass filter on U/V channels
const float CHROMA_REF_PIXELS = 320.0;         // Reference resolution for scaling
const float CHROMA_RADIUS = 0.5;               // Effective filter radius (texture pixels)

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
const float BEAM_SPREAD_SCALE = 2.0;           // Scale uniform to sample distance
const vec2 BEAM_WEIGHTS = vec2(0.6, 0.2);      // Center (0.6) + left/right (0.2 combined)

// CRT Phosphor Mask: UKTV-style columnar shading
const vec3 MASK_BASE = vec3(0.90, 0.95, 0.85);      // Base mask color per phase
const vec3 MASK_AMPLITUDE = vec3(0.10, 0.05, 0.15); // Sine wave amplitude per channel
const vec2 MASK_LUMINANCE_RANGE = vec2(0.2, 0.8);   // Luma range for mask strength
const float MASK_MIX_STRENGTH = 0.35;               // Overall mask intensity

// Scanlines: Vertical intensity variation
const vec2 SCANLINE_PARAMS = vec2(0.95, 0.05); // Min value + amplitude (peak-to-peak)

// White Balance: PAL RGB correction to match expected output
const vec3 WHITE_BALANCE = vec3(1.0, 0.985, 1.03);

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
    // Precompute values used multiple times to avoid redundant calculations
    vec2 texelSize = 1.0 / fbSize;             // Texture coordinate per pixel
    vec2 uv = TexCoord;
    vec4 color = texture(fbTexture, uv);       // Use uv instead of TexCoord (consistent)
    vec2 fragPos = gl_FragCoord.xy;            // Fragment position for mask/line math

    if (enablePalEffects != 0) {
        // --- PAL Chroma Processing ---
        vec3 yuv = rgb2yuv(color.rgb);         // Convert initial color to YUV

        // Chroma Bandwidth Limit: Low-pass filter on U/V to simulate PAL's limited bandwidth
        float chromaScale = (fbSize.x / CHROMA_REF_PIXELS) * CHROMA_RADIUS;
        float sampleStep = texelSize.x * chromaScale;             // Step between samples

        vec2 offset1_5 = vec2(sampleStep * 1.5, 0.0);             // ±1.5x step (left/right)
        vec2 offset0_75 = vec2(sampleStep * 0.75, 0.0);           // ±0.75x step

        // Sample neighbors and filter chroma (Y is unused—only U/V matter here)
        vec3 yuvL1_5 = rgb2yuv(texture(fbTexture, uv - offset1_5).rgb);
        vec3 yuvL0_75 = rgb2yuv(texture(fbTexture, uv - offset0_75).rgb);
        vec3 yuvR0_75 = rgb2yuv(texture(fbTexture, uv + offset0_75).rgb);
        vec3 yuvR1_5 = rgb2yuv(texture(fbTexture, uv + offset1_5).rgb);

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

        float spreadDist = beamSpread * BEAM_SPREAD_SCALE;
        float spreadMix  = clamp(beamSpread, 0.0, 1.0);           // Safe mix factor

        vec3 left = texture(fbTexture, uv - vec2(texelSize.x * spreadDist, 0.0)).rgb;
        vec3 right = texture(fbTexture, uv + vec2(texelSize.x * spreadDist, 0.0)).rgb;

        // Original YUV round-trip (assumed to simulate PAL chroma subsampling)
        left = yuv2rgb(rgb2yuv(left));
        right = yuv2rgb(rgb2yuv(right));

        vec3 spreadColor = palRgb * BEAM_WEIGHTS.x + (left + right) * BEAM_WEIGHTS.y;
        color.rgb = mix(palRgb, spreadColor, spreadMix);           // Preserve alpha
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
        float scanFreq = SLINES_CNT * 2.0 * PI;              // Full sine waves per frame
        color.rgb *= SCANLINE_PARAMS.x +
                     SCANLINE_PARAMS.y * sin(TexCoord.y * scanFreq);
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
    // No FBO, no PAL YUV pipeline.  One draw call: Speccy texture (with
    // optional gigascreen blend) → screen, with scanlines and phosphor mask.
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

const float SLINES_CNT = 256.0;
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
        color *= SCAN_BASE + SCAN_AMP * tri(TexCoord.y * SLINES_CNT);

    if (maskScale > 0)
    {
        float maskCol = floor(gl_FragCoord.x / float(maskScale));
        float phase   = mod(maskCol, 3.0);

        vec3 mask = vec3(
            (phase < 1.0)                 ? 1.00 : 0.72,
            (phase >= 1.0 && phase < 2.0) ? 1.00 : 0.72,
            (phase >= 2.0)                ? 1.00 : 0.72);

        float lum      = dot(color, LUMINANCE);
        float strength = clamp((lum - 0.12) / 0.65, 0.0, 1.0) * 0.55;
        color *= mix(vec3(1.0), mask, strength);
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

    inline void BindTextureUnit(GLenum unit, GLuint tex)
    {
        int idx = (unit == GL_TEXTURE0) ? 0 : (unit == GL_TEXTURE1) ? 1 : -1;
        if (idx < 0) return;
        glActiveTexture(unit);
        if (bound_tex[idx] != tex)
        {
            glBindTexture(GL_TEXTURE_2D, tex);
            bound_tex[idx] = tex;
        }
    }

    // Upload CPU pixel data directly.  Optimal for low-end GPUs where PBO
    // async DMA stalls as badly as a direct glTexSubImage2D call.
    inline void UploadTexture(GLuint texture, const void* src,
        GLsizei width, GLsizei height)
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        bound_tex[0] = texture;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
            width, height, GL_RGBA, GL_UNSIGNED_BYTE, src);
    }

    // Issue glClear only when the viewport or zoom mode changes, covering both
    // swap-chain buffers over two consecutive frames to avoid double-buffer flicker.
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

    void initGraphics(int scr_width, int scr_height)
    {
        if (scr_height != -1)
        {
            // Smallest integer scale that covers the viewport on both axes,
            // keeping pixels square.  Guarantees scanline/mask apertures land
            // on whole FBO texel boundaries.
            int sx = (scr_width + sline_len - 1) / sline_len;
            int sy = (scr_height + slines_cnt - 1) / slines_cnt;
            int s = (sx > sy) ? sx : sy;
            if (s < 1) s = 1;
            fb_width = sline_len * s;
            fb_height = slines_cnt * s;
        }

        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

        // Source textures (Speccy frame data)
        for (GLuint* tid : { &texture_id1, &texture_id2 })
        {
            glGenTextures(1, tid);
            glBindTexture(GL_TEXTURE_2D, *tid);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                sline_len, slines_cnt, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }

        // FBO texture (intermediate render target for the full-quality path)
        glGenTextures(1, &fb_texture);
        glBindTexture(GL_TEXTURE_2D, fb_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
            fb_width, fb_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

#ifdef GLEW_EXT_texture_filter_anisotropic
        if (GLEW_EXT_texture_filter_anisotropic)
        {
            GLfloat maxAniso = 0.0f;
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                maxAniso > 0.0f ? maxAniso : 1.0f);
        }
#endif

        // vao1: cropped quad — maps only the 320x240 visible area of the 512x256 texture.
        // Used for the FBO blit pass and the lightweight single-pass draw.
        float vertices_cropped[] = {
            -1.0f,  1.0f, 0.0f,             0.0f,
            -1.0f, -1.0f, 0.0f,             240.0f / 256.0f,
             1.0f, -1.0f, 320.0f / 512.0f,  240.0f / 256.0f,
             1.0f,  1.0f, 320.0f / 512.0f,  0.0f
        };

        // vao2: full NDC quad — maps [0,1]x[0,1] texture to the whole screen.
        // Used for the screen post-processing pass.
        float vertices_full[] = {
            -1.0f,  1.0f,  0.0f, 1.0f,
            -1.0f, -1.0f,  0.0f, 0.0f,
             1.0f, -1.0f,  1.0f, 0.0f,
             1.0f,  1.0f,  1.0f, 1.0f
        };

        unsigned int indices[] = { 0, 1, 2, 0, 2, 3 };

        // Shared EBO — uploaded once, bound into both VAOs.
        glGenBuffers(1, &ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        auto buildVAO = [](GLuint& vao, GLuint& vbo, float* verts, size_t vsize, GLuint ebo_)
            {
                glGenVertexArrays(1, &vao);
                glGenBuffers(1, &vbo);
                glBindVertexArray(vao);
                glBindBuffer(GL_ARRAY_BUFFER, vbo);
                glBufferData(GL_ARRAY_BUFFER, vsize, verts, GL_STATIC_DRAW);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                    (void*)(2 * sizeof(float)));
                glEnableVertexAttribArray(1);
            };

        buildVAO(vao1, vbo1, vertices_cropped, sizeof(vertices_cropped), ebo);
        buildVAO(vao2, vbo2, vertices_full, sizeof(vertices_full), ebo);
        glBindVertexArray(0);

        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, fb_texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            reportError("Framebuffer Not Complete",
                "The framebuffer is not properly configured.");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // Compile shaders
        GLuint vs = compileShader(GL_VERTEX_SHADER, vertex_src);
        GLuint fs_fb = compileShader(GL_FRAGMENT_SHADER, fb_fragment_src);
        GLuint fs_screen = compileShader(GL_FRAGMENT_SHADER, screen_fragment_src);
        GLuint fs_lw = compileShader(GL_FRAGMENT_SHADER, lw_fragment_src);

        fb_shader = linkProgram(vs, fs_fb);
        screen_shader = linkProgram(vs, fs_screen);
        lw_shader = linkProgram(vs, fs_lw);

        glDeleteShader(vs);
        glDeleteShader(fs_fb);
        glDeleteShader(fs_screen);
        glDeleteShader(fs_lw);

        // Full-quality uniforms
        glUseProgram(fb_shader);
        u_scale_cached.set(glGetUniformLocation(fb_shader, "scale"));
        u_blend_factor_cached.set(glGetUniformLocation(fb_shader, "blendFactor"));
        u_texture1_cached.set(glGetUniformLocation(fb_shader, "texture1"));
        u_texture2_cached.set(glGetUniformLocation(fb_shader, "texture2"));

        glUseProgram(screen_shader);
        u_simple_scale_cached.set(glGetUniformLocation(screen_shader, "scale"));
        u_fb_texture_cached.set(glGetUniformLocation(screen_shader, "fbTexture"));
        u_show_scanlines_cached.set(glGetUniformLocation(screen_shader, "showScanlines"));
        u_fb_size_cached.set(glGetUniformLocation(screen_shader, "fbSize"));
        u_mask_scale_cached.set(glGetUniformLocation(screen_shader, "maskScale"));
        u_enable_pal_effects_cached.set(glGetUniformLocation(screen_shader, "enablePalEffects"));
        u_pal_strength_cached.set(glGetUniformLocation(screen_shader, "palStrength"));
        u_beam_spread_cached.set(glGetUniformLocation(screen_shader, "beamSpread"));

        if (u_scale_cached.location == -1 ||
            u_blend_factor_cached.location == -1 ||
            u_texture1_cached.location == -1 ||
            u_texture2_cached.location == -1 ||
            u_simple_scale_cached.location == -1 ||
            u_fb_texture_cached.location == -1 ||
            u_show_scanlines_cached.location == -1 ||
            u_fb_size_cached.location == -1 ||
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
    }

    // -------------------------------------------------------------------------
    // cleanupGraphics
    // -------------------------------------------------------------------------

    void cleanupGraphics()
    {
        glDeleteTextures(1, &texture_id1);
        glDeleteTextures(1, &texture_id2);
        glDeleteTextures(1, &fb_texture);
        glDeleteFramebuffers(1, &fbo);
        glDeleteBuffers(1, &vbo1);
        glDeleteVertexArrays(1, &vao1);
        glDeleteBuffers(1, &vbo2);
        glDeleteBuffers(1, &ebo);
        glDeleteVertexArrays(1, &vao2);
        glDeleteProgram(fb_shader);
        glDeleteProgram(screen_shader);
        glDeleteProgram(lw_shader);
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

    void DrawGL(int vport_width, int vport_height)
    {
        PROFILER_BEGIN(draw_p);

        bool giga_enabled = op_gigascreen;

        if (giga_enabled && !giga_was_enabled)
            video_frame_last = -1;
        giga_was_enabled = giga_enabled;

        if (giga_enabled && video_frame_last != Handler()->VideoFrame())
            std::swap(p_tex1, p_tex2);
        video_frame_last = Handler()->VideoFrame();

        // CPU-side colour conversion into p_tex1
        byte* data = (byte*)Handler()->VideoData();
        dword* p = p_tex1;

#ifdef USE_UI
        byte* data_ui = (byte*)Handler()->VideoDataUI();
        if (data_ui)
        {
            for (int y = 0; y < 240; ++y)
            {
                for (int x = 0; x < 320; ++x)
                {
                    xUi::eRGBAColor c_ui = xUi::palette[*data_ui++];
                    xUi::eRGBAColor c = color_cache.items[*data++];
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

        // Aspect-correction scale (identical for both render paths)
        const float aspect_src = 320.0f / 240.0f;
        const float aspect_dst = (float)vport_width / (float)vport_height;
        float scale_x = 1.0f, scale_y = 1.0f;
        if (aspect_dst > aspect_src)
            scale_x = aspect_src / aspect_dst;
        else
            scale_y = aspect_dst / aspect_src;

        // =====================================================================
        //  LIGHTWEIGHT PATH
        //  No FBO, no PAL pipeline.  Single draw call directly to screen.
        //  Gigascreen, scanlines, and screen-space phosphor mask still work.
        // =====================================================================
        if (!OpPalEffects() && !op_mipmapping)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, vport_width, vport_height);
            HandleLayoutClear(vport_width, vport_height);

            UseProgram(lw_shader);

            // Upload and configure source texture(s)
            UploadTexture(texture_id1, p_tex1, sline_len, slines_cnt);
            // Nearest-neighbour keeps pixel-art look crisp on integer scales.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

            u_lw_texture1.update(0);
            u_lw_blend_factor.update(0.0f);

            if (giga_enabled)
            {
                UploadTexture(texture_id2, p_tex2, sline_len, slines_cnt);
                BindTextureUnit(GL_TEXTURE1, texture_id2);
                u_lw_texture2.update(1);
                u_lw_blend_factor.update(0.5f);
            }

            // Restore TEXTURE0 after possible texture2 upload side-effect.
            BindTextureUnit(GL_TEXTURE0, texture_id1);

            u_lw_scale.update({ scale_x * opZoom(), scale_y * opZoom() });
            u_lw_show_scanlines.update(
                op_scanlines && vport_height > slines_cnt + slines_cnt / 2 ? 1 : 0);
            u_lw_mask_scale.update(OpMaskScale());

            glBindVertexArray(vao1); // cropped quad — only the 320x240 visible area
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
            return;
        }

        // =====================================================================
        //  FULL-QUALITY PATH
        //  Two-pass: FBO blit/blend → screen post-processing (PAL, mask, lines)
        // =====================================================================

        if (op_mipmapping != mip_enabled_current)
        {
            mip_enabled_current = op_mipmapping;
            mip_dirty = true;
        }

        // --- First pass: render into FBO ---
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, fb_width, fb_height);

        UseProgram(fb_shader);
        u_blend_factor_cached.update(giga_enabled ? 0.5f : 0.0f);
        u_scale_cached.update({ 1.0f, 1.0f });

        UploadTexture(texture_id1, p_tex1, sline_len, slines_cnt);
        u_texture1_cached.update(0);

        if (giga_enabled)
        {
            UploadTexture(texture_id2, p_tex2, sline_len, slines_cnt);
            BindTextureUnit(GL_TEXTURE1, texture_id2);
            u_texture2_cached.update(1);
        }

        BindTextureUnit(GL_TEXTURE0, texture_id1);

        glBindVertexArray(vao1); // cropped quad
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

        // --- Second pass: FBO → screen ---
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, vport_width, vport_height);
        HandleLayoutClear(vport_width, vport_height);

        UseProgram(screen_shader);
        u_show_scanlines_cached.update(
            op_scanlines && vport_height > slines_cnt + slines_cnt / 2 ? 1 : 0);
        u_simple_scale_cached.update({ scale_x * opZoom(), scale_y * opZoom() });
        u_fb_texture_cached.update(0);
        u_fb_size_cached.update({ float(fb_width), float(fb_height) });
        u_mask_scale_cached.update(OpMaskScale());
        u_enable_pal_effects_cached.update(OpPalEffects() ? 1 : 0);
        u_pal_strength_cached.update(static_cast<float>(OpPalStrength()) / 100.0f);
        u_beam_spread_cached.update(static_cast<float>(OpBeamSpread()) / 100.0f);

        BindTextureUnit(GL_TEXTURE0, fb_texture);
        if (mip_enabled_current)
        {
            if (mip_dirty)
            {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                mip_dirty = false;
            }
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        else
        {
            if (mip_dirty)
            {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                mip_dirty = false;
            }
        }

        glBindVertexArray(vao2); // full NDC quad for the screen pass
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }

}//namespace xPlatform

#endif//USE_GL