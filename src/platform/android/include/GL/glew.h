// Minimal GLEW compatibility header for Android.
//
// The rendering code includes <GL/glew.h> as the single entry point to OpenGL. On Android
// there is no GLEW: OpenGL ES 3.x entry points are exported directly by libGLESv3, so this
// header just pulls in the ES headers and provides no-op stand-ins for the GLEW API.
#pragma once

#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#ifndef GLEW_OK
#define GLEW_OK 0
#endif
#ifndef GLEW_VERSION
#define GLEW_VERSION 1
#endif

typedef unsigned int GLenum;

static inline GLenum glewInit(void) { return GLEW_OK; }
static inline const GLubyte* glewGetErrorString(GLenum) { return (const GLubyte*) "No error"; }
static inline const GLubyte* glewGetString(GLenum) { return (const GLubyte*) "Android GLES"; }
static inline bool glewIsSupported(const char*) { return false; }

#ifndef glewExperimental
static bool glewExperimental = true;
#endif

// Desktop GL enums that are referenced in shared code paths but have no ES equivalent.
#ifndef GL_FILL
#define GL_FILL 0x1B02
#endif
#ifndef GL_LINE
#define GL_LINE 0x1B01
#endif
