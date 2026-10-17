#if defined(WOWDECOMP_PLATFORM_WEB)

// GL 1.x fixed-function emulation over WebGL2 for CGxDeviceOpenGl (see GlShim.h).
//
// Only the subset the device uses is implemented: texture env (incl. ARB_texture_env_combine), texgen, four lights
// with material/color material, fog, alpha test, user clip planes, point size/sprites, client vertex arrays sourced
// from VBOs, and BGRA/packed texture uploads. Each draw builds a key from the tracked state and uses a GLSL ES 3.00
// program generated for that key.
//
// ARB_vertex_program / ARB_fragment_program are emulated too: program text is translated by ArbProgram.cpp, and a
// draw links the bound program(s) with the fixed-function generator's code for whichever stage is not programmable
// (the stages share the vColor / vSpec / vFog / vTex* varyings). Linked programs are cached per (vertex program,
// fragment program, fixed-function state the pairing still reads); a pairing that fails to compile falls back to
// fixed function for the broken stage.
//
// Texture rectangles stay unsupported (GL_ARB_texture_rectangle = 0): the device then gives FFX power-of-two 2D
// render targets and the shaders' 2D variants, as on GL drivers without rectangles. RECT sampling in a program is
// still translated, as a 2D lookup with coordinates divided by the texture size.

#include "Engine/Source/Gx/CGxDeviceOpenGL/GlExt.h"
#include "Engine/Source/Gx/CGxDeviceOpenGL/Web/ArbProgram.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <emscripten/html5.h>

// Inside this file the gl* names are the real WebGL2 entry points.
#undef glEnable
#undef glDisable
#undef glTexEnvi
#undef glTexEnvf
#undef glTexEnvfv
#undef glTexEnviv
#undef glTexParameteri
#undef glLightf
#undef glLightfv
#undef glLightModelfv
#undef glLightModeli
#undef glMaterialf
#undef glMaterialfv
#undef glColorMaterial
#undef glTexGeni
#undef glTexGenfv
#undef glFogf
#undef glFogi
#undef glFogfv
#undef glAlphaFunc
#undef glClipPlane
#undef glColor4fv
#undef glMatrixMode
#undef glLoadMatrixf
#undef glLoadIdentity
#undef glEnableClientState
#undef glDisableClientState
#undef glVertexPointer
#undef glNormalPointer
#undef glColorPointer
#undef glTexCoordPointer
#undef glDrawElements
#undef glDrawArrays
#undef glBindTexture
#undef glDeleteTextures
#undef glTexImage2D
#undef glTexSubImage2D
#undef glCopyTexImage2D
#undef glReadPixels
#undef glPixelStorei
#undef glPolygonMode
#undef glPointSize
#undef glDepthRange
#undef glFinish
#undef glHint
#undef glGetIntegerv
#undef glGetFloatv

// WebGL2 / GLES3 entry points not declared by the GL 1.3 header.
extern "C" {
typedef char GLchar;
void glGenBuffers(GLsizei n, GLuint* buffers);
void glBindBuffer(GLenum target, GLuint buffer);
void glBufferData(GLenum target, GLsizeiptrARB size, const void* data, GLenum usage);
void glBufferSubData(GLenum target, GLintptrARB offset, GLsizeiptrARB size, const void* data);
void glDeleteBuffers(GLsizei n, const GLuint* buffers);
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer);
void glVertexAttrib4fv(GLuint index, const GLfloat* v);
void glEnableVertexAttribArray(GLuint index);
void glDisableVertexAttribArray(GLuint index);
GLuint glCreateShader(GLenum type);
void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
void glCompileShader(GLuint shader);
void glGetShaderiv(GLuint shader, GLenum pname, GLint* params);
void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
void glDeleteShader(GLuint shader);
GLuint glCreateProgram();
void glAttachShader(GLuint program, GLuint shader);
void glBindAttribLocation(GLuint program, GLuint index, const GLchar* name);
void glLinkProgram(GLuint program);
void glGetProgramiv(GLuint program, GLenum pname, GLint* params);
void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
void glUseProgram(GLuint program);
GLint glGetUniformLocation(GLuint program, const GLchar* name);
void glUniform1i(GLint location, GLint v0);
void glUniform1f(GLint location, GLfloat v0);
void glUniform2fv(GLint location, GLsizei count, const GLfloat* value);
void glUniform3fv(GLint location, GLsizei count, const GLfloat* value);
void glUniform4fv(GLint location, GLsizei count, const GLfloat* value);
void glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
void glGenFramebuffers(GLsizei n, GLuint* framebuffers);
void glBindFramebuffer(GLenum target, GLuint framebuffer);
void glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers);
GLenum glCheckFramebufferStatus(GLenum target);
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
void glGenRenderbuffers(GLsizei n, GLuint* renderbuffers);
void glBindRenderbuffer(GLenum target, GLuint renderbuffer);
void glDeleteRenderbuffers(GLsizei n, const GLuint* renderbuffers);
void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
void glGenerateMipmap(GLenum target);
void glGenQueries(GLsizei n, GLuint* ids);
void glDeleteQueries(GLsizei n, const GLuint* ids);
void glBeginQuery(GLenum target, GLuint id);
void glEndQuery(GLenum target);
void glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params);
void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices);
void glBlendColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glDepthRangef(GLfloat n, GLfloat f);
void glActiveTexture(GLenum texture);
void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void* data);
void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void* data);
}

#define SHIM_VERTEX_SHADER 0x8B31
#define SHIM_FRAGMENT_SHADER 0x8B30
#define SHIM_COMPILE_STATUS 0x8B81
#define SHIM_LINK_STATUS 0x8B82
#define SHIM_ANY_SAMPLES_PASSED_CONSERVATIVE 0x8D6A
#define SHIM_RGBA8 0x8058
#define SHIM_RGB8 0x8051
#define SHIM_RGBA4 0x8056
#define SHIM_RGB5_A1 0x8057
#define SHIM_RGB565 0x8D62
#define SHIM_RGB16F 0x881B
#define SHIM_DEPTH_COMPONENT32F 0x8CAC
#define SHIM_HALF_FLOAT 0x140B
#define SHIM_UNSIGNED_SHORT_4_4_4_4 0x8033
#define SHIM_UNSIGNED_SHORT_5_5_5_1 0x8034
#define SHIM_UNSIGNED_SHORT_5_6_5 0x8363
#define SHIM_UNSIGNED_SHORT_4_4_4_4_REV 0x8365
#define SHIM_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#define SHIM_ALIASED_POINT_SIZE_RANGE 0x846D
#define SHIM_TEXTURE_CUBE_MAP 0x8513
#define SHIM_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define SHIM_MAX_VERTEX_UNIFORM_VECTORS 0x8DFB
#define SHIM_MAX_FRAGMENT_UNIFORM_VECTORS 0x8DFD

// ---- GlExt.h globals ---------------------------------------------------------------------------------------------

int32_t glExtGPUProgramParameters;
PFNGLPROGRAMLOCALPARAMETERS4FVEXTPROC glProgramLocalParameters4fvEXT;
PFNGLPROGRAMENVPARAMETERS4FVEXTPROC glProgramEnvParameters4fvEXT;
int32_t wglExtSwapControl;
int32_t wglArbPbuffer;
int32_t wglArbPixelFormat;
int32_t glATIEnvmapBumpmap;
int32_t glATIFragmentShader;
int32_t glNVMultisampleFilterHint;
int32_t glNVTextureShader3;
int32_t glNVTextureShader2;
int32_t glNVTextureShader;
int32_t glNVRegisterCombiners2;
int32_t glNVRegisterCombiners;
int32_t glUnkC60498;
int32_t glSGISGenerateMipmap;
int32_t glExtBlendColor;
int32_t glExtFramebufferMultisample;
int32_t glExtFramebufferObject;
int32_t glExtDrawRangeElements;
int32_t glExtTextureFilterAnisotropic;
int32_t glExtTextureCompressionS3tc;
int32_t glExtTextureCompression;
int32_t glExtTextureLodBias;
int32_t glExtClampToEdge;
int32_t glExtBgra;
int32_t glARBPointSprite;
int32_t glARBPointParameters;
int32_t glARBOcclusionQuery;
int32_t glARBMultisample;
int32_t glARBTextureCubeMap;
int32_t glARBTextureRectangle;
int32_t glARBTextureEnvCombine;
int32_t glARBVertexBufferObject;
int32_t glARBFragmentProgram;
int32_t glARBVertexProgram;
PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT;
PFNWGLQUERYPBUFFERARBPROC wglQueryPbufferARB;
PFNWGLDESTROYPBUFFERARBPROC wglDestroyPbufferARB;
PFNWGLRELEASEPBUFFERDCARBPROC wglReleasePbufferDCARB;
PFNWGLGETPBUFFERDCARBPROC wglGetPbufferDCARB;
PFNWGLCREATEPBUFFERARBPROC wglCreatePbufferARB;
PFNWGLCHOOSEPIXELFORMATARBPROC wglChoosePixelFormatARB;
PFNWGLGETPIXELFORMATATTRIBIVARBPROC wglGetPixelFormatAttribivARB;
PFNWGLGETEXTENSIONSSTRINGARBPROC wglGetExtensionsStringARB;
PFNGLBLENDCOLOREXTPROC glBlendColorEXT;
PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC glFramebufferRenderbufferEXT;
PFNGLFRAMEBUFFERTEXTURE3DEXTPROC glFramebufferTexture3DEXT;
PFNGLFRAMEBUFFERTEXTURE2DEXTPROC glFramebufferTexture2DEXT;
PFNGLFRAMEBUFFERTEXTURE1DEXTPROC glFramebufferTexture1DEXT;
PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC glCheckFramebufferStatusEXT;
PFNGLGENFRAMEBUFFERSEXTPROC glGenFramebuffersEXT;
PFNGLDELETEFRAMEBUFFERSEXTPROC glDeleteFramebuffersEXT;
PFNGLBINDFRAMEBUFFEREXTPROC glBindFramebufferEXT;
PFNGLRENDERBUFFERSTORAGEEXTPROC glRenderbufferStorageEXT;
PFNGLGENRENDERBUFFERSEXTPROC glGenRenderbuffersEXT;
PFNGLDELETERENDERBUFFERSEXTPROC glDeleteRenderbuffersEXT;
PFNGLBINDRENDERBUFFEREXTPROC glBindRenderbufferEXT;
PFNGLPOINTPARAMETERFVARBPROC glPointParameterfvARB;
PFNGLPOINTPARAMETERFARBPROC glPointParameterfARB;
PFNGLGETQUERYOBJECTUIVARBPROC glGetQueryObjectuivARB;
PFNGLGETQUERYOBJECTIVARBPROC glGetQueryObjectivARB;
PFNGLGETQUERYIVARBPROC glGetQueryivARB;
PFNGLENDQUERYARBPROC glEndQueryARB;
PFNGLBEGINQUERYARBPROC glBeginQueryARB;
PFNGLDELETEQUERIESARBPROC glDeleteQueriesARB;
PFNGLGENQUERIESARBPROC glGenQueriesARB;
PFNGLUNMAPBUFFERARBPROC glUnmapBufferARB;
PFNGLMAPBUFFERARBPROC glMapBufferARB;
PFNGLBUFFERSUBDATAARBPROC glBufferSubDataARB;
PFNGLBUFFERDATAARBPROC glBufferDataARB;
PFNGLGENBUFFERSARBPROC glGenBuffersARB;
PFNGLDELETEBUFFERSARBPROC glDeleteBuffersARB;
PFNGLBINDBUFFERARBPROC glBindBufferARB;
PFNGLGENPROGRAMSNVPROC glGenProgramsNV;
PFNGLDELETEPROGRAMSNVPROC glDeleteProgramsNV;
PFNGLPROGRAMPARAMETERS4FVNVPROC glProgramParameters4fvNV;
PFNGLBINDPROGRAMNVPROC glBindProgramNV;
PFNGLLOADPROGRAMNVPROC glLoadProgramNV;
PFNGLDISABLEVERTEXATTRIBARRAYARBPROC glDisableVertexAttribArrayARB;
PFNGLENABLEVERTEXATTRIBARRAYARBPROC glEnableVertexAttribArrayARB;
PFNGLVERTEXATTRIBPOINTERARBPROC glVertexAttribPointerARB;
PFNGLGETPROGRAMIVARBPROC glGetProgramivARB;
PFNGLISPROGRAMARBPROC glIsProgramARB;
PFNGLPROGRAMLOCALPARAMETER4FVARBPROC glProgramLocalParameter4fvARB;
PFNGLPROGRAMENVPARAMETER4FVARBPROC glProgramEnvParameter4fvARB;
PFNGLGENPROGRAMSARBPROC glGenProgramsARB;
PFNGLDELETEPROGRAMSARBPROC glDeleteProgramsARB;
PFNGLBINDPROGRAMARBPROC glBindProgramARB;
PFNGLPROGRAMSTRINGARBPROC glProgramStringARB;
PFNGLCOMBINERPARAMETERINVPROC glCombinerParameteriNV;
PFNGLCOMBINERPARAMETERFVNVPROC glCombinerParameterfvNV;
PFNGLFINALCOMBINERINPUTNVPROC glFinalCombinerInputNV;
PFNGLCOMBINERSTAGEPARAMETERFVNVPROC glCombinerStageParameterfvNV;
PFNGLCOMBINEROUTPUTNVPROC glCombinerOutputNV;
PFNGLCOMBINERINPUTNVPROC glCombinerInputNV;
PFNGLDRAWRANGEELEMENTSEXTPROC glDrawRangeElementsEXT;
PFNGLCOMPRESSEDTEXSUBIMAGE2DARBPROC glCompressedTexSubImage2DARB;
PFNGLCOMPRESSEDTEXIMAGE2DARBPROC glCompressedTexImage2DARB;
PFNGLCLIENTACTIVETEXTUREARBPROC glClientActiveTextureARB;
PFNGLACTIVETEXTUREARBPROC glActiveTextureARB;
PFNGLUNLOCKARRAYSEXTPROC glUnlockArraysEXT;
PFNGLLOCKARRAYSEXTPROC glLockArraysEXT;
GLint glMaxTextureImageUnitsARB;
GLint glMaxTextureUnits;
int32_t glExtCVA;
uint32_t glVersion;

