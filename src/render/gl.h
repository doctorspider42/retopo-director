#pragma once

// A hand rolled OpenGL 3.3 core loader.
//
// Everything from GL 1.1 comes from the system header and the platform library;
// anything newer is fetched through glfwGetProcAddress into a function pointer
// with the same name. That avoids vendoring a generator and keeps the surface
// down to exactly the calls this renderer makes.

#if defined(RD_PLATFORM_WINDOWS)
#  include <windows.h>
#endif

#include <GL/gl.h>

#include <cstddef>
#include <cstdint>

// Types the 1.1 headers predate.
typedef char          GLchar;
typedef ptrdiff_t     GLsizeiptr;
typedef ptrdiff_t     GLintptr;

// ---------------------------------------------------------------------------
// Constants beyond GL 1.1
// ---------------------------------------------------------------------------
#define GL_ARRAY_BUFFER                   0x8892
#define GL_ELEMENT_ARRAY_BUFFER           0x8893
#define GL_STATIC_DRAW                    0x88E4
#define GL_DYNAMIC_DRAW                   0x88E8
#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_INFO_LOG_LENGTH                0x8B84
#define GL_TEXTURE0                       0x84C0
#define GL_TEXTURE1                       0x84C1
#define GL_TEXTURE_2D_MULTISAMPLE         0x9100
#define GL_FRAMEBUFFER                    0x8D40
#define GL_READ_FRAMEBUFFER               0x8CA8
#define GL_DRAW_FRAMEBUFFER               0x8CA9
#define GL_RENDERBUFFER                   0x8D41
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_DEPTH_ATTACHMENT               0x8D00
#define GL_DEPTH_STENCIL_ATTACHMENT       0x821A
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#define GL_FRAMEBUFFER_BINDING            0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING       0x8CAA
#define GL_DEPTH24_STENCIL8               0x88F0
#define GL_DEPTH_COMPONENT24              0x81A6
#define GL_RGBA8                          0x8058
#define GL_SRGB8_ALPHA8                   0x8C43
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_MULTISAMPLE                    0x809D
#define GL_FRAMEBUFFER_SRGB               0x8DB9
#define GL_MAX_SAMPLES                    0x8D57
#define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#define GL_DEBUG_OUTPUT                   0x92E0
#define GL_DEBUG_OUTPUT_SYNCHRONOUS       0x8242
#define GL_LINE_SMOOTH                    0x0B20
#define GL_POLYGON_OFFSET_LINE            0x2A02

// ---------------------------------------------------------------------------
// Function pointers
// ---------------------------------------------------------------------------
#define RD_GL_FUNCTIONS(X)                                                                        \
    X(void,    glGenVertexArrays,        (GLsizei, GLuint*))                                      \
    X(void,    glBindVertexArray,        (GLuint))                                                \
    X(void,    glDeleteVertexArrays,     (GLsizei, const GLuint*))                                \
    X(void,    glGenBuffers,             (GLsizei, GLuint*))                                      \
    X(void,    glBindBuffer,             (GLenum, GLuint))                                        \
    X(void,    glBufferData,             (GLenum, GLsizeiptr, const void*, GLenum))               \
    X(void,    glBufferSubData,          (GLenum, GLintptr, GLsizeiptr, const void*))             \
    X(void,    glDeleteBuffers,          (GLsizei, const GLuint*))                                \
    X(void,    glVertexAttribPointer,    (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))\
    X(void,    glEnableVertexAttribArray,(GLuint))                                                \
    X(void,    glDisableVertexAttribArray,(GLuint))                                               \
    X(GLuint,  glCreateShader,           (GLenum))                                                \
    X(void,    glShaderSource,           (GLuint, GLsizei, const GLchar* const*, const GLint*))   \
    X(void,    glCompileShader,          (GLuint))                                                \
    X(void,    glGetShaderiv,            (GLuint, GLenum, GLint*))                                \
    X(void,    glGetShaderInfoLog,       (GLuint, GLsizei, GLsizei*, GLchar*))                    \
    X(void,    glDeleteShader,           (GLuint))                                                \
    X(GLuint,  glCreateProgram,          (void))                                                  \
    X(void,    glAttachShader,           (GLuint, GLuint))                                        \
    X(void,    glLinkProgram,            (GLuint))                                                \
    X(void,    glGetProgramiv,           (GLuint, GLenum, GLint*))                                \
    X(void,    glGetProgramInfoLog,      (GLuint, GLsizei, GLsizei*, GLchar*))                    \
    X(void,    glUseProgram,             (GLuint))                                                \
    X(void,    glDeleteProgram,          (GLuint))                                                \
    X(GLint,   glGetUniformLocation,     (GLuint, const GLchar*))                                 \
    X(void,    glUniform1i,              (GLint, GLint))                                          \
    X(void,    glUniform1f,              (GLint, GLfloat))                                        \
    X(void,    glUniform2fv,             (GLint, GLsizei, const GLfloat*))                        \
    X(void,    glUniform3fv,             (GLint, GLsizei, const GLfloat*))                        \
    X(void,    glUniform4fv,             (GLint, GLsizei, const GLfloat*))                        \
    X(void,    glUniformMatrix4fv,       (GLint, GLsizei, GLboolean, const GLfloat*))             \
    X(void,    glActiveTexture,          (GLenum))                                                \
    X(void,    glGenerateMipmap,         (GLenum))                                                \
    X(void,    glGenFramebuffers,        (GLsizei, GLuint*))                                      \
    X(void,    glBindFramebuffer,        (GLenum, GLuint))                                        \
    X(void,    glFramebufferTexture2D,   (GLenum, GLenum, GLenum, GLuint, GLint))                 \
    X(GLenum,  glCheckFramebufferStatus, (GLenum))                                                \
    X(void,    glDeleteFramebuffers,     (GLsizei, const GLuint*))                                \
    X(void,    glGenRenderbuffers,       (GLsizei, GLuint*))                                      \
    X(void,    glBindRenderbuffer,       (GLenum, GLuint))                                        \
    X(void,    glRenderbufferStorage,    (GLenum, GLenum, GLsizei, GLsizei))                      \
    X(void,    glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))     \
    X(void,    glFramebufferRenderbuffer,(GLenum, GLenum, GLenum, GLuint))                        \
    X(void,    glDeleteRenderbuffers,    (GLsizei, const GLuint*))                                \
    X(void,    glBlitFramebuffer,        (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, \
                                          GLbitfield, GLenum))

