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
            title,
            wxOK | wxICON_ERROR);
#else
        fprintf(stderr, "%s: %s\n", title, message);
#endif

        if (exit_on_error) {
            exit(EXIT_FAILURE);
        }
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
        virtual void Change(bool next = true)
        {
            eOptionEnum::Change(0, 3, next);
        }
        virtual int Order() const { return 35; }
        float Zoom() const
        {
            switch (*this)
            {
            case 1: return 300.0f / 256.0f;
            case 2: return 320.0f / 256.0f;
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

    static GLuint fb_texture{}, texture_id1{}, texture_id2{};
    static GLuint ebo{};
    static GLuint vao1{}, vbo1{};
    static GLuint vao2{}, vbo2{};
    static GLuint fbo{};
    static GLuint fb_shader{}, screen_shader{};
    static GLuint u_blend_factor{};
    static GLuint u_scale{};
    static GLuint u_simple_scale{};
    static GLuint u_show_scanlines{};
    static GLuint u_texture1{};
    static GLuint u_texture2{};
    static GLuint u_fb_texture{};
    static GLuint u_enable_pal_effects{};
    static GLuint u_enable_dot_crawl{};
    static GLuint u_enable_phase_mod{};
    static GLuint u_pal_strength{};
    static GLuint u_beam_spread{};
    static GLuint u_frame_count{};
    static GLuint u_fb_size{};
    static GLuint u_mask_scale{};

    static int fb_width = sline_len * 4, fb_height = slines_cnt * 4;

    static dword tex1[sline_len * slines_cnt], * p_tex1 = tex1;
    static dword tex2[sline_len * slines_cnt], * p_tex2 = tex2;
    static int video_frame_last = -1;

    const char* vertex_src = // vertex shader:
        R"(
#version 330 core

layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoord;
out vec2 TexCoord;

uniform vec2 scale;

void main()
{
    vec2 pos = aPos * scale;
    gl_Position = vec4(pos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

    const char* fb_fragment_src = // fragment shader:
        R"(
#version 330 core

out vec4 FragColor;
in vec2 TexCoord;

uniform sampler2D texture1;
uniform sampler2D texture2;
uniform float blendFactor;

void main()
{
    vec4 color1 = texture(texture1, TexCoord);
    vec4 color2 = texture(texture2, TexCoord);
    vec4 color = mix(color1, color2, blendFactor);
    FragColor = color;
}
)";

    const char* screen_fragment_src =  // fragment shader:
        R"(
#version 330 core

in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D fbTexture;
uniform bool  showScanlines;
uniform bool  enablePalEffects;
uniform bool  enableDotCrawl;
uniform bool  enablePhaseModulation;

uniform float palStrength;   // 0.0 - 1.0
uniform float beamSpread;    // 0.0 - 2.0
uniform float uFrameCount;
uniform float maskScale;
uniform vec2 fbSize;

#define slines_cnt 256.0
#define sline_len 512.0
#define PI 3.14159265359

void main()
{
    vec2 texel = 1.0 / fbSize;
    vec2 uv = TexCoord;

    vec4 color = texture(fbTexture, uv);

    if (enablePalEffects)
    {
        // -------------------------------------------
        // PAL artifacts
        // -------------------------------------------

        vec2 zxTexel = 1.0 / vec2(sline_len, slines_cnt);

        // Dot Crawl
        if (enableDotCrawl)
        {
            float crawl =
                sin(uFrameCount * 0.15 + uv.x * sline_len * 0.25) *
                palStrength * zxTexel.x * 0.4;
            uv.x += crawl;
        }

        // Chromatic Aberration
        float chromaShift = palStrength * zxTexel.x;

        vec4 cr = texture(fbTexture, uv - vec2(chromaShift, 0.0));
        vec4 cg = texture(fbTexture, uv);
        vec4 cb = texture(fbTexture, uv + vec2(chromaShift * 1.25, 0.0));

        // Green Dot Artifact
        cr.rgb = mix(
            cr.rgb,
            vec3(cr.g * 1.1, cr.g, cr.b * 0.9),
            palStrength * 0.6
        );

        // Phase Modulation
        if (enablePhaseModulation)
        {
            float phase =
                sin(uv.x * sline_len * 0.5 + uFrameCount * 0.2) *
                palStrength * 0.04;

            cr.rgb += vec3( phase, -phase * 0.4, 0.0);
            cb.rgb -= vec3( phase * 0.4, 0.0, phase);
        }

        // Luminance-Chrominance Crosstalk
        float luma = dot(
            vec3(cr.r, cg.g, cb.b),
            vec3(0.299, 0.587, 0.114)
        );

        vec3 crosstalk =
            (luma - vec3(cr.r, cg.g, cb.b)) *
            palStrength * 0.025;

        cr.rgb += crosstalk;
        cg.rgb += crosstalk;
        cb.rgb += crosstalk;

        vec4 palColor = vec4(cr.r, cg.g, cb.b, color.a);

        // CRT beam spread
        float intensity = dot(palColor.rgb, vec3(0.299, 0.587, 0.114));
        float spread = beamSpread * (0.3 + intensity * 0.7);

        vec4 left  = texture(fbTexture, uv - vec2(texel.x * spread, 0.0));
        vec4 right = texture(fbTexture, uv + vec2(texel.x * spread, 0.0));

        color = mix(
            palColor,
            palColor * 0.5 + left * 0.25 + right * 0.25,
            beamSpread
        );
    }

    // CRT phosphor mask (UKTV-style)

    float x = floor(gl_FragCoord.x / maskScale);
    float phase = x * 2.0 * PI / 3.0;

    vec3 mask;
    mask.r = 0.90 + 0.10 * sin(phase);
    mask.g = 0.95 + 0.05 * sin(phase + 2.0 * PI / 3.0);
    mask.b = 0.85 + 0.15 * sin(phase + 4.0 * PI / 3.0);

    float luma = dot(color.rgb, vec3(0.299, 0.587, 0.114));
    float maskStrength = smoothstep(0.2, 0.8, luma);

    color.rgb *= mix(vec3(1.0), mask, maskStrength * 0.35);

    if (showScanlines)
    {
        float scan =
        0.97 + 0.03 *
        sin(TexCoord.y * slines_cnt * 2.0 * PI);

        color.rgb *= scan;
    }

    const vec3 whiteBalance = vec3(
        1.02,  // R+
        0.985,  // G-
        1.03   // B+
    );
    color.rgb *= whiteBalance;

    FragColor = color;
}
)";

    GLuint compileShader(GLenum type, const char* src)
    {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);

        GLint success;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            char log[512];
            glGetShaderInfoLog(shader, 512, nullptr, log);
            reportError(
                (type == GL_VERTEX_SHADER) ? "Vertex Shader Error" : "Fragment Shader Error",
                log
            );
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    GLuint linkProgram(GLuint vert, GLuint frag)
    {
        GLuint program = glCreateProgram();
        glAttachShader(program, vert);
        glAttachShader(program, frag);
        glLinkProgram(program);

        GLint success;
        glGetProgramiv(program, GL_LINK_STATUS, &success);
        if (!success)
        {
            char log[512];
            glGetProgramInfoLog(program, 512, nullptr, log);
            reportError("Program Link Error", log);
            glDeleteProgram(program);
            return 0;
        }
        return program;
    }

    void initGraphics(int scr_width, int scr_height)
    {
        if (scr_height != -1)
        {
            float base_scale =
                (float)((scr_width > scr_height) ? scr_width : scr_height) /
                (float)((scr_width > scr_height) ? sline_len : slines_cnt);

            base_scale *= 2.0f;

            fb_width = int(sline_len * base_scale);
            fb_height = int(slines_cnt * base_scale);
        }

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        glGenTextures(1, &texture_id1);
        glGenTextures(1, &texture_id2);
        glGenTextures(1, &fb_texture);

        glBindTexture(GL_TEXTURE_2D, texture_id1);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, sline_len, slines_cnt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindTexture(GL_TEXTURE_2D, texture_id2);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, sline_len, slines_cnt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindTexture(GL_TEXTURE_2D, fb_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, fb_width, fb_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

#ifdef GLEW_EXT_texture_filter_anisotropic
        if (GLEW_EXT_texture_filter_anisotropic)
        {
            GLfloat maxAniso = 0.0f;
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso);
            GLfloat aniso = (maxAniso > 0.0f) ? maxAniso : 1.0f;
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
        }
#endif

        float vertices[] = {
            //  pos       // tex
            -1.0f,  1.0f,  0.0f, 1.0f,
            -1.0f, -1.0f,  0.0f, 0.0f,
             1.0f, -1.0f,  1.0f, 0.0f,
             1.0f,  1.0f,  1.0f, 1.0f
        };

        float vertices_scaled[] = {
            -1.0f,  1.0f, 0.0f,            0.0f,
            -1.0f, -1.0f, 0.0f,            240.0f / 256.0f,
             1.0f, -1.0f, 320.0f / 512.0f, 240.0f / 256.0f,
             1.0f,  1.0f, 320.0f / 512.0f, 0.0f
        };

        unsigned int indices[] = { 0, 1, 2, 0, 2, 3 };

        glGenVertexArrays(1, &vao1);
        glGenBuffers(1, &vbo1);
        glGenBuffers(1, &ebo);

        glBindVertexArray(vao1);

        glBindBuffer(GL_ARRAY_BUFFER, vbo1);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices_scaled), vertices_scaled, GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);

        glGenVertexArrays(1, &vao2);
        glGenBuffers(1, &vbo2);

        glBindVertexArray(vao2);

        glBindBuffer(GL_ARRAY_BUFFER, vbo2);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);

        // Create and attach framebuffer
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb_texture, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            reportError("Framebuffer Not Complete", "The framebuffer is not properly configured.");

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertex_src);
        GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fb_fragment_src);
        GLuint simpleFragmentShader = compileShader(GL_FRAGMENT_SHADER, screen_fragment_src);

        fb_shader = linkProgram(vertexShader, fragmentShader);
        screen_shader = linkProgram(vertexShader, simpleFragmentShader);

        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        glDeleteShader(simpleFragmentShader);

        glUseProgram(fb_shader);
        u_scale = glGetUniformLocation(fb_shader, "scale");

        u_blend_factor = glGetUniformLocation(fb_shader, "blendFactor");
        u_texture1 = glGetUniformLocation(fb_shader, "texture1");
        u_texture2 = glGetUniformLocation(fb_shader, "texture2");

        glUseProgram(screen_shader);
        u_simple_scale = glGetUniformLocation(screen_shader, "scale");

        u_fb_texture = glGetUniformLocation(screen_shader, "fbTexture");
        u_show_scanlines = glGetUniformLocation(screen_shader, "showScanlines");

        u_enable_pal_effects = glGetUniformLocation(screen_shader, "enablePalEffects");
        u_enable_dot_crawl = glGetUniformLocation(screen_shader, "enableDotCrawl");
        u_enable_phase_mod = glGetUniformLocation(screen_shader, "enablePhaseModulation");
        u_pal_strength = glGetUniformLocation(screen_shader, "palStrength");
        u_beam_spread = glGetUniformLocation(screen_shader, "beamSpread");
        u_frame_count = glGetUniformLocation(screen_shader, "uFrameCount");
        u_fb_size = glGetUniformLocation(screen_shader, "fbSize");
        u_mask_scale = glGetUniformLocation(screen_shader, "maskScale");

        if (u_enable_pal_effects == -1 || u_enable_dot_crawl == -1
            || u_enable_phase_mod == -1 || u_pal_strength == -1 || u_beam_spread == -1)
        {
            reportError("Shader Uniform Error", "Failed to get uniform locations.");
        }
    }

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
    }