// The context is GL-only on web; nothing to bind or unbind beyond GlShimInitialize.
void BindGlExtensions() {
}

void BindWglExtensions() {
}

bool FindGlExt(const char* name) {
    return false;
}

bool HasWGLExtension(const char* name) {
    return false;
}

void UnbindGlExtensions() {
}

// ---- tracked fixed-function state --------------------------------------------------------------------------------

namespace {

// Set to 1 to log GL errors after each draw (glGetError stalls the pipeline, so off by default).
#define GLSHIM_CHECK_ERRORS 0

const int kUnits = 8;
const int kLights = 4;
const int kAttribTex0 = 3;

struct Mat4 {
    float m[16];
};

const Mat4 kIdentity = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };

struct ArrayState {
    bool m_enabled;
    bool m_normalized; // generic attributes only
    GLint m_size;
    GLenum m_type;
    GLsizei m_stride;
    const GLvoid* m_pointer;
    GLuint m_buffer;
};

struct UnitState {
    bool m_tex2d;
    bool m_texCube;
    bool m_genEnabled[4];
    GLenum m_genMode[4];
    float m_eyePlane[4][4];
    float m_objPlane[4][4];
    GLenum m_envMode;
    GLenum m_combineRgb;
    GLenum m_combineAlpha;
    GLenum m_sourceRgb[3];
    GLenum m_sourceAlpha[3];
    GLenum m_operandRgb[3];
    GLenum m_operandAlpha[3];
    float m_scaleRgb;
    float m_scaleAlpha;
    float m_envColor[4];
    Mat4 m_matrix;
    ArrayState m_texCoordArray;
    GLuint m_bound2d;
    GLuint m_boundCube;
};

struct LightState {
    bool m_enabled;
    float m_position[4];
    float m_ambient[4];
    float m_diffuse[4];
    float m_specular[4];
    float m_attenuation[3];
};

struct State {
    uint32_t m_version;
    GLenum m_matrixMode;
    Mat4 m_modelView;
    Mat4 m_projection;
    uint32_t m_activeUnit;
    uint32_t m_clientUnit;
    UnitState m_units[kUnits];
    bool m_lighting;
    LightState m_lights[kLights];
    float m_sceneAmbient[4];
    bool m_localViewer;
    bool m_separateSpecular;
    bool m_colorMaterialEnabled;
    GLenum m_colorMaterialMode;
    float m_matAmbient[4];
    float m_matDiffuse[4];
    float m_matSpecular[4];
    float m_matEmission[4];
    float m_matShininess;
    bool m_normalize;
    bool m_fog;
    GLenum m_fogMode;
    float m_fogStart;
    float m_fogEnd;
    float m_fogDensity;
    float m_fogColor[4];
    bool m_alphaTest;
    GLenum m_alphaFunc;
    float m_alphaRef;
    uint32_t m_clipMask;
    float m_clipPlanes[6][4];
    float m_color[4];
    float m_pointSize;
    float m_pointMin;
    float m_pointMax;
    float m_pointAttenuation[3];
    bool m_pointSprite;
    ArrayState m_vertexArray;
    ArrayState m_normalArray;
    ArrayState m_colorArray;
    ArrayState m_attribArrays[16]; // ARB_vertex_program generic attributes
    bool m_vertexProgram;          // GL_VERTEX_PROGRAM_ARB enabled
    bool m_fragmentProgram;        // GL_FRAGMENT_PROGRAM_ARB enabled
    GLuint m_boundVertexProgram;
    GLuint m_boundFragmentProgram;
    GLuint m_arrayBuffer;
    GLuint m_framebuffer;
    GLint m_unpackRowLength;
};

State s_state;
bool s_hasAnisotropy;
std::unordered_map<GLuint, bool> s_generateMipmap;

struct TexInfo {
    GLsizei m_width;
    GLsizei m_height;
    GLenum m_format;
};

std::unordered_map<GLuint, TexInfo> s_texInfo;
bool s_dump;
int s_dumpIndex;
uint32_t s_enabledAttribs;

// ---- ARB program objects -----------------------------------------------------------------------------------------

const int kArbParams = 256;

struct ArbObject {
    uint32_t m_serial = 0;       // new for every glProgramStringARB; part of the linked-program key
    ArbProgram m_program;
    std::vector<float> m_locals; // program.local, 4 floats each
    bool m_failed = false;       // GLSL compile failed: the stage falls back to fixed function
};

std::unordered_map<GLuint, ArbObject> s_arbObjects;
GLuint s_arbNextId = 1;
uint32_t s_arbSerial;
uint32_t s_arbStamp = 1; // bumped by every env / local parameter write
float s_arbEnv[2][kArbParams * 4];
GLint s_arbErrorPosition = -1;
GLint s_arbMaxParams[2] = { kArbParams, kArbParams };

int ArbStage(GLenum target) {
    return target == GL_FRAGMENT_PROGRAM_ARB ? 1 : 0;
}

GLuint& ArbBound(int stage) {
    return stage ? s_state.m_boundFragmentProgram : s_state.m_boundVertexProgram;
}

// The program a stage draws with, or null for fixed-function processing.
ArbObject* ActiveArb(int stage) {
    if (!(stage ? s_state.m_fragmentProgram : s_state.m_vertexProgram)) {
        return nullptr;
    }

    auto it = s_arbObjects.find(ArbBound(stage));

    if (it == s_arbObjects.end() || !it->second.m_program.m_valid || it->second.m_failed || it->second.m_program.m_vertex != (stage == 0)) {
        return nullptr;
    }

    return &it->second;
}

void Dirty() {
    s_state.m_version++;
}

void Copy4(float* out, const float* in) {
    memcpy(out, in, sizeof(float) * 4);
}

void Set4(float* out, float a, float b, float c, float d) {
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
}

// Column-major, like GL.
Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 r;

    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;

            for (int k = 0; k < 4; k++) {
                sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            }

            r.m[col * 4 + row] = sum;
        }
    }

    return r;
}

bool Invert(const Mat4& in, Mat4& out) {
    const float* m = in.m;
    float inv[16];

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

    if (det == 0.0f) {
        out = kIdentity;
        return false;
    }

    for (int i = 0; i < 16; i++) {
        out.m[i] = inv[i] / det;
    }

    return true;
}

void TransformPoint(const Mat4& m, const float* in, float* out) {
    float r[4];

    for (int row = 0; row < 4; row++) {
        r[row] = m.m[row] * in[0] + m.m[4 + row] * in[1] + m.m[8 + row] * in[2] + m.m[12 + row] * in[3];
    }

    Copy4(out, r);
}

// Planes are specified in object space and stored in eye space: p' = p * inverse(modelview).
void TransformPlane(const float* in, float* out) {
    Mat4 inverse;
    Invert(s_state.m_modelView, inverse);
    float r[4];

    for (int col = 0; col < 4; col++) {
        r[col] = in[0] * inverse.m[col * 4 + 0] + in[1] * inverse.m[col * 4 + 1] + in[2] * inverse.m[col * 4 + 2] + in[3] * inverse.m[col * 4 + 3];
    }

    Copy4(out, r);
}

void ResetState() {
    State& s = s_state;
    memset(&s, 0, sizeof(s));
    s.m_matrixMode = GL_MODELVIEW;
    s.m_modelView = kIdentity;
    s.m_projection = kIdentity;

    for (int i = 0; i < kUnits; i++) {
        UnitState& u = s.m_units[i];
        u.m_genMode[0] = u.m_genMode[1] = u.m_genMode[2] = u.m_genMode[3] = GL_EYE_LINEAR;
        Set4(u.m_eyePlane[0], 1, 0, 0, 0);
        Set4(u.m_eyePlane[1], 0, 1, 0, 0);
        Set4(u.m_objPlane[0], 1, 0, 0, 0);
        Set4(u.m_objPlane[1], 0, 1, 0, 0);
        u.m_envMode = GL_MODULATE;
        u.m_combineRgb = GL_MODULATE;
        u.m_combineAlpha = GL_MODULATE;
        u.m_sourceRgb[0] = u.m_sourceAlpha[0] = GL_TEXTURE;
        u.m_sourceRgb[1] = u.m_sourceAlpha[1] = GL_PREVIOUS;
        u.m_sourceRgb[2] = u.m_sourceAlpha[2] = GL_CONSTANT;
        u.m_operandRgb[0] = u.m_operandRgb[1] = GL_SRC_COLOR;
        u.m_operandRgb[2] = GL_SRC_ALPHA;
        u.m_operandAlpha[0] = u.m_operandAlpha[1] = u.m_operandAlpha[2] = GL_SRC_ALPHA;
        u.m_scaleRgb = u.m_scaleAlpha = 1.0f;
        u.m_matrix = kIdentity;
        u.m_texCoordArray.m_size = 4;
        u.m_texCoordArray.m_type = GL_FLOAT;
    }

    for (int i = 0; i < kLights; i++) {
        LightState& l = s.m_lights[i];
        Set4(l.m_position, 0, 0, 1, 0);
        Set4(l.m_ambient, 0, 0, 0, 1);
        Set4(l.m_diffuse, i == 0, i == 0, i == 0, 1);
        Set4(l.m_specular, i == 0, i == 0, i == 0, 1);
        l.m_attenuation[0] = 1.0f;
    }

    Set4(s.m_sceneAmbient, 0.2f, 0.2f, 0.2f, 1.0f);
    s.m_colorMaterialMode = GL_AMBIENT_AND_DIFFUSE;
    Set4(s.m_matAmbient, 0.2f, 0.2f, 0.2f, 1.0f);
    Set4(s.m_matDiffuse, 0.8f, 0.8f, 0.8f, 1.0f);
    Set4(s.m_matSpecular, 0, 0, 0, 1);
    Set4(s.m_matEmission, 0, 0, 0, 1);
    s.m_fogMode = GL_EXP;
    s.m_fogEnd = 1.0f;
    s.m_fogDensity = 1.0f;
    s.m_alphaFunc = GL_ALWAYS;
    Set4(s.m_color, 1, 1, 1, 1);
    s.m_pointSize = 1.0f;
    s.m_pointMax = 64.0f;
    s.m_pointAttenuation[0] = 1.0f;
    s.m_vertexArray.m_size = 4;
    s.m_vertexArray.m_type = GL_FLOAT;
    s.m_normalArray.m_size = 3;
    s.m_normalArray.m_type = GL_FLOAT;
    s.m_colorArray.m_size = 4;
    s.m_colorArray.m_type = GL_FLOAT;
    s.m_version = 1;
}

UnitState& ActiveUnit() {
    return s_state.m_units[s_state.m_activeUnit];
}

// ---- program generation ------------------------------------------------------------------------------------------

struct UnitKey {
    uint8_t m_target;      // 0 off, 1 2D, 2 cube
    uint8_t m_gen[4];      // 0 off, 1 object, 2 eye, 3 sphere, 4 reflection, 5 normal
    uint8_t m_envMode;     // 0 modulate, 1 replace, 2 decal, 3 add, 4 blend, 5 combine
    uint8_t m_combine[2];  // rgb, alpha: 0 replace, 1 modulate, 2 add, 3 add signed, 4 interpolate, 5 subtract, 6 dot3 rgb, 7 dot3 rgba
    uint8_t m_source[2][3];  // 0 texture, 1 constant, 2 primary, 3 previous
    uint8_t m_operand[2][3]; // 0 color, 1 1-color, 2 alpha, 3 1-alpha
    uint8_t m_scale[2];      // 1, 2, 4
};

struct ProgramKey {
    uint8_t m_lighting;
    uint8_t m_lightMask;
    uint8_t m_lightPoint;
    uint8_t m_localViewer;
    uint8_t m_separateSpecular;
    uint8_t m_colorMaterial; // 0 off, 1 ambient+diffuse, 2 ambient, 3 diffuse, 4 specular, 5 emission
    uint8_t m_normalize;
    uint8_t m_fog;           // 0 off, 1 linear, 2 exp, 3 exp2
    uint8_t m_alphaFunc;     // 0 off, else GL func - GL_NEVER + 1
    uint8_t m_clipMask;
    uint8_t m_pointSprite;
    uint8_t m_texcoordMask;  // units whose texcoords a fragment program reads (with or without an enabled texture)
    UnitKey m_units[kUnits];
};

// Linked-program cache key: the ARB programs (by serial) plus the fixed-function state the pairing still uses.
struct ComboKey {
    uint32_t m_vertexProgram;
    uint32_t m_fragmentProgram;
    ProgramKey m_ff;
};

bool UnitTexcoords(const ProgramKey& key, int unit) {
    return key.m_units[unit].m_target || (key.m_texcoordMask & (1u << unit));
}

