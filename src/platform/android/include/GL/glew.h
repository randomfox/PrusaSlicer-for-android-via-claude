// Minimal GLEW compatibility header for Android.
//
// The rendering code includes <GL/glew.h> as the single entry point to OpenGL. On Android
// there is no GLEW: OpenGL ES 3.x entry points are exported directly by libGLESv3, so this
// header pulls in the ES headers, provides stand-ins for the GLEW API and maps the few
// desktop-only OpenGL names used by shared code onto ES equivalents (or harmless values).
//
// NOTE: Stage 1 of the Android port only uses the slicing core; the rendering code is
// compiled but not exercised yet. Desktop-only code paths that are reachable at runtime
// still need to be made ES-aware when the GUI is brought up (stage 2).
#pragma once

#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

// ---- GLEW API ---------------------------------------------------------------------------

#ifndef GLEW_OK
#define GLEW_OK 0
#endif
#ifndef GLEW_VERSION
#define GLEW_VERSION 1
#endif

static inline GLenum glewInit(void) { return GLEW_OK; }
static inline const GLubyte* glewGetErrorString(GLenum) { return (const GLubyte*) "No error"; }
static inline const GLubyte* glewGetString(GLenum) { return (const GLubyte*) "Android GLES"; }
static inline bool glewIsSupported(const char*) { return false; }

#ifndef glewExperimental
static bool glewExperimental = true;
#endif

// An ES context behaves like a core profile (no fixed function pipeline).
#define GLEW_ARB_compatibility 0

// ---- Desktop-only enums -----------------------------------------------------------------

#ifndef GL_FILL
#define GL_FILL 0x1B02
#endif
#ifndef GL_LINE
#define GL_LINE 0x1B01
#endif
#ifndef GL_TEXTURE_1D
#define GL_TEXTURE_1D 0x0DE0
#endif
#ifndef GL_TEXTURE_1D_ARRAY
#define GL_TEXTURE_1D_ARRAY 0x8C18
#endif
#ifndef GL_TEXTURE_RECTANGLE
#define GL_TEXTURE_RECTANGLE 0x84F5
#endif
#ifndef GL_MIRROR_CLAMP_TO_EDGE
#define GL_MIRROR_CLAMP_TO_EDGE 0x8743
#endif
#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif
#ifndef GL_COMPRESSED_RGB
#define GL_COMPRESSED_RGB 0x84ED
#endif
#ifndef GL_COMPRESSED_RGBA
#define GL_COMPRESSED_RGBA 0x84EE
#endif
#ifndef GL_READ_ONLY
#define GL_READ_ONLY 0x88B8
#endif
#ifndef GL_WRITE_ONLY
#define GL_WRITE_ONLY 0x88B9
#endif
#ifndef GL_READ_WRITE
#define GL_READ_WRITE 0x88BA
#endif

// ---- Desktop-only functions mapped onto ES --------------------------------------------

static inline void glClearDepth(double depth) { glClearDepthf(static_cast<GLfloat>(depth)); }

static inline void glDrawBuffer(GLenum buffer) { glDrawBuffers(1, &buffer); }

static inline void* glMapBuffer(GLenum target, GLenum access)
{
    GLint size = 0;
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    GLbitfield flags = 0;
    if (access == GL_READ_ONLY || access == GL_READ_WRITE)
        flags |= GL_MAP_READ_BIT;
    if (access == GL_WRITE_ONLY || access == GL_READ_WRITE)
        flags |= GL_MAP_WRITE_BIT;
    return glMapBufferRange(target, 0, size, flags);
}