#ifdef USE_BIG_ENDIAN
#define RGBX(r, g, b) (((r) << 24)|((g) << 16)|((b) << 8))
#else//USE_BIG_ENDIAN
#define RGBX(r, g, b) (((b) << 16)|((g) << 8)|(r))
#endif//USE_BIG_ENDIAN

    //=============================================================================
    //	DrawGL
    //-----------------------------------------------------------------------------

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
    }
    color_cache;

    void DrawGL(int vport_width, int vport_height)
    {
        PROFILER_BEGIN(draw_p);

        bool giga_enabled = op_gigascreen;

        if (giga_enabled && video_frame_last != Handler()->VideoFrame())
        {
            std::swap(p_tex1, p_tex2);
        }
        video_frame_last = Handler()->VideoFrame();

        byte* data = (byte*)Handler()->VideoData();
        dword* p = p_tex1; // swap?

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
                    *p++ = RGBX((c.r >> c_ui.a) + c_ui.r, (c.g >> c_ui.a) + c_ui.g, (c.b >> c_ui.a) + c_ui.b);
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
                {
                    *p++ = color_cache.items[*data++];
                }
                p += 512 - 320;
            }
        }
        PROFILER_END(draw_p);

        PROFILER_SECTION(draw);

        float aspect_src = 320.0f / 240.0f;
        float aspect_dst = (float)vport_width / (float)vport_height;
        float scale_x = 1.0f, scale_y = 1.0f;

        if (aspect_dst > aspect_src) {
            scale_x = aspect_src / aspect_dst;
        }
        else {
            scale_y = aspect_dst / aspect_src;
        }

        // 1st pass: render to FBO

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, fb_width, fb_height);

        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(fb_shader);
        glUniform1f(u_blend_factor, (giga_enabled) ? 0.5f : 0.0f);
        glUniform2f(u_scale, 1.0f, 1.0f);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture_id1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sline_len, slines_cnt, GL_RGBA, GL_UNSIGNED_BYTE, p_tex1);
        glUniform1i(u_texture1, 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, texture_id2);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sline_len, slines_cnt, GL_RGBA, GL_UNSIGNED_BYTE, p_tex2);
        glUniform1i(u_texture2, 1);

        glBindVertexArray(vao2);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

        // 2nd pass: render FBO texture to screen (downsampled)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, vport_width, vport_height);

        glClear(GL_COLOR_BUFFER_BIT);

        // In the screen shader section of DrawGL():
        glUseProgram(screen_shader);
        glUniform1f(u_show_scanlines, op_scanlines && vport_height > slines_cnt + slines_cnt / 2);
        glUniform2f(u_simple_scale, scale_x * opZoom(), scale_y * opZoom());
        glUniform1i(u_fb_texture, 0);
        glUniform2f(u_fb_size, float(fb_width), float(fb_height));
        glUniform1f(u_mask_scale, 2.0);

        // Pass PAL options from xOptions
        bool pal_enabled = OpPalEffects();
        bool dot_crawl_enabled = OpDotCrawl();
        bool phase_mod_enabled = OpPhaseMod();
        int pal_strength_val = OpPalStrength();  // 0-100 -> convert to 0.0-1.0
        int beam_spread_val = OpBeamSpread();    // 0-200 -> convert to 0.0-2.0

        glUniform1i(u_enable_pal_effects, pal_enabled);
        glUniform1i(u_enable_dot_crawl, dot_crawl_enabled);
        glUniform1i(u_enable_phase_mod, phase_mod_enabled);
        glUniform1f(u_pal_strength, static_cast<float>(pal_strength_val) / 100.0f);
        glUniform1f(u_beam_spread, static_cast<float>(beam_spread_val) / 100.0f * 2.0f);
        glUniform1f(u_frame_count, static_cast<float>(video_frame_last)); // Animate dot crawl

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fb_texture);
        glGenerateMipmap(GL_TEXTURE_2D);

        glBindVertexArray(vao1);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }

}
//namespace xPlatform

#endif//USE_GL