struct Program {
    GLuint m_program;
    uint32_t m_version;
    GLint m_mv, m_p, m_nm;
    GLint m_matAmb, m_matDiff, m_matSpec, m_matEmis, m_matShin, m_sceneAmb;
    GLint m_lightPos, m_lightAmb, m_lightDiff, m_lightSpec, m_lightAtt;
    GLint m_texMat, m_eyePlane, m_objPlane, m_envColor;
    GLint m_clip, m_pointSize, m_pointAtt, m_pointClamp;
    GLint m_fogColor, m_fogParams, m_alphaRef;
    GLint m_vc, m_fc;      // ARB program slot arrays
    uint32_t m_arbStamp;   // s_arbStamp at the last slot upload
    uint32_t m_arbVersion; // state version at the last slot upload
    uint8_t m_failed;      // 1 vertex shader, 2 fragment shader, 3 link failed
};

std::unordered_map<std::string, Program> s_programs;
const Program* s_currentProgram;

uint8_t SourceIndex(GLenum source) {
    switch (source) {
        case GL_CONSTANT:
            return 1;
        case GL_PRIMARY_COLOR:
            return 2;
        case GL_PREVIOUS:
            return 3;
        default:
            return 0;
    }
}

uint8_t OperandIndex(GLenum operand) {
    switch (operand) {
        case GL_ONE_MINUS_SRC_COLOR:
            return 1;
        case GL_SRC_ALPHA:
            return 2;
        case GL_ONE_MINUS_SRC_ALPHA:
            return 3;
        default:
            return 0;
    }
}

uint8_t CombineIndex(GLenum combine) {
    switch (combine) {
        case GL_REPLACE:
            return 0;
        case GL_ADD:
            return 2;
        case GL_ADD_SIGNED:
            return 3;
        case GL_INTERPOLATE:
            return 4;
        case GL_SUBTRACT:
            return 5;
        case GL_DOT3_RGB:
            return 6;
        case GL_DOT3_RGBA:
            return 7;
        default:
            return 1;
    }
}

uint8_t GenIndex(GLenum mode) {
    switch (mode) {
        case GL_OBJECT_LINEAR:
            return 1;
        case GL_EYE_LINEAR:
            return 2;
        case GL_SPHERE_MAP:
            return 3;
        case GL_REFLECTION_MAP_ARB:
            return 4;
        case GL_NORMAL_MAP_ARB:
            return 5;
        default:
            return 0;
    }
}

uint8_t ScaleIndex(float scale) {
    return scale >= 4.0f ? 4 : (scale >= 2.0f ? 2 : 1);
}

ProgramKey BuildKey(uint32_t texcoordMask) {
    const State& s = s_state;
    ProgramKey key;
    memset(&key, 0, sizeof(key));
    key.m_texcoordMask = static_cast<uint8_t>(texcoordMask);

    key.m_lighting = s.m_lighting;

    if (s.m_lighting) {
        for (int i = 0; i < kLights; i++) {
            if (s.m_lights[i].m_enabled) {
                key.m_lightMask |= 1 << i;

                if (s.m_lights[i].m_position[3] != 0.0f) {
                    key.m_lightPoint |= 1 << i;
                }
            }
        }

        key.m_localViewer = s.m_localViewer;
        key.m_separateSpecular = s.m_separateSpecular;

        if (s.m_colorMaterialEnabled) {
            switch (s.m_colorMaterialMode) {
                case GL_AMBIENT:
                    key.m_colorMaterial = 2;
                    break;
                case GL_DIFFUSE:
                    key.m_colorMaterial = 3;
                    break;
                case GL_SPECULAR:
                    key.m_colorMaterial = 4;
                    break;
                case GL_EMISSION:
                    key.m_colorMaterial = 5;
                    break;
                default:
                    key.m_colorMaterial = 1;
                    break;
            }
        }

        key.m_normalize = s.m_normalize;
    }

    if (s.m_fog) {
        key.m_fog = s.m_fogMode == GL_LINEAR ? 1 : (s.m_fogMode == GL_EXP2 ? 3 : 2);
    }

    if (s.m_alphaTest && s.m_alphaFunc != GL_ALWAYS) {
        key.m_alphaFunc = static_cast<uint8_t>(s.m_alphaFunc - GL_NEVER + 1);
    }

    key.m_clipMask = static_cast<uint8_t>(s.m_clipMask);
    key.m_pointSprite = s.m_pointSprite;

    for (int i = 0; i < kUnits; i++) {
        const UnitState& u = s.m_units[i];
        UnitKey& k = key.m_units[i];
        k.m_target = u.m_texCube ? 2 : (u.m_tex2d ? 1 : 0);

        if (!UnitTexcoords(key, i)) {
            continue;
        }

        for (int c = 0; c < 4; c++) {
            k.m_gen[c] = u.m_genEnabled[c] ? GenIndex(u.m_genMode[c]) : 0;
        }

        if (!k.m_target) {
            continue;
        }

        switch (u.m_envMode) {
            case GL_REPLACE:
                k.m_envMode = 1;
                break;
            case GL_DECAL:
                k.m_envMode = 2;
                break;
            case GL_ADD:
                k.m_envMode = 3;
                break;
            case GL_BLEND:
                k.m_envMode = 4;
                break;
            case GL_COMBINE:
                k.m_envMode = 5;
                break;
            default:
                k.m_envMode = 0;
                break;
        }

        if (k.m_envMode == 5) {
            k.m_combine[0] = CombineIndex(u.m_combineRgb);
            k.m_combine[1] = CombineIndex(u.m_combineAlpha);

            for (int a = 0; a < 3; a++) {
                k.m_source[0][a] = SourceIndex(u.m_sourceRgb[a]);
                k.m_source[1][a] = SourceIndex(u.m_sourceAlpha[a]);
                k.m_operand[0][a] = OperandIndex(u.m_operandRgb[a]);
                k.m_operand[1][a] = OperandIndex(u.m_operandAlpha[a]);
            }

            k.m_scale[0] = ScaleIndex(u.m_scaleRgb);
            k.m_scale[1] = ScaleIndex(u.m_scaleAlpha);
        }
    }

    return key;
}

std::string Str(int i) {
    return std::to_string(i);
}

std::string VertexSource(const ProgramKey& key) {
    std::string s = "#version 300 es\nprecision highp float;\n";
    s += "layout(location = 0) in vec4 aPos;\nlayout(location = 1) in vec3 aNormal;\nlayout(location = 2) in vec4 aColor;\n";

    for (int i = 0; i < kUnits; i++) {
        if (UnitTexcoords(key, i)) {
            s += "layout(location = " + Str(kAttribTex0 + i) + ") in vec4 aTex" + Str(i) + ";\nout vec4 vTex" + Str(i) + ";\n";
        }
    }

    s += "uniform mat4 uMV;\nuniform mat4 uP;\nuniform mat3 uNM;\n";
    s += "uniform vec4 uMatAmb, uMatDiff, uMatSpec, uMatEmis, uSceneAmb;\nuniform float uMatShin;\n";
    s += "uniform vec4 uLightPos[4], uLightAmb[4], uLightDiff[4], uLightSpec[4];\nuniform vec3 uLightAtt[4];\n";
    s += "uniform mat4 uTexMat[8];\nuniform vec4 uEyePlane[32], uObjPlane[32];\nuniform vec4 uClip[6];\n";
    s += "uniform float uPointSize;\nuniform vec3 uPointAtt;\nuniform vec2 uPointClamp;\n";
    s += "out vec4 vColor;\nout vec4 vSpec;\nout float vFog;\n";

    for (int i = 0; i < 6; i++) {
        if (key.m_clipMask & (1 << i)) {
            s += "out float vClip" + Str(i) + ";\n";
        }
    }

    s += "void main() {\n";
    s += "    vec4 eye = uMV * aPos;\n    gl_Position = uP * eye;\n";
    s += "    vec3 n = uNM * aNormal;\n";

    if (key.m_normalize) {
        s += "    n = normalize(n);\n";
    }

    s += "    vSpec = vec4(0.0);\n";

    if (key.m_lighting) {
        const char* ma = key.m_colorMaterial == 1 || key.m_colorMaterial == 2 ? "aColor" : "uMatAmb";
        const char* md = key.m_colorMaterial == 1 || key.m_colorMaterial == 3 ? "aColor" : "uMatDiff";
        const char* ms = key.m_colorMaterial == 4 ? "aColor" : "uMatSpec";
        const char* me = key.m_colorMaterial == 5 ? "aColor" : "uMatEmis";
        s += std::string("    vec4 ma = ") + ma + ", md = " + md + ", ms = " + ms + ", me = " + me + ";\n";
        s += "    vec3 c = me.rgb + ma.rgb * uSceneAmb.rgb;\n    vec3 sp = vec3(0.0);\n";
        s += key.m_localViewer ? "    vec3 V = normalize(-eye.xyz);\n" : "    vec3 V = vec3(0.0, 0.0, 1.0);\n";

        for (int i = 0; i < kLights; i++) {
            if (!(key.m_lightMask & (1 << i))) {
                continue;
            }

            std::string l = Str(i);
            s += "    {\n";

            if (key.m_lightPoint & (1 << i)) {
                s += "        vec3 d = uLightPos[" + l + "].xyz - eye.xyz;\n        float dist = length(d);\n        vec3 L = d / max(dist, 1e-6);\n";
                s += "        float att = 1.0 / (uLightAtt[" + l + "].x + uLightAtt[" + l + "].y * dist + uLightAtt[" + l + "].z * dist * dist);\n";
            } else {
                s += "        vec3 L = normalize(uLightPos[" + l + "].xyz);\n        float att = 1.0;\n";
            }

            s += "        float nl = max(dot(n, L), 0.0);\n";
            s += "        c += att * (ma.rgb * uLightAmb[" + l + "].rgb + nl * md.rgb * uLightDiff[" + l + "].rgb);\n";
            s += "        if (nl > 0.0) {\n            float nh = max(dot(n, normalize(L + V)), 0.0);\n";
            s += "            sp += att * (uMatShin > 0.0 ? pow(nh, uMatShin) : 1.0) * ms.rgb * uLightSpec[" + l + "].rgb;\n        }\n";
            s += "    }\n";
        }

        if (key.m_separateSpecular) {
            s += "    vColor = clamp(vec4(c, md.a), 0.0, 1.0);\n    vSpec = vec4(clamp(sp, 0.0, 1.0), 0.0);\n";
        } else {
            s += "    vColor = clamp(vec4(c + sp, md.a), 0.0, 1.0);\n";
        }
    } else {
        s += "    vColor = aColor;\n";
    }

    for (int i = 0; i < kUnits; i++) {
        const UnitKey& k = key.m_units[i];

        if (!UnitTexcoords(key, i)) {
            continue;
        }

        std::string u = Str(i);
        s += "    {\n        vec4 tc = aTex" + u + ";\n";

        bool sphere = k.m_gen[0] == 3 || k.m_gen[1] == 3;
        bool reflection = k.m_gen[0] == 4 || k.m_gen[1] == 4 || k.m_gen[2] == 4;

        if (sphere || reflection) {
            s += "        vec3 r = reflect(normalize(eye.xyz), n);\n";
        }

        if (sphere) {
            s += "        float sm = 2.0 * sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0) * (r.z + 1.0));\n";
        }

        static const char* kComp = "stpq";

        for (int c = 0; c < 4; c++) {
            std::string comp(1, kComp[c]);
            std::string plane = Str(i * 4 + c);

            switch (k.m_gen[c]) {
                case 1:
                    s += "        tc." + comp + " = dot(uObjPlane[" + plane + "], aPos);\n";
                    break;
                case 2:
                    s += "        tc." + comp + " = dot(uEyePlane[" + plane + "], eye);\n";
                    break;
                case 3:
                    if (c < 2) {
                        s += "        tc." + comp + " = r." + std::string(1, "xy"[c]) + " / sm + 0.5;\n";
                    }
                    break;
                case 4:
                    if (c < 3) {
                        s += "        tc." + comp + " = r." + std::string(1, "xyz"[c]) + ";\n";
                    }
                    break;
                case 5:
                    if (c < 3) {
                        s += "        tc." + comp + " = n." + std::string(1, "xyz"[c]) + ";\n";
                    }
                    break;
                default:
                    break;
            }
        }

        s += "        vTex" + u + " = uTexMat[" + u + "] * tc;\n    }\n";
    }

    s += "    vFog = abs(eye.z);\n";

    for (int i = 0; i < 6; i++) {
        if (key.m_clipMask & (1 << i)) {
            s += "    vClip" + Str(i) + " = dot(uClip[" + Str(i) + "], eye);\n";
        }
    }

    s += "    float pd = length(eye.xyz);\n";
    s += "    gl_PointSize = clamp(uPointSize * inversesqrt(max(uPointAtt.x + uPointAtt.y * pd + uPointAtt.z * pd * pd, 1e-6)), uPointClamp.x, uPointClamp.y);\n";
    s += "}\n";

    return s;
}

std::string CombineArg(const UnitKey& k, int channel, int arg, int unit) {
    static const char* kSources[4] = { "tex", "uEnvColor[U]", "primary", "prev" };
    std::string source = kSources[k.m_source[channel][arg]];
    size_t pos = source.find('U');

    if (pos != std::string::npos) {
        source.replace(pos, 1, Str(unit));
    }

    switch (k.m_operand[channel][arg]) {
        case 0:
            return channel ? source + ".a" : source + ".rgb";
        case 1:
            return channel ? "(1.0 - " + source + ".a)" : "(1.0 - " + source + ".rgb)";
        case 2:
            return channel ? source + ".a" : "vec3(" + source + ".a)";
        default:
            return channel ? "(1.0 - " + source + ".a)" : "vec3(1.0 - " + source + ".a)";
    }
}

