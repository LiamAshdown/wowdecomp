#ifndef ENGINE_SOURCE_GX_CGXDEVICEOPENGL_WEB_GLSHIM_H
#define ENGINE_SOURCE_GX_CGXDEVICEOPENGL_WEB_GLSHIM_H

// Web build: what GlExt.h gets instead of <windows.h> + <GL/gl.h>.
//
// CGxDeviceOpenGl drives a GL 1.x fixed-function pipeline with ARB extensions. WebGL2 has none of the fixed-function
// state, so the legacy entry points the device uses are redirected (macros below) to GlShim.cpp, which tracks that
// state and draws through generated GLSL ES 3.00 programs. Core GL calls that WebGL2 also has go straight through.

#include <windows.h>

#define GL_GLEXT_LEGACY
// GlExt.h declares the ARB multitexture entry points as function pointers.
#define GL_ARB_multitexture 1
#include <GL/gl.h>

#define DECLARE_HANDLE(name) \
    struct name##__ {        \
        int unused;          \
    };                       \
    typedef struct name##__* name

typedef WORD ATOM;
typedef HANDLE HGLRC;

void GlShimEnable(GLenum cap);
void GlShimDisable(GLenum cap);
void GlShimTexEnvi(GLenum target, GLenum pname, GLint param);
void GlShimTexEnvf(GLenum target, GLenum pname, GLfloat param);
void GlShimTexEnvfv(GLenum target, GLenum pname, const GLfloat* params);
void GlShimTexEnviv(GLenum target, GLenum pname, const GLint* params);
void GlShimTexParameteri(GLenum target, GLenum pname, GLint param);
void GlShimLightf(GLenum light, GLenum pname, GLfloat param);
void GlShimLightfv(GLenum light, GLenum pname, const GLfloat* params);
void GlShimLightModelfv(GLenum pname, const GLfloat* params);
void GlShimLightModeli(GLenum pname, GLint param);
void GlShimMaterialf(GLenum face, GLenum pname, GLfloat param);
void GlShimMaterialfv(GLenum face, GLenum pname, const GLfloat* params);
void GlShimColorMaterial(GLenum face, GLenum mode);
void GlShimTexGeni(GLenum coord, GLenum pname, GLint param);
void GlShimTexGenfv(GLenum coord, GLenum pname, const GLfloat* params);
void GlShimFogf(GLenum pname, GLfloat param);
void GlShimFogi(GLenum pname, GLint param);
void GlShimFogfv(GLenum pname, const GLfloat* params);
void GlShimAlphaFunc(GLenum func, GLclampf ref);
void GlShimClipPlane(GLenum plane, const GLdouble* equation);
void GlShimColor4fv(const GLfloat* v);
void GlShimMatrixMode(GLenum mode);
void GlShimLoadMatrixf(const GLfloat* m);
void GlShimLoadIdentity();
void GlShimEnableClientState(GLenum array);
void GlShimDisableClientState(GLenum array);
void GlShimVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer);
void GlShimNormalPointer(GLenum type, GLsizei stride, const GLvoid* pointer);
void GlShimColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer);
void GlShimTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer);
void GlShimDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices);
void GlShimDrawArrays(GLenum mode, GLint first, GLsizei count);
void GlShimBindTexture(GLenum target, GLuint texture);
void GlShimDeleteTextures(GLsizei n, const GLuint* textures);
void GlShimTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid* pixels);
void GlShimTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid* pixels);
void GlShimCopyTexImage2D(GLenum target, GLint level, GLenum internalFormat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
void GlShimReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid* pixels);
void GlShimPixelStorei(GLenum pname, GLint param);
void GlShimPolygonMode(GLenum face, GLenum mode);
void GlShimPointSize(GLfloat size);
void GlShimDepthRange(GLclampd nearVal, GLclampd farVal);
void GlShimFinish();
void GlShimHint(GLenum target, GLenum mode);
void GlShimGetIntegerv(GLenum pname, GLint* params);
void GlShimGetFloatv(GLenum pname, GLfloat* params);

// Called once the WebGL2 context is current: sets the GlExt.h extension flags and function pointers.
void GlShimInitialize();

// Debugging: when set, the next frame's draws are logged with their state (until the next call).
void GlShimSetDump(bool dump);

#define glEnable GlShimEnable
#define glDisable GlShimDisable
#define glTexEnvi GlShimTexEnvi
#define glTexEnvf GlShimTexEnvf
#define glTexEnvfv GlShimTexEnvfv
#define glTexEnviv GlShimTexEnviv
#define glTexParameteri GlShimTexParameteri
#define glLightf GlShimLightf
#define glLightfv GlShimLightfv
#define glLightModelfv GlShimLightModelfv
#define glLightModeli GlShimLightModeli
#define glMaterialf GlShimMaterialf
#define glMaterialfv GlShimMaterialfv
#define glColorMaterial GlShimColorMaterial
#define glTexGeni GlShimTexGeni
#define glTexGenfv GlShimTexGenfv
#define glFogf GlShimFogf
#define glFogi GlShimFogi
#define glFogfv GlShimFogfv
#define glAlphaFunc GlShimAlphaFunc
#define glClipPlane GlShimClipPlane
#define glColor4fv GlShimColor4fv
#define glMatrixMode GlShimMatrixMode
#define glLoadMatrixf GlShimLoadMatrixf
#define glLoadIdentity GlShimLoadIdentity
#define glEnableClientState GlShimEnableClientState
#define glDisableClientState GlShimDisableClientState
#define glVertexPointer GlShimVertexPointer
#define glNormalPointer GlShimNormalPointer
#define glColorPointer GlShimColorPointer
#define glTexCoordPointer GlShimTexCoordPointer
#define glDrawElements GlShimDrawElements
#define glDrawArrays GlShimDrawArrays
#define glBindTexture GlShimBindTexture
#define glDeleteTextures GlShimDeleteTextures
#define glTexImage2D GlShimTexImage2D
#define glTexSubImage2D GlShimTexSubImage2D
#define glCopyTexImage2D GlShimCopyTexImage2D
#define glReadPixels GlShimReadPixels
#define glPixelStorei GlShimPixelStorei
#define glPolygonMode GlShimPolygonMode
#define glPointSize GlShimPointSize
#define glDepthRange GlShimDepthRange
#define glFinish GlShimFinish
#define glHint GlShimHint
#define glGetIntegerv GlShimGetIntegerv
#define glGetFloatv GlShimGetFloatv

#endif