#define RD_GL_DECLARE(ret, name, params) extern ret (APIENTRY* rd_##name) params;
RD_GL_FUNCTIONS(RD_GL_DECLARE)
#undef RD_GL_DECLARE

// The pointers are exposed under their real names, so the rest of the renderer
// reads like ordinary GL code.
#define glGenVertexArrays        rd_glGenVertexArrays
#define glBindVertexArray        rd_glBindVertexArray
#define glDeleteVertexArrays     rd_glDeleteVertexArrays
#define glGenBuffers             rd_glGenBuffers
#define glBindBuffer             rd_glBindBuffer
#define glBufferData             rd_glBufferData
#define glBufferSubData          rd_glBufferSubData
#define glDeleteBuffers          rd_glDeleteBuffers
#define glVertexAttribPointer    rd_glVertexAttribPointer
#define glEnableVertexAttribArray  rd_glEnableVertexAttribArray
#define glDisableVertexAttribArray rd_glDisableVertexAttribArray
#define glCreateShader           rd_glCreateShader
#define glShaderSource           rd_glShaderSource
#define glCompileShader          rd_glCompileShader
#define glGetShaderiv            rd_glGetShaderiv
#define glGetShaderInfoLog       rd_glGetShaderInfoLog
#define glDeleteShader           rd_glDeleteShader
#define glCreateProgram          rd_glCreateProgram
#define glAttachShader           rd_glAttachShader
#define glLinkProgram            rd_glLinkProgram
#define glGetProgramiv           rd_glGetProgramiv
#define glGetProgramInfoLog      rd_glGetProgramInfoLog
#define glUseProgram             rd_glUseProgram
#define glDeleteProgram          rd_glDeleteProgram
#define glGetUniformLocation     rd_glGetUniformLocation
#define glUniform1i              rd_glUniform1i
#define glUniform1f              rd_glUniform1f
#define glUniform2fv             rd_glUniform2fv
#define glUniform3fv             rd_glUniform3fv
#define glUniform4fv             rd_glUniform4fv
#define glUniformMatrix4fv       rd_glUniformMatrix4fv
#define glActiveTexture          rd_glActiveTexture
#define glGenerateMipmap         rd_glGenerateMipmap
#define glGenFramebuffers        rd_glGenFramebuffers
#define glBindFramebuffer        rd_glBindFramebuffer
#define glFramebufferTexture2D   rd_glFramebufferTexture2D
#define glCheckFramebufferStatus rd_glCheckFramebufferStatus
#define glDeleteFramebuffers     rd_glDeleteFramebuffers
#define glGenRenderbuffers       rd_glGenRenderbuffers
#define glBindRenderbuffer       rd_glBindRenderbuffer
#define glRenderbufferStorage    rd_glRenderbufferStorage
#define glRenderbufferStorageMultisample rd_glRenderbufferStorageMultisample
#define glFramebufferRenderbuffer rd_glFramebufferRenderbuffer
#define glDeleteRenderbuffers    rd_glDeleteRenderbuffers
#define glBlitFramebuffer        rd_glBlitFramebuffer

namespace rd::gl {

// Must be called with a context already current. Returns false when a required
// entry point is missing; last_load_error() then says which one.
bool        load();
bool        loaded();
const char* last_load_error();

// Logs and clears any pending GL error. `where` is used in the message.
void check_error(const char* where);

} // namespace rd::gl