std::string CombineExpr(const UnitKey& k, int channel, int unit) {
    std::string a0 = CombineArg(k, channel, 0, unit);
    std::string a1 = CombineArg(k, channel, 1, unit);
    std::string a2 = CombineArg(k, channel, 2, unit);
    std::string half = channel ? "0.5" : "vec3(0.5)";

    switch (k.m_combine[channel]) {
        case 0:
            return a0;
        case 2:
            return a0 + " + " + a1;
        case 3:
            return a0 + " + " + a1 + " - " + half;
        case 4:
            return a0 + " * " + a2 + " + " + a1 + " * (1.0 - " + a2 + ")";
        case 5:
            return a0 + " - " + a1;
        case 6:
        case 7:
            return channel ? "4.0 * dot(" + CombineArg(k, 0, 0, unit) + " - 0.5, " + CombineArg(k, 0, 1, unit) + " - 0.5)"
                           : "vec3(4.0 * dot(" + a0 + " - 0.5, " + a1 + " - 0.5))";
        default:
            return a0 + " * " + a1;
    }
}

std::string AlphaTest(const ProgramKey& key, const char* color) {
    if (!key.m_alphaFunc) {
        return "";
    }

    static const char* kOps[8] = { nullptr, "<", "==", "<=", ">", "!=", ">=", nullptr };
    int func = (key.m_alphaFunc - 1) & 7;
    std::string test = func == 0 ? "false" : (func == 7 ? "true" : std::string(color) + ".a " + kOps[func] + " uAlphaRef");
    return "    if (!(" + test + ")) discard;\n";
}

// Per-fragment fixed-function state that still applies after a fragment program: clip planes and the alpha test.
std::string ArbFragmentDecls(const ProgramKey& key) {
    std::string s = "uniform float uAlphaRef;\n";

    for (int i = 0; i < 6; i++) {
        if (key.m_clipMask & (1 << i)) {
            s += "in float vClip" + Str(i) + ";\n";
        }
    }

    return s;
}

std::string ArbFragmentEpilogue(const ProgramKey& key) {
    std::string s;

    for (int i = 0; i < 6; i++) {
        if (key.m_clipMask & (1 << i)) {
            s += "    if (vClip" + Str(i) + " < 0.0) discard;\n";
        }
    }

    return s + AlphaTest(key, "oColor");
}

std::string FragmentSource(const ProgramKey& key) {
    std::string s = "#version 300 es\nprecision highp float;\n";
    s += "in vec4 vColor;\nin vec4 vSpec;\nin float vFog;\n";

    for (int i = 0; i < kUnits; i++) {
        if (key.m_units[i].m_target) {
            s += "in vec4 vTex" + Str(i) + ";\n";
            s += std::string("uniform ") + (key.m_units[i].m_target == 2 ? "samplerCube" : "sampler2D") + " uSampler" + Str(i) + ";\n";
        }
    }

    for (int i = 0; i < 6; i++) {
        if (key.m_clipMask & (1 << i)) {
            s += "in float vClip" + Str(i) + ";\n";
        }
    }

    s += "uniform vec4 uEnvColor[8];\nuniform vec4 uFogColor;\nuniform vec3 uFogParams;\nuniform float uAlphaRef;\n";
    s += "out vec4 fragColor;\nvoid main() {\n";

    for (int i = 0; i < 6; i++) {
        if (key.m_clipMask & (1 << i)) {
            s += "    if (vClip" + Str(i) + " < 0.0) discard;\n";
        }
    }

    s += "    vec4 primary = vColor;\n    vec4 prev = primary;\n";

    for (int i = 0; i < kUnits; i++) {
        const UnitKey& k = key.m_units[i];

        if (!k.m_target) {
            continue;
        }

        std::string u = Str(i);
        s += "    {\n";

        if (k.m_target == 2) {
            s += "        vec4 tex = texture(uSampler" + u + ", vTex" + u + ".xyz);\n";
        } else if (key.m_pointSprite) {
            s += "        vec4 tex = texture(uSampler" + u + ", gl_PointCoord);\n";
        } else {
            s += "        vec4 tex = texture(uSampler" + u + ", vTex" + u + ".xy / vTex" + u + ".w);\n";
        }

        switch (k.m_envMode) {
            case 1:
                s += "        prev = tex;\n";
                break;
            case 2:
                s += "        prev = vec4(mix(prev.rgb, tex.rgb, tex.a), prev.a);\n";
                break;
            case 3:
                s += "        prev = vec4(clamp(prev.rgb + tex.rgb, 0.0, 1.0), prev.a * tex.a);\n";
                break;
            case 4:
                s += "        prev = vec4(mix(prev.rgb, uEnvColor[" + u + "].rgb, tex.rgb), prev.a * tex.a);\n";
                break;
            case 5: {
                std::string rgb = "(" + CombineExpr(k, 0, i) + ") * " + Str(k.m_scale[0]) + ".0";
                std::string alpha = "(" + CombineExpr(k, 1, i) + ") * " + Str(k.m_scale[1]) + ".0";

                if (k.m_combine[0] == 7) {
                    s += "        prev = clamp(vec4(" + rgb + ", (" + CombineExpr(k, 0, i) + ").r * " + Str(k.m_scale[0]) + ".0), 0.0, 1.0);\n";
                } else {
                    s += "        prev = clamp(vec4(" + rgb + ", " + alpha + "), 0.0, 1.0);\n";
                }

                break;
            }
            default:
                s += "        prev = prev * tex;\n";
                break;
        }

        s += "    }\n";
    }

    if (key.m_lighting && key.m_separateSpecular) {
        s += "    prev.rgb = clamp(prev.rgb + vSpec.rgb, 0.0, 1.0);\n";
    }

    s += AlphaTest(key, "prev");

    switch (key.m_fog) {
        case 1:
            s += "    float f = clamp((uFogParams.y - vFog) / (abs(uFogParams.y - uFogParams.x) < 1e-6 ? 1e-6 : uFogParams.y - uFogParams.x), 0.0, 1.0);\n";
            break;
        case 2:
            s += "    float f = clamp(exp(-uFogParams.z * vFog), 0.0, 1.0);\n";
            break;
        case 3:
            s += "    float f = clamp(exp(-(uFogParams.z * vFog) * (uFogParams.z * vFog)), 0.0, 1.0);\n";
            break;
        default:
            break;
    }

    if (key.m_fog) {
        s += "    prev.rgb = mix(uFogColor.rgb, prev.rgb, f);\n";
    }

    s += "    fragColor = prev;\n}\n";

    return s;
}

GLuint CompileShader(GLenum type, const std::string& source, bool& success) {
    GLuint shader = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, SHIM_COMPILE_STATUS, &ok);

    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        fprintf(stderr, "[GlShim] shader compile failed:\n%s\n%s\n", log, text);
    }

    success = ok != 0;
    return shader;
}

Program& GetProgram(const ComboKey& key, const ArbObject* vp, const ArbObject* fp) {
    std::string id(reinterpret_cast<const char*>(&key), sizeof(key));
    auto it = s_programs.find(id);

    if (it != s_programs.end()) {
        return it->second;
    }

    bool vsOk;
    bool fsOk;
    std::string vsText = vp ? ArbVertexShader(vp->m_program) : VertexSource(key.m_ff);
    std::string fsText = fp ? ArbFragmentShader(fp->m_program, key.m_ff.m_pointSprite, ArbFragmentDecls(key.m_ff), ArbFragmentEpilogue(key.m_ff)) : FragmentSource(key.m_ff);
    GLuint vs = CompileShader(SHIM_VERTEX_SHADER, vsText, vsOk);
    GLuint fs = CompileShader(SHIM_FRAGMENT_SHADER, fsText, fsOk);
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(program, SHIM_LINK_STATUS, &ok);

    if (!ok && vsOk && fsOk) {
        char log[2048];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        fprintf(stderr, "[GlShim] program link failed: %s\n%s\n%s\n", log, vsText.c_str(), fsText.c_str());
    }

    Program p;
    memset(&p, 0, sizeof(p));
    p.m_program = program;
    p.m_failed = static_cast<uint8_t>((vsOk ? 0 : 1) | (fsOk ? 0 : 2));

    if (!ok && !p.m_failed) {
        p.m_failed = 3;
    }

    p.m_vc = glGetUniformLocation(program, "uVC");
    p.m_fc = glGetUniformLocation(program, "uFC");
    p.m_mv = glGetUniformLocation(program, "uMV");
    p.m_p = glGetUniformLocation(program, "uP");
    p.m_nm = glGetUniformLocation(program, "uNM");
    p.m_matAmb = glGetUniformLocation(program, "uMatAmb");
    p.m_matDiff = glGetUniformLocation(program, "uMatDiff");
    p.m_matSpec = glGetUniformLocation(program, "uMatSpec");
    p.m_matEmis = glGetUniformLocation(program, "uMatEmis");
    p.m_matShin = glGetUniformLocation(program, "uMatShin");
    p.m_sceneAmb = glGetUniformLocation(program, "uSceneAmb");
    p.m_lightPos = glGetUniformLocation(program, "uLightPos");
    p.m_lightAmb = glGetUniformLocation(program, "uLightAmb");
    p.m_lightDiff = glGetUniformLocation(program, "uLightDiff");
    p.m_lightSpec = glGetUniformLocation(program, "uLightSpec");
    p.m_lightAtt = glGetUniformLocation(program, "uLightAtt");
    p.m_texMat = glGetUniformLocation(program, "uTexMat");
    p.m_eyePlane = glGetUniformLocation(program, "uEyePlane");
    p.m_objPlane = glGetUniformLocation(program, "uObjPlane");
    p.m_envColor = glGetUniformLocation(program, "uEnvColor");
    p.m_clip = glGetUniformLocation(program, "uClip");
    p.m_pointSize = glGetUniformLocation(program, "uPointSize");
    p.m_pointAtt = glGetUniformLocation(program, "uPointAtt");
    p.m_pointClamp = glGetUniformLocation(program, "uPointClamp");
    p.m_fogColor = glGetUniformLocation(program, "uFogColor");
    p.m_fogParams = glGetUniformLocation(program, "uFogParams");
    p.m_alphaRef = glGetUniformLocation(program, "uAlphaRef");

    glUseProgram(program);

    for (int i = 0; i < 16; i++) {
        GLint sampler = glGetUniformLocation(program, ("uSampler" + Str(i)).c_str());

        if (sampler >= 0) {
            glUniform1i(sampler, i);
        }

        sampler = glGetUniformLocation(program, ("s" + Str(i)).c_str());

        if (sampler >= 0) {
            glUniform1i(sampler, i);
        }
    }

    s_currentProgram = nullptr;

    return s_programs.emplace(id, p).first->second;
}

void UploadUniforms(Program& p) {
    const State& s = s_state;

    glUniformMatrix4fv(p.m_mv, 1, GL_FALSE, s.m_modelView.m);
    glUniformMatrix4fv(p.m_p, 1, GL_FALSE, s.m_projection.m);

    // Normal matrix: inverse transpose of the modelview's upper 3x3.
    Mat4 inverse;
    Invert(s.m_modelView, inverse);
    float nm[9];

    for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) {
            nm[col * 3 + row] = inverse.m[row * 4 + col];
        }
    }

    glUniformMatrix3fv(p.m_nm, 1, GL_FALSE, nm);

    if (p.m_lightPos >= 0) {
        float pos[16], amb[16], diff[16], spec[16], att[12];

        for (int i = 0; i < kLights; i++) {
            Copy4(pos + i * 4, s.m_lights[i].m_position);
            Copy4(amb + i * 4, s.m_lights[i].m_ambient);
            Copy4(diff + i * 4, s.m_lights[i].m_diffuse);
            Copy4(spec + i * 4, s.m_lights[i].m_specular);
            memcpy(att + i * 3, s.m_lights[i].m_attenuation, sizeof(float) * 3);
        }

        glUniform4fv(p.m_lightPos, kLights, pos);
        glUniform4fv(p.m_lightAmb, kLights, amb);
        glUniform4fv(p.m_lightDiff, kLights, diff);
        glUniform4fv(p.m_lightSpec, kLights, spec);
        glUniform3fv(p.m_lightAtt, kLights, att);
    }

    glUniform4fv(p.m_matAmb, 1, s.m_matAmbient);
    glUniform4fv(p.m_matDiff, 1, s.m_matDiffuse);
    glUniform4fv(p.m_matSpec, 1, s.m_matSpecular);
    glUniform4fv(p.m_matEmis, 1, s.m_matEmission);
    glUniform1f(p.m_matShin, s.m_matShininess);
    glUniform4fv(p.m_sceneAmb, 1, s.m_sceneAmbient);

    float texMat[16 * kUnits], eye[16 * kUnits], obj[16 * kUnits], env[4 * kUnits];

    for (int i = 0; i < kUnits; i++) {
        const UnitState& u = s.m_units[i];
        memcpy(texMat + i * 16, u.m_matrix.m, sizeof(float) * 16);
        memcpy(eye + i * 16, u.m_eyePlane, sizeof(float) * 16);
        memcpy(obj + i * 16, u.m_objPlane, sizeof(float) * 16);
        Copy4(env + i * 4, u.m_envColor);
    }

    glUniformMatrix4fv(p.m_texMat, kUnits, GL_FALSE, texMat);
    glUniform4fv(p.m_eyePlane, 4 * kUnits, eye);
    glUniform4fv(p.m_objPlane, 4 * kUnits, obj);
    glUniform4fv(p.m_envColor, kUnits, env);
    glUniform4fv(p.m_clip, 6, &s.m_clipPlanes[0][0]);
    glUniform1f(p.m_pointSize, s.m_pointSize);
    glUniform3fv(p.m_pointAtt, 1, s.m_pointAttenuation);

    float clampRange[2] = { s.m_pointMin, s.m_pointMax };
    glUniform2fv(p.m_pointClamp, 1, clampRange);
    glUniform4fv(p.m_fogColor, 1, s.m_fogColor);

    float fog[3] = { s.m_fogStart, s.m_fogEnd, s.m_fogDensity };
    glUniform3fv(p.m_fogParams, 1, fog);
    glUniform1f(p.m_alphaRef, s.m_alphaRef);

    p.m_version = s.m_version;
}

