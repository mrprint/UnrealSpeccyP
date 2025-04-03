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
#include "../../ui/ui.h"
#include "../../tools/profiler.h"
#include "../../tools/options.h"

#ifdef USE_GL

#ifdef _WINDOWS
#include <windows.h>
#endif//_WINDOWS

#include <GL/glew.h>


PROFILER_DECLARE(draw_p);
PROFILER_DECLARE(draw);

namespace xPlatform
{

void initGlew()
{
	glewInit();
}

static struct eOptionZoom : public xOptions::eOptionInt
{
	virtual const char* Name() const { return "zoom"; }
	virtual const char** Values() const
	{
		static const char* values[] = { "fill screen", "small border", "no border", NULL };
		return values;
	}
	virtual void Change(bool next = true)
	{
		eOptionInt::Change(0, 3, next);
	}
	virtual int Order() const { return 35; }
	float Zoom() const
	{
		switch (*this)
		{
		case 1: return 300.0f / 256.0f;
		case 2: return 320.0f / 256.0f;
		default: return 1.0f;
		}
	}
} op_zoom;

static float opZoom() { return op_zoom.Zoom(); }

static struct eOptionFiltering : public xOptions::eOptionBool
{
	eOptionFiltering() { Set(true); }
	virtual const char* Name() const { return "filtering"; }
	virtual int Order() const { return 36; }
} op_filtering;

static struct eOptionGigascreen : public xOptions::eOptionBool
{
	eOptionGigascreen() { Set(false); }
	virtual const char* Name() const { return "gigascreen"; }
	virtual int Order() const { return 38; }
} op_gigascreen;

static struct eOptionScanlines : public xOptions::eOptionBool
{
	eOptionScanlines() { Set(false); }
	virtual const char* Name() const { return "scanlines"; }
	virtual int Order() const { return 39; }
} op_scanlines;

static GLuint texture_id1, texture_id2, vao, vbo, ebo;
static GLuint shader_program;
static GLuint u_blend_factor;
static GLuint u_scale;
static GLuint u_show_scanlines;
static GLuint u_texture1;
static GLuint u_texture2;

static dword tex1[512 * 256], *p_tex1 = tex1;
static dword tex2[512 * 256], *p_tex2 = tex2;
static int video_frame_last = -1;

const char* vertex_shader_source = // vertex shader:
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

const char* fragment_shader_source = // fragment shader:
R"(
#version 330 core

out vec4 FragColor;
in vec2 TexCoord;

uniform sampler2D texture1;
uniform sampler2D texture2;
uniform float blendFactor;
uniform bool showScanlines;

void main()
{
	vec4 color;

    if (showScanlines)
    {
        float scanlineEffect = 0.95 + 0.05 * sin(TexCoord.y * 512.0 * 3.14159);
		float blurRadius = 1.0 / (3.5 * 240.0);

		vec4 blurredColor1 = texture(texture1, TexCoord) * 0.5;
		blurredColor1 += texture(texture1, TexCoord - vec2(blurRadius, 0)) * 0.166;
		blurredColor1 += texture(texture1, TexCoord - vec2(blurRadius * 0.666, 0)) * 0.167;
		blurredColor1 += texture(texture1, TexCoord - vec2(blurRadius * 0.333, 0)) * 0.167;


		if (blendFactor > 0.0001)
		{
			vec4 blurredColor2 = texture(texture2, TexCoord) * 0.5;
			blurredColor2 += texture(texture2, TexCoord - vec2(blurRadius, 0)) * 0.166;
			blurredColor2 += texture(texture2, TexCoord - vec2(blurRadius * 0.666, 0)) * 0.167;
			blurredColor2 += texture(texture2, TexCoord - vec2(blurRadius * 0.333, 0)) * 0.167;
			color = mix(blurredColor1, blurredColor2, blendFactor) * scanlineEffect;
		}
		else
		{
			color = blurredColor1 * scanlineEffect;
		}
    }
    else
    {
        vec4 color1 = texture(texture1, TexCoord);
		vec4 color2 = texture(texture2, TexCoord);
		color = mix(color1, color2, blendFactor);
    }

    FragColor = color;
}
)";

