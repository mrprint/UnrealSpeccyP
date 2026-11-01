#ifndef __GL_VERSION_H__
#define __GL_VERSION_H__

#pragma once

// =============================================================================
//  platform/gl/gl_version.h
//
//  The OpenGL version the renderer (draw.cpp) is written for, and the GLSL that
//  goes with it. One place, so that the context that gets created
//  (sdl2_desktop_video.cpp), the shaders (draw.cpp) and the Dear ImGui backend
//  (sdl2_desktop_imgui.cpp) always agree.
//
//  Everywhere but macOS: OpenGL 3.0 / GLSL 1.30. That is the oldest version with
//  all that the renderer uses - vertex array objects are the newest of it - and
//  the shaders are plain GLSL 1.30 (in/out, texture(), fwidth()). A driver may
//  still hand out a newer context than asked for; that is fine.
//
//  macOS: Apple offers only a legacy 2.1 context or a core-profile one from 3.2
//  up, and a core profile takes GLSL 1.50 and up, not 1.30. 3.2 / 1.50 is the
//  oldest that fits. The shaders are the same text - 1.50 accepts what 1.30 does
//  here. (Not built or run on macOS yet.)
//
//  Any of the values can be overridden from the build, e.g. to get the previous
//  3.3 core context everywhere:
//      -DRENDERER_GL_MAJOR=3 -DRENDERER_GL_MINOR=3 -DRENDERER_GLSL_VERSION=\"#version 330\"
// =============================================================================

#if defined(__APPLE__)
	#ifndef RENDERER_GL_MAJOR
	#define RENDERER_GL_MAJOR 3
	#endif
	#ifndef RENDERER_GL_MINOR
	#define RENDERER_GL_MINOR 2
	#endif
	#ifndef RENDERER_GLSL_VERSION
	#define RENDERER_GLSL_VERSION "#version 150"
	#endif
#else
	#ifndef RENDERER_GL_MAJOR
	#define RENDERER_GL_MAJOR 3
	#endif
	#ifndef RENDERER_GL_MINOR
	#define RENDERER_GL_MINOR 0
	#endif
	#ifndef RENDERER_GLSL_VERSION
	#define RENDERER_GLSL_VERSION "#version 130"
	#endif
#endif

#if RENDERER_GL_MAJOR < 3
	#error "The renderer needs OpenGL 3.0 at least (vertex array objects, GLSL 1.30)"
#endif

// Profiles exist from 3.2 on (a request for one below that is meaningless).
#if RENDERER_GL_MAJOR > 3 || (RENDERER_GL_MAJOR == 3 && RENDERER_GL_MINOR >= 2)
	#define RENDERER_GL_CORE_PROFILE 1
#else
	#define RENDERER_GL_CORE_PROFILE 0
#endif

// The first line of a shader source, for ImGui_ImplOpenGL3_Init() without the
// newline (RENDERER_GLSL_VERSION), and with it (RENDERER_GLSL_VERSION_LINE).
#define RENDERER_GLSL_VERSION_LINE RENDERER_GLSL_VERSION "\n"

#endif//__GL_VERSION_H__