void MatrixRow(const ArbSlot& slot, float* out) {
    const State& s = s_state;
    Mat4 m;

    switch (slot.m_matrix) {
        case ArbMatrix_ModelView:
            m = s.m_modelView;
            break;
        case ArbMatrix_Projection:
            m = s.m_projection;
            break;
        case ArbMatrix_Mvp:
            m = Multiply(s.m_projection, s.m_modelView);
            break;
        default:
            m = s.m_units[slot.m_unit & (kUnits - 1)].m_matrix;
            break;
    }

    if (slot.m_modifier == 1 || slot.m_modifier == 3) {
        Mat4 inverse;
        Invert(m, inverse);
        m = inverse;
    }

    bool transpose = slot.m_modifier == 2 || slot.m_modifier == 3;

    for (int c = 0; c < 4; c++) {
        out[c] = transpose ? m.m[slot.m_row * 4 + c] : m.m[c * 4 + slot.m_row];
    }
}

void UploadSlots(GLint location, const ArbObject& object, int stage) {
    const std::vector<ArbSlot>& slots = object.m_program.m_slots;

    if (location < 0 || slots.empty()) {
        return;
    }

    const State& s = s_state;
    static std::vector<float> s_data;
    s_data.assign(slots.size() * 4, 0.0f);

    for (size_t i = 0; i < slots.size(); i++) {
        const ArbSlot& slot = slots[i];
        float* out = &s_data[i * 4];

        switch (slot.m_kind) {
            case ArbSlot_Literal:
                Copy4(out, slot.m_value);
                break;
            case ArbSlot_Env:
                if (slot.m_index < kArbParams) {
                    Copy4(out, &s_arbEnv[stage][slot.m_index * 4]);
                }
                break;
            case ArbSlot_Local:
                if (slot.m_index * 4u < object.m_locals.size()) {
                    Copy4(out, &object.m_locals[slot.m_index * 4]);
                }
                break;
            case ArbSlot_Matrix:
                MatrixRow(slot, out);
                break;
            case ArbSlot_FogColor:
                Copy4(out, s.m_fogColor);
                break;
            case ArbSlot_FogParams:
                Set4(out, s.m_fogDensity, s.m_fogStart, s.m_fogEnd, 1.0f / std::max(s.m_fogEnd - s.m_fogStart, 1e-6f));
                break;
        }
    }

    glUniform4fv(location, static_cast<GLsizei>(slots.size()), s_data.data());
}

void SetupAttrib(GLuint location, const ArrayState& array, bool normalizeInts, const float* constant) {
    if (array.m_enabled && array.m_buffer) {
        glBindBuffer(GL_ARRAY_BUFFER_ARB, array.m_buffer);
        GLboolean normalized = normalizeInts && array.m_type != GL_FLOAT ? GL_TRUE : GL_FALSE;
        glVertexAttribPointer(location, array.m_size, array.m_type, normalized, array.m_stride, array.m_pointer);

        if (!(s_enabledAttribs & (1u << location))) {
            glEnableVertexAttribArray(location);
            s_enabledAttribs |= 1u << location;
        }

        return;
    }

    if (array.m_enabled && !array.m_buffer) {
        static bool s_warned;

        if (!s_warned) {
            fprintf(stderr, "[GlShim] client-memory vertex arrays are not supported\n");
            s_warned = true;
        }
    }

    if (s_enabledAttribs & (1u << location)) {
        glDisableVertexAttribArray(location);
        s_enabledAttribs &= ~(1u << location);
    }

    glVertexAttrib4fv(location, constant);
}

// The fixed-function part of a draw's key; a vertex / fragment program replaces the matching part.
ComboKey BuildCombo(const ArbObject* vp, const ArbObject* fp) {
    ComboKey combo;
    memset(&combo, 0, sizeof(combo));
    combo.m_vertexProgram = vp ? vp->m_serial : 0;
    combo.m_fragmentProgram = fp ? fp->m_serial : 0;

    ProgramKey& key = combo.m_ff;
    key = BuildKey(fp && !vp ? fp->m_program.m_texcoordMask : 0);

    if (vp) {
        // Lighting (and with it the implicit color sum), texgen, texture matrices and clip planes.
        key.m_lighting = key.m_lightMask = key.m_lightPoint = key.m_localViewer = key.m_separateSpecular = 0;
        key.m_colorMaterial = key.m_normalize = key.m_clipMask = 0;

        for (UnitKey& unit : key.m_units) {
            memset(unit.m_gen, 0, sizeof(unit.m_gen));
        }
    }

    if (fp) {
        // Texturing, the texture environment and fog; texgen stays for the texcoords the program reads.
        key.m_fog = 0;

        for (UnitKey& unit : key.m_units) {
            uint8_t gen[4];
            memcpy(gen, unit.m_gen, sizeof(gen));
            memset(&unit, 0, sizeof(unit));
            memcpy(unit.m_gen, gen, sizeof(gen));
        }
    }

    return combo;
}

void PrepareDraw() {
    State& s = s_state;
    ArbObject* vp = ActiveArb(0);
    ArbObject* fp = ActiveArb(1);
    ComboKey combo = BuildCombo(vp, fp);
    Program* program = &GetProgram(combo, vp, fp);

    if (program->m_failed && (vp || fp)) {
        // Drop the stage that did not compile (both if only the link failed) and use fixed function for it.
        fprintf(stderr, "[GlShim] ARB programs vp=%u fp=%u failed to build (%d); falling back to fixed function\n", vp ? s.m_boundVertexProgram : 0,
            fp ? s.m_boundFragmentProgram : 0, program->m_failed);

        if (vp && (program->m_failed & 1)) {
            vp->m_failed = true;
        }

        if (fp && (program->m_failed & 2)) {
            fp->m_failed = true;
        }

        vp = ActiveArb(0);
        fp = ActiveArb(1);
        combo = BuildCombo(vp, fp);
        program = &GetProgram(combo, vp, fp);
    }

    const ProgramKey& key = combo.m_ff;

    if (s_currentProgram != program) {
        glUseProgram(program->m_program);
        s_currentProgram = program;
    }

    if (program->m_version != s.m_version) {
        UploadUniforms(*program);
    }

    bool usesState = (vp && vp->m_program.m_usesState) || (fp && fp->m_program.m_usesState);

    if ((vp || fp) && (program->m_arbStamp != s_arbStamp || (usesState && program->m_arbVersion != s.m_version))) {
        if (vp) {
            UploadSlots(program->m_vc, *vp, 0);
        }

        if (fp) {
            UploadSlots(program->m_fc, *fp, 1);
        }

        program->m_arbStamp = s_arbStamp;
        program->m_arbVersion = s.m_version;
    }

    static const float kNormal[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
    static const float kTexCoord[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    static const float kPosition[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    uint32_t used = 0;

    if (vp) {
        // Generic attribute n is location n.
        for (GLuint i = 0; i < 16; i++) {
            if (vp->m_program.m_attribMask & (1u << i)) {
                SetupAttrib(i, s.m_attribArrays[i], s.m_attribArrays[i].m_normalized, kTexCoord);
                used |= 1u << i;
            }
        }
    } else {
        SetupAttrib(0, s.m_vertexArray, false, kPosition);
        SetupAttrib(1, s.m_normalArray, true, kNormal);
        SetupAttrib(2, s.m_colorArray, true, s.m_color);
        used = 7;

        for (int i = 0; i < kUnits; i++) {
            if (UnitTexcoords(key, i)) {
                SetupAttrib(kAttribTex0 + i, s.m_units[i].m_texCoordArray, false, kTexCoord);
                used |= 1u << (kAttribTex0 + i);
            }
        }
    }

    for (GLuint i = 0; i < 16; i++) {
        if (s_enabledAttribs & ~used & (1u << i)) {
            glDisableVertexAttribArray(i);
            s_enabledAttribs &= ~(1u << i);
        }
    }

    // The device caches the ARRAY_BUFFER binding; keep the real binding equal to what it last set.
    glBindBuffer(GL_ARRAY_BUFFER_ARB, s.m_arrayBuffer);

    if (s_dump) {
        GLint viewport[4], scissor[4], blendSrc, blendDst;
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        glGetIntegerv(0x80C9, &blendSrc);
        glGetIntegerv(0x80C8, &blendDst);
        std::string units;

        for (int i = 0; i < kUnits; i++) {
            const UnitKey& k = key.m_units[i];
            int target = fp ? (fp->m_program.m_samplers[i] == ArbSampler_Cube ? 2 : (fp->m_program.m_samplers[i] ? 1 : 0)) : k.m_target;

            if (!target) {
                continue;
            }

            GLuint name = target == 2 ? s.m_units[i].m_boundCube : s.m_units[i].m_bound2d;
            const TexInfo& info = s_texInfo[name];
            char buffer[160];
            snprintf(buffer, sizeof(buffer), " u%d[tex%u %dx%d fmt%x env%d c%d/%d src%d%d%d/%d%d%d gen%d%d%d arr%d]", i, name, info.m_width, info.m_height, info.m_format,
                k.m_envMode, k.m_combine[0], k.m_combine[1], k.m_source[0][0], k.m_source[0][1], k.m_source[0][2], k.m_source[1][0], k.m_source[1][1], k.m_source[1][2],
                k.m_gen[0], k.m_gen[1], k.m_gen[2], vp ? s.m_attribArrays[6 + i].m_enabled : s.m_units[i].m_texCoordArray.m_enabled);
            units += buffer;
        }

        printf("[draw %d] vp=%u fp=%u view=%d,%d,%d,%d sc=%d%s%d,%d,%d,%d blend=%d %x/%x depth=%d lit=%d fog=%d atest=%d/%.2f clip=%x col=%d(%d) fbo=%u%s\n", s_dumpIndex++,
            vp ? s.m_boundVertexProgram : 0, fp ? s.m_boundFragmentProgram : 0, viewport[0], viewport[1], viewport[2], viewport[3], glIsEnabled(GL_SCISSOR_TEST), "@",
            scissor[0], scissor[1], scissor[2], scissor[3], glIsEnabled(GL_BLEND), blendSrc, blendDst, glIsEnabled(GL_DEPTH_TEST), key.m_lighting, fp ? fp->m_program.m_fog : key.m_fog,
            key.m_alphaFunc, s.m_alphaRef, key.m_clipMask, vp ? s.m_attribArrays[4].m_enabled : s.m_colorArray.m_enabled, s.m_colorArray.m_size, s.m_framebuffer, units.c_str());
    }
}

// ---- texture uploads ---------------------------------------------------------------------------------------------

struct Upload {
    GLenum m_internal;
    GLenum m_format;
    GLenum m_type;
    std::vector<uint8_t> m_data;
    bool m_hasData;
};

uint32_t BytesPerPixel(GLenum format, GLenum type) {
    if (type == GL_FLOAT) {
        return format == GL_DEPTH_COMPONENT ? 4 : 12;
    }

    if (type == SHIM_HALF_FLOAT) {
        return 6;
    }

    if (type == GL_UNSIGNED_BYTE) {
        return format == GL_RGB ? 3 : 4;
    }

    return type == GL_UNSIGNED_INT_8_8_8_8_REV ? 4 : 2;
}

// Converts device upload formats (D3D-style BGRA and packed _REV types) to WebGL2 sized formats, honoring the tracked
// GL_UNPACK_ROW_LENGTH; the result is tightly packed.
Upload Convert(GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid* pixels) {
    Upload up;
    up.m_hasData = pixels != nullptr;

    if (format == GL_BGRA && type == GL_UNSIGNED_INT_8_8_8_8_REV) {
        up.m_internal = SHIM_RGBA8;
        up.m_format = GL_RGBA;
        up.m_type = GL_UNSIGNED_BYTE;
    } else if (format == GL_BGRA && type == SHIM_UNSIGNED_SHORT_4_4_4_4_REV) {
        up.m_internal = SHIM_RGBA4;
        up.m_format = GL_RGBA;
        up.m_type = SHIM_UNSIGNED_SHORT_4_4_4_4;
    } else if (format == GL_BGRA && type == SHIM_UNSIGNED_SHORT_1_5_5_5_REV) {
        up.m_internal = SHIM_RGB5_A1;
        up.m_format = GL_RGBA;
        up.m_type = SHIM_UNSIGNED_SHORT_5_5_5_1;
    } else if (format == GL_RGB && type == SHIM_UNSIGNED_SHORT_5_6_5) {
        up.m_internal = SHIM_RGB565;
        up.m_format = GL_RGB;
        up.m_type = SHIM_UNSIGNED_SHORT_5_6_5;
    } else if (format == GL_DEPTH_COMPONENT) {
        up.m_internal = SHIM_DEPTH_COMPONENT32F;
        up.m_format = GL_DEPTH_COMPONENT;
        up.m_type = GL_FLOAT;
    } else if (type == SHIM_HALF_FLOAT) {
        up.m_internal = SHIM_RGB16F;
        up.m_format = GL_RGB;
        up.m_type = SHIM_HALF_FLOAT;
    } else {
        up.m_internal = format == GL_RGB ? SHIM_RGB8 : SHIM_RGBA8;
        up.m_format = format == GL_RGB ? GL_RGB : GL_RGBA;
        up.m_type = GL_UNSIGNED_BYTE;
    }

    if (!pixels) {
        return up;
    }

    uint32_t bpp = BytesPerPixel(format, type);
    uint32_t srcPitch = (s_state.m_unpackRowLength > 0 ? s_state.m_unpackRowLength : width) * bpp;
    uint32_t dstPitch = width * bpp;
    up.m_data.resize(static_cast<size_t>(dstPitch) * height);

    const uint8_t* src = static_cast<const uint8_t*>(pixels);
    uint8_t* dst = up.m_data.data();

    for (GLsizei y = 0; y < height; y++, src += srcPitch, dst += dstPitch) {
        if (type == GL_UNSIGNED_INT_8_8_8_8_REV && format == GL_BGRA) {
            for (GLsizei x = 0; x < width; x++) {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = src[x * 4 + 3];
            }
        } else if (type == SHIM_UNSIGNED_SHORT_4_4_4_4_REV) {
            const uint16_t* in = reinterpret_cast<const uint16_t*>(src);
            uint16_t* out = reinterpret_cast<uint16_t*>(dst);

            for (GLsizei x = 0; x < width; x++) {
                uint16_t v = in[x];
                uint16_t b = v & 0xF, g = (v >> 4) & 0xF, r = (v >> 8) & 0xF, a = (v >> 12) & 0xF;
                out[x] = static_cast<uint16_t>((r << 12) | (g << 8) | (b << 4) | a);
            }
        } else if (type == SHIM_UNSIGNED_SHORT_1_5_5_5_REV) {
            const uint16_t* in = reinterpret_cast<const uint16_t*>(src);
            uint16_t* out = reinterpret_cast<uint16_t*>(dst);

            for (GLsizei x = 0; x < width; x++) {
                uint16_t v = in[x];
                uint16_t b = v & 0x1F, g = (v >> 5) & 0x1F, r = (v >> 10) & 0x1F, a = (v >> 15) & 0x1;
                out[x] = static_cast<uint16_t>((r << 11) | (g << 6) | (b << 1) | a);
            }
        } else {
            memcpy(dst, src, dstPitch);
        }
    }

    return up;
}

GLuint BoundTexture(GLenum target);

void RecordTexture(GLenum target, GLint level, GLsizei width, GLsizei height, GLenum format) {
    if (level == 0) {
        s_texInfo[BoundTexture(target == GL_TEXTURE_2D ? GL_TEXTURE_2D : SHIM_TEXTURE_CUBE_MAP)] = { width, height, format };
    }
}

GLuint BoundTexture(GLenum target) {
    const UnitState& u = ActiveUnit();
    return target == GL_TEXTURE_2D ? u.m_bound2d : u.m_boundCube;
}

void MaybeGenerateMipmap(GLenum target, GLint level) {
    if (level != 0 || target != GL_TEXTURE_2D) {
        return;
    }

    auto it = s_generateMipmap.find(BoundTexture(GL_TEXTURE_2D));

    if (it != s_generateMipmap.end() && it->second) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
}

// ---- extension entry points --------------------------------------------------------------------------------------

void APIENTRY ShimActiveTexture(GLenum texture) {
    s_state.m_activeUnit = (texture - GL_TEXTURE0_ARB) & (kUnits - 1);
    glActiveTexture(texture);
}

void APIENTRY ShimClientActiveTexture(GLenum texture) {
    s_state.m_clientUnit = (texture - GL_TEXTURE0_ARB) & (kUnits - 1);
}

void APIENTRY ShimBindBuffer(GLenum target, GLuint buffer) {
    if (target == GL_ARRAY_BUFFER_ARB) {
        s_state.m_arrayBuffer = buffer;
    }

    glBindBuffer(target, buffer);
}

void APIENTRY ShimBufferData(GLenum target, GLsizeiptrARB size, const GLvoid* data, GLenum usage) {
    glBufferData(target, size, data, usage);
}

void APIENTRY ShimBufferSubData(GLenum target, GLintptrARB offset, GLsizeiptrARB size, const GLvoid* data) {
    glBufferSubData(target, offset, size, data);
}

void APIENTRY ShimGenBuffers(GLsizei n, GLuint* buffers) {
    glGenBuffers(n, buffers);
}

void APIENTRY ShimDeleteBuffers(GLsizei n, const GLuint* buffers) {
    glDeleteBuffers(n, buffers);
}

// Only the VBO broker maps buffers, and the device does not use it on web.
GLvoid* APIENTRY ShimMapBuffer(GLenum target, GLenum access) {
    return nullptr;
}

GLboolean APIENTRY ShimUnmapBuffer(GLenum target) {
    return GL_FALSE;
}

void CheckDrawError(const char* what, GLenum mode, GLsizei count) {
#if GLSHIM_CHECK_ERRORS
    static int s_reported;
    GLenum error = glGetError();

    if (error != GL_NO_ERROR && s_reported < 50) {
        s_reported++;
        fprintf(stderr, "[GlShim] %s mode=%u count=%d error=0x%x fbo=%u vbo=%u units=%d%d%d%d\n", what, mode, count, error, s_state.m_framebuffer, s_state.m_arrayBuffer,
            s_state.m_units[0].m_tex2d, s_state.m_units[1].m_tex2d, s_state.m_units[2].m_tex2d, s_state.m_units[3].m_tex2d);
    }
#endif
}


void APIENTRY ShimDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid* indices) {
    PrepareDraw();
    glDrawRangeElements(mode, start, end, count, type, indices);
    CheckDrawError("drawRangeElements", mode, count);
}

void APIENTRY ShimCompressedTexImage2D(GLenum target, GLint level, GLenum internalFormat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const GLvoid* data) {
    glCompressedTexImage2D(target, level, internalFormat, width, height, border, imageSize, data);
    RecordTexture(target, level, width, height, internalFormat);
}

void APIENTRY ShimCompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const GLvoid* data) {
    glCompressedTexSubImage2D(target, level, x, y, width, height, format, imageSize, data);
}

void APIENTRY ShimGenFramebuffers(GLsizei n, GLuint* framebuffers) {
    glGenFramebuffers(n, framebuffers);
}

void APIENTRY ShimDeleteFramebuffers(GLsizei n, const GLuint* framebuffers) {
    glDeleteFramebuffers(n, framebuffers);
}

void APIENTRY ShimBindFramebuffer(GLenum target, GLuint framebuffer) {
    s_state.m_framebuffer = framebuffer;
    glBindFramebuffer(target, framebuffer);
}

GLenum APIENTRY ShimCheckFramebufferStatus(GLenum target) {
    return glCheckFramebufferStatus(target);
}

void APIENTRY ShimFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

void APIENTRY ShimFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {
    glFramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer);
}

