#if defined(ARB_PROGRAM_TEST)

// Offline check for the ARB translator (not part of any build target's code: the guard keeps it empty).
//   g++ -std=c++17 -DARB_PROGRAM_TEST -I. Engine/Source/Gx/CGxDeviceOpenGL/Web/ArbProgram*.cpp -o arbtest
//   arbtest <outdir> <list>   (list: one path per line to a file holding one ARB program, e.g. split from a .bls)
// Writes <outdir>/<n>.vert|.frag for glslangValidator and prints every program that fails to translate.
// With no files it runs the built-in self-check.

#include "Engine/Source/Gx/CGxDeviceOpenGL/Web/ArbProgram.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

static void SelfCheck() {
    const char* vp =
        "!!ARBvp1.0\n# comment\nPARAM c[4] = { { 0, 1 }, program.env[1..3] };\nTEMP R0;\nADDRESS A0;\n"
        "ARL A0.x, vertex.attrib[2].x;\nDP4 R0.x, vertex.attrib[0], c[A0.x + 1];\nMOV result.position, -R0.xxyw;\n"
        "MOV_SAT result.texcoord[1].xy, c[0];\nEND\n";
    ArbProgram p;
    assert(ArbParse(vp, strlen(vp), p));
    assert(p.m_slots.size() == 4 && p.m_slots[0].m_kind == ArbSlot_Literal && p.m_slots[3].m_kind == ArbSlot_Env && p.m_slots[3].m_index == 3);
    assert(p.m_attribMask == 5);
    assert(p.m_body.find("uVC[clamp(a_A0.x + 1, 0, 3)]") != std::string::npos);

    const char* fp = "!!ARBfp1.0\nOPTION ARB_fog_linear;\nTEMP R0;\nTEX R0, fragment.texcoord[3], texture[2], CUBE;\nKIL -R0;\nCMP result.color, R0, 0.5, {1, 2};\nEND";
    assert(ArbParse(fp, strlen(fp), p));
    assert(p.m_texcoordMask == 8 && p.m_samplers[2] == ArbSampler_Cube && p.m_fog == 1);

    const char* bad = "!!ARBfp1.0\nTEMP R0;\nFOO R0, R0;\nEND";
    assert(!ArbParse(bad, strlen(bad), p) && p.m_errorPosition == 19);
    puts("self-check ok");
}

int main(int argc, char** argv) {
    if (argc < 3) {
        SelfCheck();
        return 0;
    }

    std::vector<std::string> paths;
    char line[1024];
    FILE* list = fopen(argv[2], "rb");

    while (list && fgets(line, sizeof(line), list)) {
        std::string path = line;

        while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) {
            path.pop_back();
        }

        paths.push_back(path);
    }

    int failed = 0;

    for (size_t i = 0; i < paths.size(); i++) {
        const char* path = paths[i].c_str();
        FILE* f = fopen(path, "rb");

        if (!f) {
            continue;
        }

        std::string text;
        char buffer[65536];
        size_t n;

        while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
            text.append(buffer, n);
        }

        fclose(f);

        ArbProgram p;

        if (!ArbParse(text.data(), text.size(), p)) {
            printf("FAIL %s @%d: %s\n", path, p.m_errorPosition, p.m_error.c_str());
            failed++;
            continue;
        }

        std::string out = std::string(argv[1]) + "/" + std::to_string(i) + (p.m_vertex ? ".vert" : ".frag");
        std::string glsl = p.m_vertex ? ArbVertexShader(p) : ArbFragmentShader(p, false, "uniform float uAlphaRef;\n", "    if (!(oColor.a >= uAlphaRef)) discard;\n");
        FILE* o = fopen(out.c_str(), "wb");
        fprintf(o, "%s// %s\n", glsl.c_str(), path);
        fclose(o);
    }

    printf("%d programs, %d failed\n", static_cast<int>(paths.size()), failed);
    return failed != 0;
}

#endif