void initGraphics()
{
	glGenTextures(1, &texture_id1);
	glGenTextures(1, &texture_id2);

	GLint filter = op_filtering ? GL_LINEAR : GL_NEAREST;

	glBindTexture(GL_TEXTURE_2D, texture_id1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glBindTexture(GL_TEXTURE_2D, texture_id2);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	float vertices[] = {
		-1.0f,  1.0f, 0.0f,            0.0f,
		-1.0f, -1.0f, 0.0f,            240.0f / 256.0f,
		 1.0f, -1.0f, 320.0f / 512.0f, 240.0f / 256.0f,
		 1.0f,  1.0f, 320.0f / 512.0f, 0.0f
	};

	unsigned int indices[] = { 0, 1, 2, 0, 2, 3 };

	glGenVertexArrays(1, &vao);
	glGenBuffers(1, &vbo);
	glGenBuffers(1, &ebo);

	glBindVertexArray(vao);

	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
	glEnableVertexAttribArray(1);

	GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vertexShader, 1, &vertex_shader_source, NULL);
	glCompileShader(vertexShader);

	GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fragmentShader, 1, &fragment_shader_source, NULL);
	glCompileShader(fragmentShader);

	shader_program = glCreateProgram();
	glAttachShader(shader_program, vertexShader);
	glAttachShader(shader_program, fragmentShader);
	glLinkProgram(shader_program);

	glDeleteShader(vertexShader);
	glDeleteShader(fragmentShader);

	glUseProgram(shader_program);
	u_blend_factor = glGetUniformLocation(shader_program, "blendFactor");
	u_scale = glGetUniformLocation(shader_program, "scale");
	u_show_scanlines = glGetUniformLocation(shader_program, "showScanlines");
	u_texture1 = glGetUniformLocation(shader_program, "texture1");
	u_texture2 = glGetUniformLocation(shader_program, "texture2");
}

void cleanupGraphics()
{
	glDeleteTextures(1, &texture_id1);
	glDeleteTextures(1, &texture_id2);
	glDeleteBuffers(1, &vbo);
	glDeleteBuffers(1, &ebo);
	glDeleteVertexArrays(1, &vao);
	glDeleteProgram(shader_program);
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
		for(int c = 0; c < 16; ++c)
		{
			byte i = c&8 ? brightness + bright_intensity : brightness;
			byte b = c&1 ? i : 0;
			byte r = c&2 ? i : 0;
			byte g = c&4 ? i : 0;
			items[c] = RGBX(r, g, b);
		}
	}
	dword items[16];
}
color_cache;

void DrawGL(int _w, int _h)
{
	PROFILER_BEGIN(draw_p);

	bool giga_enabled = op_gigascreen;

	if (giga_enabled && video_frame_last != Handler()->VideoFrame())
	{
		video_frame_last = Handler()->VideoFrame();
		std::swap(p_tex1, p_tex2);
	}

	byte* data = (byte*)Handler()->VideoData();
	dword* p = p_tex1; // swap?

#ifdef USE_UI
	byte* data_ui = (byte*)Handler()->VideoDataUI();
	if(data_ui)
	{
		for(int y = 0; y < 240; ++y)
		{
			for(int x = 0; x < 320; ++x)
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
		for(int y = 0; y < 240; ++y)
		{
			for(int x = 0; x < 320; ++x)
			{
				*p++ = color_cache.items[*data++];
			}
			p += 512 - 320;
		}
	}
	PROFILER_END(draw_p);

	PROFILER_SECTION(draw);

	float aspect_src = 320.0f / 240.0f;
	float aspect_dst = (float)_w / (float)_h;
	float scale_x = 1.0f, scale_y = 1.0f;

	if (aspect_dst > aspect_src) {
		scale_x = aspect_src / aspect_dst;
	}
	else {
		scale_y = aspect_dst / aspect_src;
	}

	glViewport(0, 0, _w, _h);

	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(shader_program);
	glUniform1f(u_blend_factor, (giga_enabled) ? 0.5f : 0.0f);
	glUniform2f(u_scale, scale_x * opZoom(), scale_y * opZoom());
	glUniform1f(u_show_scanlines, op_scanlines);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture_id1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, p_tex1);
	glUniform1i(u_texture1, 0);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, texture_id2);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, p_tex2);
	glUniform1i(u_texture2, 1);

	glBindVertexArray(vao);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

	glBindVertexArray(0);
}

}
//namespace xPlatform

#endif//USE_GL