void APIENTRY ShimGenRenderbuffers(GLsizei n, GLuint* renderbuffers) {
    glGenRenderbuffers(n, renderbuffers);
}

void APIENTRY ShimDeleteRenderbuffers(GLsizei n, const GLuint* renderbuffers) {
    glDeleteRenderbuffers(n, renderbuffers);
}

void APIENTRY ShimBindRenderbuffer(GLenum target, GLuint renderbuffer) {
    glBindRenderbuffer(target, renderbuffer);
}

void APIENTRY ShimRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    glRenderbufferStorage(target, internalformat, width, height);
}

void APIENTRY ShimGenQueries(GLsizei n, GLuint* ids) {
    glGenQueries(n, ids);
}

void APIENTRY ShimDeleteQueries(GLsizei n, const GLuint* ids) {
    glDeleteQueries(n, ids);
}

// WebGL2 has no sample counts, only "any samples passed"; the game only tests for visibility.
void APIENTRY ShimBeginQuery(GLenum target, GLuint id) {
    glBeginQuery(SHIM_ANY_SAMPLES_PASSED_CONSERVATIVE, id);
}

void APIENTRY ShimEndQuery(GLenum target) {
    glEndQuery(SHIM_ANY_SAMPLES_PASSED_CONSERVATIVE);
}

void APIENTRY ShimGetQueryiv(GLenum target, GLenum pname, GLint* params) {
    *params = pname == GL_QUERY_COUNTER_BITS_ARB ? 32 : 0;
}

void APIENTRY ShimGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params) {
    glGetQueryObjectuiv(id, pname, params);
}

void APIENTRY ShimGetQueryObjectiv(GLuint id, GLenum pname, GLint* params) {
    GLuint value = 0;
    glGetQueryObjectuiv(id, pname, &value);
    *params = static_cast<GLint>(value);
}

void APIENTRY ShimPointParameterf(GLenum pname, GLfloat param) {
    if (pname == GL_POINT_SIZE_MIN_ARB) {
        s_state.m_pointMin = param;
    } else if (pname == GL_POINT_SIZE_MAX_ARB) {
        s_state.m_pointMax = param;
    }

    Dirty();
}

void APIENTRY ShimPointParameterfv(GLenum pname, const GLfloat* params) {
    if (pname == GL_POINT_DISTANCE_ATTENUATION_ARB) {
        memcpy(s_state.m_pointAttenuation, params, sizeof(float) * 3);
        Dirty();
    } else {
        ShimPointParameterf(pname, params[0]);
    }
}

void APIENTRY ShimBlendColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    glBlendColor(red, green, blue, alpha);
}

void APIENTRY ShimGenPrograms(GLsizei n, GLuint* programs) {
    for (GLsizei i = 0; i < n; i++) {
        programs[i] = s_arbNextId++;
        s_arbObjects[programs[i]];
    }
}

void APIENTRY ShimDeletePrograms(GLsizei n, const GLuint* programs) {
    for (GLsizei i = 0; i < n; i++) {
        s_arbObjects.erase(programs[i]);

        for (int stage = 0; stage < 2; stage++) {
            if (ArbBound(stage) == programs[i]) {
                ArbBound(stage) = 0;
            }
        }
    }
}

// Binding an unused name creates the program object, as in GL.
void APIENTRY ShimBindProgram(GLenum target, GLuint program) {
    if (program) {
        s_arbObjects[program];
    }

    ArbBound(ArbStage(target)) = program;
}

GLboolean APIENTRY ShimIsProgram(GLuint program) {
    return program && s_arbObjects.count(program) ? GL_TRUE : GL_FALSE;
}

void APIENTRY ShimProgramString(GLenum target, GLenum format, GLsizei length, const GLvoid* string) {
    GLuint id = ArbBound(ArbStage(target));
    auto it = s_arbObjects.find(id);

    if (!id || it == s_arbObjects.end() || format != GL_PROGRAM_FORMAT_ASCII_ARB) {
        s_arbErrorPosition = 0;
        return;
    }

    ArbObject& object = it->second;
    const char* text = static_cast<const char*>(string);
    object.m_serial = ++s_arbSerial;
    object.m_failed = false;

    if (ArbParse(text, static_cast<size_t>(length), object.m_program) && object.m_program.m_vertex == (ArbStage(target) == 0)) {
        s_arbErrorPosition = -1;
        return;
    }

    object.m_program.m_valid = false;
    s_arbErrorPosition = std::max(object.m_program.m_errorPosition, 0);

    static int s_reported;

    if (s_reported++ < 20) {
        int position = s_arbErrorPosition;
        fprintf(stderr, "[GlShim] ARB program %u rejected at %d: %s\n  near: %.80s\n", id, position, object.m_program.m_error.c_str(), position < length ? text + position : "");
    }
}

void APIENTRY ShimGetProgramiv(GLenum target, GLenum pname, GLint* params) {
    int stage = ArbStage(target);

    switch (pname) {
        case GL_PROGRAM_BINDING_ARB:
            *params = static_cast<GLint>(ArbBound(stage));
            return;
        case GL_MAX_PROGRAM_ENV_PARAMETERS_ARB:
        case GL_MAX_PROGRAM_LOCAL_PARAMETERS_ARB:
        case GL_MAX_PROGRAM_NATIVE_PARAMETERS_ARB:
            *params = s_arbMaxParams[stage];
            return;
        case GL_PROGRAM_UNDER_NATIVE_LIMITS_ARB: {
            auto it = s_arbObjects.find(ArbBound(stage));
            *params = it != s_arbObjects.end() && it->second.m_program.m_valid;
            return;
        }
        default:
            *params = 0;
            return;
    }
}

void APIENTRY ShimProgramEnvParameters4fv(GLenum target, GLuint index, GLsizei count, const GLfloat* params) {
    if (index >= static_cast<GLuint>(kArbParams) || count <= 0) {
        return;
    }

    count = std::min(count, static_cast<GLsizei>(kArbParams - index));
    memcpy(&s_arbEnv[ArbStage(target)][index * 4], params, sizeof(float) * 4 * count);
    s_arbStamp++;
}

void APIENTRY ShimProgramEnvParameter4fv(GLenum target, GLuint index, const GLfloat* params) {
    ShimProgramEnvParameters4fv(target, index, 1, params);
}

