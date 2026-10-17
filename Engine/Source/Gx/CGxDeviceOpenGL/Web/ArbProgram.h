#ifndef ENGINE_SOURCE_GX_CGXDEVICEOPENGL_WEB_ARBPROGRAM_H
#define ENGINE_SOURCE_GX_CGXDEVICEOPENGL_WEB_ARBPROGRAM_H

// ARB_vertex_program / ARB_fragment_program assembly (the arbvp1/arbfp1 .bls shaders) translated to GLSL ES 3.00.
//
// Every PARAM element and every direct program.env/local/state reference becomes one vec4 "slot" in a per-program
// uniform array (uVC in the vertex stage, uFC in the fragment stage); GlShim fills the array from the env/local
// parameters and the GL state before a draw. The stages talk through the same varyings as GlShim's fixed-function
// programs (vColor, vSpec, vFog, vTex0..7), so either stage can be paired with fixed-function processing.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum ArbSlotKind : uint8_t {
    ArbSlot_Literal,
    ArbSlot_Env,
    ArbSlot_Local,
    ArbSlot_Matrix,
    ArbSlot_FogColor,
    ArbSlot_FogParams, // (density, start, end, 1 / (end - start))
};

enum ArbMatrix : uint8_t {
    ArbMatrix_ModelView,
    ArbMatrix_Projection,
    ArbMatrix_Mvp,
    ArbMatrix_Texture,
};

struct ArbSlot {
    ArbSlotKind m_kind;
    uint8_t m_matrix;   // ArbMatrix
    uint8_t m_unit;     // texture matrix unit
    uint8_t m_modifier; // 0 none, 1 inverse, 2 transpose, 3 invtrans
    uint8_t m_row;
    uint16_t m_index;   // env / local index
    float m_value[4];   // literal
};

enum ArbSampler : uint8_t {
    ArbSampler_None,
    ArbSampler_2D,
    ArbSampler_Cube,
    ArbSampler_Rect,
    ArbSampler_Shadow2D,
};

struct ArbProgram {
    bool m_vertex = false;
    bool m_valid = false;
    int32_t m_errorPosition = -1;
    std::string m_error;
    std::string m_decls;          // stage uniforms and samplers
    std::string m_body;           // statements of main()
    std::vector<ArbSlot> m_slots; // contents of uVC / uFC
    uint32_t m_attribMask = 0;    // vertex: generic attributes read (location = attribute index)
    uint32_t m_texcoordMask = 0;  // fragment: fragment.texcoord[n] read
    ArbSampler m_samplers[16] = {};
    uint8_t m_fog = 0;            // fragment fog option: 0 none, 1 linear, 2 exp, 3 exp2
    bool m_positionInvariant = false;
    bool m_writesDepth = false;
    bool m_usesState = false;     // slots depend on matrices / fog state
};

// Parses and translates one program. On failure m_valid is false and m_errorPosition / m_error say why.
bool ArbParse(const char* text, size_t length, ArbProgram& program);

// Complete vertex shader for a vertex program.
std::string ArbVertexShader(const ArbProgram& vp);

// Complete fragment shader for a fragment program. extraDecls / epilogue come from the fixed-function per-fragment
// state that still applies after a fragment program (alpha test, clip planes); the epilogue may read and discard on
// the final color, `oColor`. With pointSprite, fragment.texcoord reads gl_PointCoord (COORD_REPLACE on every unit).
std::string ArbFragmentShader(const ArbProgram& fp, bool pointSprite, const std::string& extraDecls, const std::string& epilogue);

#endif