void APIENTRY ShimProgramLocalParameters4fv(GLenum target, GLuint index, GLsizei count, const GLfloat* params) {
    auto it = s_arbObjects.find(ArbBound(ArbStage(target)));

    if (it == s_arbObjects.end() || index >= static_cast<GLuint>(kArbParams) || count <= 0) {
        return;
    }

    std::vector<float>& locals = it->second.m_locals;
    locals.resize(kArbParams * 4);
    count = std::min(count, static_cast<GLsizei>(kArbParams - index));
    memcpy(&locals[index * 4], params, sizeof(float) * 4 * count);
    s_arbStamp++;
}

void APIENTRY ShimProgramLocalParameter4fv(GLenum target, GLuint index, const GLfloat* params) {
    ShimProgramLocalParameters4fv(target, index, 1, params);
}

void APIENTRY ShimVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const GLvoid* pointer) {
    if (index < 16) {
        ArrayState& a = s_state.m_attribArrays[index];
        a.m_size = size;
        a.m_type = type;
        a.m_normalized = normalized != GL_FALSE;
        a.m_stride = stride;
        a.m_pointer = pointer;
        a.m_buffer = s_state.m_arrayBuffer;
    }
}

void APIENTRY ShimEnableVertexAttribArray(GLuint index) {
    if (index < 16) {
        s_state.m_attribArrays[index].m_enabled = true;
    }
}

void APIENTRY ShimDisableVertexAttribArray(GLuint index) {
    if (index < 16) {
        s_state.m_attribArrays[index].m_enabled = false;
    }
}

}

// ---- legacy entry points -----------------------------------------------------------------------------------------

void GlShimSetDump(bool dump) {
    if (dump && !s_dump) {
        printf("[GlShim] ---- frame dump ----\n");
        s_dumpIndex = 0;
    }

    s_dump = dump;
}

void GlShimInitialize() {
    ResetState();
    s_programs.clear();
    s_currentProgram = nullptr;
    s_generateMipmap.clear();
    s_enabledAttribs = 0;

    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context = emscripten_webgl_get_current_context();
    glExtTextureCompressionS3tc = emscripten_webgl_enable_extension(context, "WEBGL_compressed_texture_s3tc");
    s_hasAnisotropy = emscripten_webgl_enable_extension(context, "EXT_texture_filter_anisotropic");
    emscripten_webgl_enable_extension(context, "EXT_color_buffer_float");
    glExtTextureFilterAnisotropic = s_hasAnisotropy;

    glARBVertexBufferObject = 1;
    glARBTextureEnvCombine = 1;
    glARBTextureCubeMap = 1;
    glARBOcclusionQuery = 1;
    glARBPointParameters = 1;
    glARBPointSprite = 1;
    glExtTextureCompression = 1;
    glExtClampToEdge = 1;
    glExtBgra = 1;
    glExtBlendColor = 1;
    glExtFramebufferObject = 1;
    glExtDrawRangeElements = 1;
    glSGISGenerateMipmap = 1;
    glMaxTextureUnits = kUnits;
    glMaxTextureImageUnitsARB = 16;
    glVersion = 0x15;

    // ARB programs. Program objects and parameters outlive a device reset: the device keeps its program names.
    glARBVertexProgram = 1;
    glARBFragmentProgram = 1;
    glExtGPUProgramParameters = 1;

    GLint vectors = 0;
    glGetIntegerv(SHIM_MAX_VERTEX_UNIFORM_VECTORS, &vectors);
    s_arbMaxParams[0] = std::min(kArbParams, vectors - 16);
    glGetIntegerv(SHIM_MAX_FRAGMENT_UNIFORM_VECTORS, &vectors);
    s_arbMaxParams[1] = std::min(kArbParams, vectors - 16);
    s_arbErrorPosition = -1;

    glGenProgramsARB = ShimGenPrograms;
    glDeleteProgramsARB = ShimDeletePrograms;
    glBindProgramARB = ShimBindProgram;
    glIsProgramARB = ShimIsProgram;
    glProgramStringARB = ShimProgramString;
    glGetProgramivARB = ShimGetProgramiv;
    glProgramEnvParameter4fvARB = ShimProgramEnvParameter4fv;
    glProgramEnvParameters4fvEXT = ShimProgramEnvParameters4fv;
    glProgramLocalParameter4fvARB = ShimProgramLocalParameter4fv;
    glProgramLocalParameters4fvEXT = ShimProgramLocalParameters4fv;
    glVertexAttribPointerARB = ShimVertexAttribPointer;
    glEnableVertexAttribArrayARB = ShimEnableVertexAttribArray;
    glDisableVertexAttribArrayARB = ShimDisableVertexAttribArray;

    glActiveTextureARB = ShimActiveTexture;
    glClientActiveTextureARB = ShimClientActiveTexture;
    glBindBufferARB = ShimBindBuffer;
    glBufferDataARB = ShimBufferData;
    glBufferSubDataARB = ShimBufferSubData;
    glGenBuffersARB = ShimGenBuffers;
    glDeleteBuffersARB = ShimDeleteBuffers;
    glMapBufferARB = ShimMapBuffer;
    glUnmapBufferARB = ShimUnmapBuffer;
    glDrawRangeElementsEXT = ShimDrawRangeElements;
    glCompressedTexImage2DARB = ShimCompressedTexImage2D;
    glCompressedTexSubImage2DARB = ShimCompressedTexSubImage2D;
    glGenFramebuffersEXT = ShimGenFramebuffers;
    glDeleteFramebuffersEXT = ShimDeleteFramebuffers;
    glBindFramebufferEXT = ShimBindFramebuffer;
    glCheckFramebufferStatusEXT = ShimCheckFramebufferStatus;
    glFramebufferTexture2DEXT = ShimFramebufferTexture2D;
    glFramebufferRenderbufferEXT = ShimFramebufferRenderbuffer;
    glGenRenderbuffersEXT = ShimGenRenderbuffers;
    glDeleteRenderbuffersEXT = ShimDeleteRenderbuffers;
    glBindRenderbufferEXT = ShimBindRenderbuffer;
    glRenderbufferStorageEXT = ShimRenderbufferStorage;
    glGenQueriesARB = ShimGenQueries;
    glDeleteQueriesARB = ShimDeleteQueries;
    glBeginQueryARB = ShimBeginQuery;
    glEndQueryARB = ShimEndQuery;
    glGetQueryivARB = ShimGetQueryiv;
    glGetQueryObjectuivARB = ShimGetQueryObjectuiv;
    glGetQueryObjectivARB = ShimGetQueryObjectiv;
    glPointParameterfARB = ShimPointParameterf;
    glPointParameterfvARB = ShimPointParameterfv;
    glBlendColorEXT = ShimBlendColor;
}

void GlShimEnable(GLenum cap) {
    State& s = s_state;
    UnitState& u = ActiveUnit();

    switch (cap) {
        case GL_LIGHTING:
            s.m_lighting = true;
            break;
        case GL_LIGHT0:
        case GL_LIGHT1:
        case GL_LIGHT2:
        case GL_LIGHT3:
            s.m_lights[cap - GL_LIGHT0].m_enabled = true;
            break;
        case GL_COLOR_MATERIAL:
            s.m_colorMaterialEnabled = true;
            break;
        case GL_NORMALIZE:
            s.m_normalize = true;
            break;
        case GL_FOG:
            s.m_fog = true;
            break;
        case GL_ALPHA_TEST:
            s.m_alphaTest = true;
            break;
        case GL_TEXTURE_2D:
            u.m_tex2d = true;
            break;
        case GL_TEXTURE_CUBE_MAP_ARB:
            u.m_texCube = true;
            break;
        case GL_TEXTURE_GEN_S:
        case GL_TEXTURE_GEN_T:
        case GL_TEXTURE_GEN_R:
        case GL_TEXTURE_GEN_Q:
            u.m_genEnabled[cap - GL_TEXTURE_GEN_S] = true;
            break;
        case GL_CLIP_PLANE0:
        case GL_CLIP_PLANE1:
        case GL_CLIP_PLANE2:
        case GL_CLIP_PLANE3:
        case GL_CLIP_PLANE4:
        case GL_CLIP_PLANE5:
            s.m_clipMask |= 1u << (cap - GL_CLIP_PLANE0);
            break;
        case GL_POINT_SPRITE_ARB:
            s.m_pointSprite = true;
            break;
        case GL_VERTEX_PROGRAM_ARB:
            s.m_vertexProgram = true;
            return;
        case GL_FRAGMENT_PROGRAM_ARB:
            s.m_fragmentProgram = true;
            return;
        case GL_BLEND:
        case GL_DEPTH_TEST:
        case GL_CULL_FACE:
        case GL_SCISSOR_TEST:
        case GL_POLYGON_OFFSET_FILL:
        case GL_DITHER:
        case GL_STENCIL_TEST:
            glEnable(cap);
            return;
        default:
            return;
    }

    Dirty();
}

void GlShimDisable(GLenum cap) {
    State& s = s_state;
    UnitState& u = ActiveUnit();

    switch (cap) {
        case GL_LIGHTING:
            s.m_lighting = false;
            break;
        case GL_LIGHT0:
        case GL_LIGHT1:
        case GL_LIGHT2:
        case GL_LIGHT3:
            s.m_lights[cap - GL_LIGHT0].m_enabled = false;
            break;
        case GL_COLOR_MATERIAL:
            s.m_colorMaterialEnabled = false;
            break;
        case GL_NORMALIZE:
            s.m_normalize = false;
            break;
        case GL_FOG:
            s.m_fog = false;
            break;
        case GL_ALPHA_TEST:
            s.m_alphaTest = false;
            break;
        case GL_TEXTURE_2D:
            u.m_tex2d = false;
            break;
        case GL_TEXTURE_CUBE_MAP_ARB:
            u.m_texCube = false;
            break;
        case GL_TEXTURE_GEN_S:
        case GL_TEXTURE_GEN_T:
        case GL_TEXTURE_GEN_R:
        case GL_TEXTURE_GEN_Q:
            u.m_genEnabled[cap - GL_TEXTURE_GEN_S] = false;
            break;
        case GL_CLIP_PLANE0:
        case GL_CLIP_PLANE1:
        case GL_CLIP_PLANE2:
        case GL_CLIP_PLANE3:
        case GL_CLIP_PLANE4:
        case GL_CLIP_PLANE5:
            s.m_clipMask &= ~(1u << (cap - GL_CLIP_PLANE0));
            break;
        case GL_POINT_SPRITE_ARB:
            s.m_pointSprite = false;
            break;
        case GL_VERTEX_PROGRAM_ARB:
            s.m_vertexProgram = false;
            return;
        case GL_FRAGMENT_PROGRAM_ARB:
            s.m_fragmentProgram = false;
            return;
        case GL_BLEND:
        case GL_DEPTH_TEST:
        case GL_CULL_FACE:
        case GL_SCISSOR_TEST:
        case GL_POLYGON_OFFSET_FILL:
        case GL_DITHER:
        case GL_STENCIL_TEST:
            glDisable(cap);
            return;
        default:
            return;
    }

    Dirty();
}

void GlShimTexEnvi(GLenum target, GLenum pname, GLint param) {
    if (target != GL_TEXTURE_ENV) {
        return;
    }

    UnitState& u = ActiveUnit();

    switch (pname) {
        case GL_TEXTURE_ENV_MODE:
            u.m_envMode = param;
            break;
        case GL_COMBINE_RGB:
            u.m_combineRgb = param;
            break;
        case GL_COMBINE_ALPHA:
            u.m_combineAlpha = param;
            break;
        case GL_SOURCE0_RGB:
        case GL_SOURCE1_RGB:
        case GL_SOURCE2_RGB:
            u.m_sourceRgb[pname - GL_SOURCE0_RGB] = param;
            break;
        case GL_SOURCE0_ALPHA:
        case GL_SOURCE1_ALPHA:
        case GL_SOURCE2_ALPHA:
            u.m_sourceAlpha[pname - GL_SOURCE0_ALPHA] = param;
            break;
        case GL_OPERAND0_RGB:
        case GL_OPERAND1_RGB:
        case GL_OPERAND2_RGB:
            u.m_operandRgb[pname - GL_OPERAND0_RGB] = param;
            break;
        case GL_OPERAND0_ALPHA:
        case GL_OPERAND1_ALPHA:
        case GL_OPERAND2_ALPHA:
            u.m_operandAlpha[pname - GL_OPERAND0_ALPHA] = param;
            break;
        case GL_RGB_SCALE:
            u.m_scaleRgb = static_cast<float>(param);
            break;
        case GL_ALPHA_SCALE:
            u.m_scaleAlpha = static_cast<float>(param);
            break;
        default:
            return;
    }

    Dirty();
}

void GlShimTexEnvf(GLenum target, GLenum pname, GLfloat param) {
    if (target != GL_TEXTURE_ENV) {
        return;
    }

    if (pname == GL_RGB_SCALE) {
        ActiveUnit().m_scaleRgb = param;
    } else if (pname == GL_ALPHA_SCALE) {
        ActiveUnit().m_scaleAlpha = param;
    } else {
        GlShimTexEnvi(target, pname, static_cast<GLint>(param));
        return;
    }

    Dirty();
}

void GlShimTexEnvfv(GLenum target, GLenum pname, const GLfloat* params) {
    if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_COLOR) {
        Copy4(ActiveUnit().m_envColor, params);
        Dirty();
    } else {
        GlShimTexEnvf(target, pname, params[0]);
    }
}

void GlShimTexEnviv(GLenum target, GLenum pname, const GLint* params) {
    GlShimTexEnvi(target, pname, params[0]);
}

void GlShimTexParameteri(GLenum target, GLenum pname, GLint param) {
    switch (pname) {
        case GL_GENERATE_MIPMAP_SGIS:
            s_generateMipmap[BoundTexture(target)] = param != 0;
            return;
        case GL_DEPTH_TEXTURE_MODE_ARB:
            return;
        case GL_TEXTURE_MAX_ANISOTROPY_EXT:
            if (!s_hasAnisotropy) {
                return;
            }
            break;
        case GL_TEXTURE_WRAP_S:
        case GL_TEXTURE_WRAP_T:
            if (param == GL_CLAMP) {
                param = GL_CLAMP_TO_EDGE;
            }
            break;
        default:
            break;
    }

    glTexParameteri(target, pname, param);
}

void GlShimLightf(GLenum light, GLenum pname, GLfloat param) {
    if (light - GL_LIGHT0 >= static_cast<GLenum>(kLights)) {
        return;
    }

    LightState& l = s_state.m_lights[light - GL_LIGHT0];

    switch (pname) {
        case GL_CONSTANT_ATTENUATION:
            l.m_attenuation[0] = param;
            break;
        case GL_LINEAR_ATTENUATION:
            l.m_attenuation[1] = param;
            break;
        case GL_QUADRATIC_ATTENUATION:
            l.m_attenuation[2] = param;
            break;
        default:
            return;
    }

    Dirty();
}

void GlShimLightfv(GLenum light, GLenum pname, const GLfloat* params) {
    if (light - GL_LIGHT0 >= static_cast<GLenum>(kLights)) {
        return;
    }

    LightState& l = s_state.m_lights[light - GL_LIGHT0];

    switch (pname) {
        case GL_POSITION:
            TransformPoint(s_state.m_modelView, params, l.m_position);
            break;
        case GL_AMBIENT:
            Copy4(l.m_ambient, params);
            break;
        case GL_DIFFUSE:
            Copy4(l.m_diffuse, params);
            break;
        case GL_SPECULAR:
            Copy4(l.m_specular, params);
            break;
        default:
            GlShimLightf(light, pname, params[0]);
            return;
    }

    Dirty();
}

void GlShimLightModelfv(GLenum pname, const GLfloat* params) {
    if (pname == GL_LIGHT_MODEL_AMBIENT) {
        Copy4(s_state.m_sceneAmbient, params);
        Dirty();
    } else {
        GlShimLightModeli(pname, static_cast<GLint>(params[0]));
    }
}

void GlShimLightModeli(GLenum pname, GLint param) {
    if (pname == GL_LIGHT_MODEL_LOCAL_VIEWER) {
        s_state.m_localViewer = param != 0;
    } else if (pname == GL_LIGHT_MODEL_COLOR_CONTROL) {
        s_state.m_separateSpecular = param == GL_SEPARATE_SPECULAR_COLOR;
    } else {
        return;
    }

    Dirty();
}

void GlShimMaterialf(GLenum face, GLenum pname, GLfloat param) {
    if (pname == GL_SHININESS) {
        s_state.m_matShininess = param;
        Dirty();
    }
}

void GlShimMaterialfv(GLenum face, GLenum pname, const GLfloat* params) {
    State& s = s_state;

    switch (pname) {
        case GL_AMBIENT:
            Copy4(s.m_matAmbient, params);
            break;
        case GL_DIFFUSE:
            Copy4(s.m_matDiffuse, params);
            break;
        case GL_AMBIENT_AND_DIFFUSE:
            Copy4(s.m_matAmbient, params);
            Copy4(s.m_matDiffuse, params);
            break;
        case GL_SPECULAR:
            Copy4(s.m_matSpecular, params);
            break;
        case GL_EMISSION:
            Copy4(s.m_matEmission, params);
            break;
        case GL_SHININESS:
            s.m_matShininess = params[0];
            break;
        default:
            return;
    }

    Dirty();
}

void GlShimColorMaterial(GLenum face, GLenum mode) {
    s_state.m_colorMaterialMode = mode;
    Dirty();
}

void GlShimTexGeni(GLenum coord, GLenum pname, GLint param) {
    if (pname == GL_TEXTURE_GEN_MODE && coord >= GL_S && coord <= GL_Q) {
        ActiveUnit().m_genMode[coord - GL_S] = param;
        Dirty();
    }
}

void GlShimTexGenfv(GLenum coord, GLenum pname, const GLfloat* params) {
    if (coord < GL_S || coord > GL_Q) {
        return;
    }

    UnitState& u = ActiveUnit();

    if (pname == GL_EYE_PLANE) {
        TransformPlane(params, u.m_eyePlane[coord - GL_S]);
    } else if (pname == GL_OBJECT_PLANE) {
        Copy4(u.m_objPlane[coord - GL_S], params);
    } else {
        return;
    }

    Dirty();
}

void GlShimFogf(GLenum pname, GLfloat param) {
    State& s = s_state;

    switch (pname) {
        case GL_FOG_START:
            s.m_fogStart = param;
            break;
        case GL_FOG_END:
            s.m_fogEnd = param;
            break;
        case GL_FOG_DENSITY:
            s.m_fogDensity = param;
            break;
        case GL_FOG_MODE:
            s.m_fogMode = static_cast<GLenum>(param);
            break;
        default:
            return;
    }

    Dirty();
}

void GlShimFogi(GLenum pname, GLint param) {
    if (pname == GL_FOG_MODE) {
        s_state.m_fogMode = param;
        Dirty();
    } else {
        GlShimFogf(pname, static_cast<GLfloat>(param));
    }
}

void GlShimFogfv(GLenum pname, const GLfloat* params) {
    if (pname == GL_FOG_COLOR) {
        Copy4(s_state.m_fogColor, params);
        Dirty();
    } else {
        GlShimFogf(pname, params[0]);
    }
}

void GlShimAlphaFunc(GLenum func, GLclampf ref) {
    s_state.m_alphaFunc = func;
    s_state.m_alphaRef = ref;
    Dirty();
}

void GlShimClipPlane(GLenum plane, const GLdouble* equation) {
    if (plane - GL_CLIP_PLANE0 >= 6) {
        return;
    }

    float p[4] = { static_cast<float>(equation[0]), static_cast<float>(equation[1]), static_cast<float>(equation[2]), static_cast<float>(equation[3]) };
    TransformPlane(p, s_state.m_clipPlanes[plane - GL_CLIP_PLANE0]);
    Dirty();
}

void GlShimColor4fv(const GLfloat* v) {
    Copy4(s_state.m_color, v);
}

void GlShimMatrixMode(GLenum mode) {
    s_state.m_matrixMode = mode;
}

void GlShimLoadMatrixf(const GLfloat* m) {
    State& s = s_state;
    Mat4* target = s.m_matrixMode == GL_PROJECTION ? &s.m_projection : (s.m_matrixMode == GL_TEXTURE ? &ActiveUnit().m_matrix : &s.m_modelView);
    memcpy(target->m, m, sizeof(float) * 16);
    Dirty();
}

void GlShimLoadIdentity() {
    GlShimLoadMatrixf(kIdentity.m);
}

static ArrayState* ClientArray(GLenum array) {
    State& s = s_state;

    switch (array) {
        case GL_VERTEX_ARRAY:
            return &s.m_vertexArray;
        case GL_NORMAL_ARRAY:
            return &s.m_normalArray;
        case GL_COLOR_ARRAY:
            return &s.m_colorArray;
        case GL_TEXTURE_COORD_ARRAY:
            return &s.m_units[s.m_clientUnit].m_texCoordArray;
        default:
            return nullptr;
    }
}

void GlShimEnableClientState(GLenum array) {
    if (ArrayState* a = ClientArray(array)) {
        a->m_enabled = true;
    }
}

void GlShimDisableClientState(GLenum array) {
    if (ArrayState* a = ClientArray(array)) {
        a->m_enabled = false;
    }
}

static void SetPointer(ArrayState& a, GLint size, GLenum type, GLsizei stride, const GLvoid* pointer) {
    a.m_size = size;
    a.m_type = type;
    a.m_stride = stride;
    a.m_pointer = pointer;
    a.m_buffer = s_state.m_arrayBuffer;
}

void GlShimVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer) {
    SetPointer(s_state.m_vertexArray, size, type, stride, pointer);
}

void GlShimNormalPointer(GLenum type, GLsizei stride, const GLvoid* pointer) {
    SetPointer(s_state.m_normalArray, 3, type, stride, pointer);
}

void GlShimColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer) {
    SetPointer(s_state.m_colorArray, size, type, stride, pointer);
}

void GlShimTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer) {
    SetPointer(s_state.m_units[s_state.m_clientUnit].m_texCoordArray, size, type, stride, pointer);
}

void GlShimDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices) {
    PrepareDraw();
    glDrawElements(mode, count, type, indices);
    CheckDrawError("drawElements", mode, count);
}

void GlShimDrawArrays(GLenum mode, GLint first, GLsizei count) {
    PrepareDraw();
    glDrawArrays(mode, first, count);
    CheckDrawError("drawArrays", mode, count);
}

void GlShimBindTexture(GLenum target, GLuint texture) {
    UnitState& u = ActiveUnit();

    if (target == GL_TEXTURE_2D) {
        u.m_bound2d = texture;
    } else if (target == SHIM_TEXTURE_CUBE_MAP) {
        u.m_boundCube = texture;
    } else {
        return;
    }

    glBindTexture(target, texture);
}

void GlShimDeleteTextures(GLsizei n, const GLuint* textures) {
    for (GLsizei i = 0; i < n; i++) {
        s_generateMipmap.erase(textures[i]);
    }

    glDeleteTextures(n, textures);
}

void GlShimTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid* pixels) {
    Upload up = Convert(width, height, format, type, pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(target, level, up.m_internal, width, height, 0, up.m_format, up.m_type, up.m_hasData ? up.m_data.data() : nullptr);
    RecordTexture(target, level, width, height, up.m_internal);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, s_state.m_unpackRowLength);

    if (up.m_hasData) {
        MaybeGenerateMipmap(target, level);
    }
}

void GlShimTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid* pixels) {
    Upload up = Convert(width, height, format, type, pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D(target, level, x, y, width, height, up.m_format, up.m_type, up.m_data.data());
    glPixelStorei(GL_UNPACK_ROW_LENGTH, s_state.m_unpackRowLength);
    MaybeGenerateMipmap(target, level);
}

// The default framebuffer has no alpha channel, so a copy from it can only target an RGB texture.
void GlShimCopyTexImage2D(GLenum target, GLint level, GLenum internalFormat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border) {
    glCopyTexImage2D(target, level, s_state.m_framebuffer ? SHIM_RGBA8 : SHIM_RGB8, x, y, width, height, 0);
    RecordTexture(target, level, width, height, s_state.m_framebuffer ? SHIM_RGBA8 : SHIM_RGB8);
}

void GlShimReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid* pixels) {
    if (format == GL_DEPTH_COMPONENT) {
        // ponytail: WebGL2 cannot read depth back; report "far" everywhere.
        float* out = static_cast<float*>(pixels);

        for (GLsizei i = 0; i < width * height; i++) {
            out[i] = 1.0f;
        }

        return;
    }

    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    if (format == GL_BGRA) {
        uint8_t* p = static_cast<uint8_t*>(pixels);

        for (GLsizei i = 0; i < width * height; i++) {
            uint8_t r = p[i * 4];
            p[i * 4] = p[i * 4 + 2];
            p[i * 4 + 2] = r;
        }
    }
}

void GlShimPixelStorei(GLenum pname, GLint param) {
    if (pname == GL_UNPACK_ROW_LENGTH) {
        s_state.m_unpackRowLength = param;
    }

    glPixelStorei(pname, param);
}

void GlShimPolygonMode(GLenum face, GLenum mode) {
}

void GlShimPointSize(GLfloat size) {
    s_state.m_pointSize = size;
    Dirty();
}

void GlShimDepthRange(GLclampd nearVal, GLclampd farVal) {
    glDepthRangef(static_cast<GLfloat>(nearVal), static_cast<GLfloat>(farVal));
}

// Presenting happens when the frame callback returns; a glFinish here would only stall.
void GlShimFinish() {
}

void GlShimHint(GLenum target, GLenum mode) {
}

void GlShimGetIntegerv(GLenum pname, GLint* params) {
    switch (pname) {
        case GL_MAX_TEXTURE_UNITS_ARB:
            *params = kUnits;
            return;
        case GL_MAX_CLIP_PLANES:
            *params = 6;
            return;
        case GL_MAX_RECTANGLE_TEXTURE_SIZE_ARB:
            *params = 0;
            return;
        case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT:
            if (!s_hasAnisotropy) {
                *params = 1;
                return;
            }
            break;
        case GL_PROGRAM_ERROR_POSITION_ARB:
            *params = s_arbErrorPosition;
            return;
        default:
            break;
    }

    glGetIntegerv(pname, params);
}

void GlShimGetFloatv(GLenum pname, GLfloat* params) {
    if (pname == GL_POINT_SIZE_MAX_ARB) {
        GLfloat range[2] = { 1.0f, 1.0f };
        glGetFloatv(SHIM_ALIASED_POINT_SIZE_RANGE, range);
        *params = range[1];
        return;
    }

    glGetFloatv(pname, params);
}

#endif
