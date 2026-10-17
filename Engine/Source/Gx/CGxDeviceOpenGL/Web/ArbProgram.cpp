#if defined(WOWDECOMP_PLATFORM_WEB) || defined(ARB_PROGRAM_TEST)

// ARB assembly -> GLSL ES 3.00 (see ArbProgram.h). Statements are translated one by one: registers become vec4 locals,
// each instruction becomes `dst.mask = expr.mask;`. Covers the full ARBvp1.0 / ARBfp1.0 instruction sets, PARAM /
// TEMP / ADDRESS / ATTRIB / OUTPUT / ALIAS declarations, relative addressing, the program.env/local and
// state.matrix / state.fog bindings, and the fog / position-invariant / shadow options. Other state.* bindings
// (lights, materials, clip planes, texgen) are reported as errors; the game's shaders use none.

#include "Engine/Source/Gx/CGxDeviceOpenGL/Web/ArbProgram.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace {

struct Symbol {
    enum Kind { Temp, Address, Param, Alias };
    Kind m_kind;
    std::string m_glsl; // Temp / Address / Alias (ATTRIB, OUTPUT): GLSL expression
    int m_slot;         // Param: first slot
    int m_size;         // Param: elements
    bool m_array;
};

std::string Trim(const std::string& s) {
    size_t a = 0;
    size_t b = s.size();

    while (a < b && isspace(static_cast<unsigned char>(s[a]))) {
        a++;
    }

    while (b > a && isspace(static_cast<unsigned char>(s[b - 1]))) {
        b--;
    }

    return s.substr(a, b - a);
}

std::string StripSpace(const std::string& s) {
    std::string r;

    for (char c : s) {
        if (!isspace(static_cast<unsigned char>(c))) {
            r += c;
        }
    }

    return r;
}

// Splits on commas outside [] and {}.
std::vector<std::string> SplitTop(const std::string& s) {
    std::vector<std::string> parts;
    int depth = 0;
    size_t start = 0;

    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '[' || s[i] == '{') {
            depth++;
        } else if (s[i] == ']' || s[i] == '}') {
            depth--;
        } else if (s[i] == ',' && depth == 0) {
            parts.push_back(Trim(s.substr(start, i - start)));
            start = i + 1;
        }
    }

    parts.push_back(Trim(s.substr(start)));
    return parts;
}

std::string Float(double v) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.9g", v);
    std::string s = buffer;

    if (s.find_first_of(".eE") == std::string::npos) {
        s += ".0";
    }

    return s;
}

bool ParseNumber(const std::string& s, double& out) {
    if (s.empty()) {
        return false;
    }

    char* end = nullptr;
    out = strtod(s.c_str(), &end);
    return end && *end == '\0';
}

bool ParseInt(const std::string& s, int& out) {
    if (s.empty()) {
        return false;
    }

    for (char c : s) {
        if (!isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }

    out = atoi(s.c_str());
    return true;
}

// "{a, b, c, d}" with missing components defaulting to (0, 0, 0, 1); a bare number replicates.
bool ParseLiteral(const std::string& s, float out[4]) {
    double v;

    if (s.size() >= 2 && s.front() == '{' && s.back() == '}') {
        std::vector<std::string> parts = SplitTop(s.substr(1, s.size() - 2));

        if (parts.empty() || parts.size() > 4) {
            return false;
        }

        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;

        for (size_t i = 0; i < parts.size(); i++) {
            if (!ParseNumber(parts[i], v)) {
                return false;
            }

            out[i] = static_cast<float>(v);
        }

        return true;
    }

    if (!ParseNumber(s, v)) {
        return false;
    }

    out[0] = out[1] = out[2] = out[3] = static_cast<float>(v);
    return true;
}

std::string LiteralExpr(const float v[4]) {
    return "vec4(" + Float(v[0]) + ", " + Float(v[1]) + ", " + Float(v[2]) + ", " + Float(v[3]) + ")";
}

// "[a]" or "[a..b]" at the start of s; returns the characters consumed.
size_t ParseRange(const std::string& s, int& first, int& last) {
    if (s.empty() || s[0] != '[') {
        return 0;
    }

    size_t close = s.find(']');

    if (close == std::string::npos) {
        return 0;
    }

    std::string inner = s.substr(1, close - 1);
    size_t dots = inner.find("..");

    if (dots == std::string::npos) {
        if (!ParseInt(inner, first)) {
            return 0;
        }

        last = first;
    } else if (!ParseInt(inner.substr(0, dots), first) || !ParseInt(inner.substr(dots + 2), last) || last < first) {
        return 0;
    }

    return close + 1;
}

struct Translator {
    ArbProgram& m_program;
    bool m_vertex;
    std::map<std::string, Symbol> m_symbols;
    std::map<std::string, int> m_directSlots;
    std::string m_error;

    explicit Translator(ArbProgram& program) : m_program(program), m_vertex(program.m_vertex) {
    }

    const char* Uniform() const {
        return m_vertex ? "uVC" : "uFC";
    }

    bool Fail(const std::string& message) {
        if (m_error.empty()) {
            m_error = message;
        }

        return false;
    }

    // Expands one binding item (whitespace removed) into slots.
    bool Binding(const std::string& s, std::vector<ArbSlot>& out) {
        ArbSlot slot;
        memset(&slot, 0, sizeof(slot));
        int first;
        int last;

        if (!s.empty() && (s[0] == '{' || s[0] == '-' || s[0] == '+' || s[0] == '.' || isdigit(static_cast<unsigned char>(s[0])))) {
            slot.m_kind = ArbSlot_Literal;

            if (!ParseLiteral(s, slot.m_value)) {
                return Fail("bad constant " + s);
            }

            out.push_back(slot);
            return true;
        }

        if (s.compare(0, 11, "program.env") == 0 || s.compare(0, 13, "program.local") == 0) {
            bool env = s[8] == 'e';
            std::string rest = s.substr(env ? 11 : 13);
            size_t used = ParseRange(rest, first, last);

            if (!used || used != rest.size() || last > 4095) {
                return Fail("bad program parameter " + s);
            }

            for (int i = first; i <= last; i++) {
                slot.m_kind = env ? ArbSlot_Env : ArbSlot_Local;
                slot.m_index = static_cast<uint16_t>(i);
                out.push_back(slot);
            }

            return true;
        }

        if (s == "state.fog.color" || s == "state.fog.params") {
            slot.m_kind = s == "state.fog.color" ? ArbSlot_FogColor : ArbSlot_FogParams;
            m_program.m_usesState = true;
            out.push_back(slot);
            return true;
        }

        if (s.compare(0, 13, "state.matrix.") == 0) {
            std::string rest = s.substr(13);
            slot.m_kind = ArbSlot_Matrix;

            static const struct {
                const char* m_name;
                ArbMatrix m_matrix;
            } kNames[] = { { "modelview", ArbMatrix_ModelView }, { "projection", ArbMatrix_Projection }, { "mvp", ArbMatrix_Mvp }, { "texture", ArbMatrix_Texture } };

            bool found = false;

            for (const auto& name : kNames) {
                size_t length = strlen(name.m_name);

                if (rest.compare(0, length, name.m_name) == 0) {
                    slot.m_matrix = name.m_matrix;
                    rest = rest.substr(length);
                    found = true;
                    break;
                }
            }

            if (!found) {
                return Fail("unsupported matrix " + s);
            }

            if (!rest.empty() && rest[0] == '[') {
                size_t used = ParseRange(rest, first, last);

                if (!used || first != last || first > 7 || (slot.m_matrix != ArbMatrix_Texture && first != 0)) {
                    return Fail("bad matrix index " + s);
                }

                slot.m_unit = static_cast<uint8_t>(first);
                rest = rest.substr(used);
            }

            static const char* kModifiers[4] = { "", ".inverse", ".transpose", ".invtrans" };

            for (int i = 3; i > 0; i--) {
                size_t length = strlen(kModifiers[i]);

                if (rest.compare(0, length, kModifiers[i]) == 0) {
                    slot.m_modifier = static_cast<uint8_t>(i);
                    rest = rest.substr(length);
                    break;
                }
            }

            first = 0;
            last = 3;

            if (rest.compare(0, 4, ".row") == 0) {
                size_t used = ParseRange(rest.substr(4), first, last);

                if (!used || used + 4 != rest.size() || last > 3) {
                    return Fail("bad matrix row " + s);
                }
            } else if (!rest.empty()) {
                return Fail("bad matrix binding " + s);
            }

            m_program.m_usesState = true;

            for (int i = first; i <= last; i++) {
                slot.m_row = static_cast<uint8_t>(i);
                out.push_back(slot);
            }

            return true;
        }

        return Fail("unsupported binding " + s);
    }

    // Slot for a binding referenced directly by an instruction (deduplicated).
    bool DirectSlot(const std::string& s, std::string& out) {
        auto it = m_directSlots.find(s);
        int slot;

        if (it != m_directSlots.end()) {
            slot = it->second;
        } else {
            std::vector<ArbSlot> slots;

            if (!Binding(s, slots)) {
                return false;
            }

            if (slots.size() != 1) {
                return Fail("binding is not a single vector: " + s);
            }

            slot = static_cast<int>(m_program.m_slots.size());
            m_program.m_slots.push_back(slots[0]);
            m_directSlots[s] = slot;
        }

        out = std::string(Uniform()) + "[" + std::to_string(slot) + "]";
        return true;
    }

    bool VertexInput(const std::string& s, std::string& out) {
        int index = -1;
        int first;
        int last;

        if (s.compare(0, 13, "vertex.attrib") == 0) {
            if (ParseRange(s.substr(13), first, last) != s.size() - 13 || first != last || first > 15) {
                return Fail("bad attribute " + s);
            }

            index = first;
        } else if (s == "vertex.position") {
            index = 0;
        } else if (s == "vertex.weight" || s == "vertex.weight[0]") {
            index = 1;
        } else if (s == "vertex.normal") {
            index = 2;
        } else if (s == "vertex.color" || s == "vertex.color.primary") {
            index = 3;
        } else if (s == "vertex.color.secondary") {
            index = 4;
        } else if (s == "vertex.fogcoord") {
            index = 5;
        } else if (s == "vertex.texcoord") {
            index = 8;
        } else if (s.compare(0, 15, "vertex.texcoord") == 0) {
            if (ParseRange(s.substr(15), first, last) != s.size() - 15 || first != last || first > 7) {
                return Fail("bad texcoord " + s);
            }

            index = 8 + first;
        } else {
            return Fail("unsupported vertex input " + s);
        }

        m_program.m_attribMask |= 1u << index;
        out = "a" + std::to_string(index);
        return true;
    }

    bool FragmentInput(const std::string& s, std::string& out) {
        int first;
        int last;

        if (s == "fragment.color" || s == "fragment.color.primary") {
            out = "fCol0";
        } else if (s == "fragment.color.secondary") {
            out = "fCol1";
        } else if (s == "fragment.fogcoord") {
            out = "fFog";
        } else if (s == "fragment.position") {
            out = "gl_FragCoord";
        } else if (s == "fragment.texcoord") {
            m_program.m_texcoordMask |= 1;
            out = "fTex0";
        } else if (s.compare(0, 17, "fragment.texcoord") == 0) {
            if (ParseRange(s.substr(17), first, last) != s.size() - 17 || first != last || first > 7) {
                return Fail("bad texcoord " + s);
            }

            m_program.m_texcoordMask |= 1u << first;
            out = "fTex" + std::to_string(first);
        } else {
            return Fail("unsupported fragment input " + s);
        }

        return true;
    }

    bool Output(const std::string& s, std::string& out) {
        int first;
        int last;

        if (!m_vertex) {
            if (s == "result.color") {
                out = "oColor";
            } else if (s == "result.depth") {
                m_program.m_writesDepth = true;
                out = "oDepth";
            } else {
                return Fail("unsupported output " + s);
            }

            return true;
        }

        if (s == "result.position") {
            if (m_program.m_positionInvariant) {
                return Fail("result.position written by a position-invariant program");
            }

            out = "oPos";
        } else if (s == "result.color" || s == "result.color.primary" || s == "result.color.front" || s == "result.color.front.primary") {
            out = "oCol0";
        } else if (s == "result.color.secondary" || s == "result.color.front.secondary") {
            out = "oCol1";
        } else if (s.compare(0, 17, "result.color.back") == 0) {
            out = "oBack";
        } else if (s == "result.fogcoord") {
            out = "oFog";
        } else if (s == "result.pointsize") {
            out = "oPts";
        } else if (s == "result.texcoord") {
            out = "oTex0";
        } else if (s.compare(0, 15, "result.texcoord") == 0) {
            if (ParseRange(s.substr(15), first, last) != s.size() - 15 || first != last || first > 7) {
                return Fail("bad texcoord " + s);
            }

            out = "oTex" + std::to_string(first);
        } else {
            return Fail("unsupported output " + s);
        }

        return true;
    }

    // Splits "base.swizzle"; swizzle is empty when the last component is not one.
    void SplitSwizzle(const std::string& s, std::string& base, std::string& swizzle) {
        int depth = 0;
        size_t dot = std::string::npos;

        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '[') {
                depth++;
            } else if (s[i] == ']') {
                depth--;
            } else if (s[i] == '.' && depth == 0) {
                dot = i;
            }
        }

        base = s;
        swizzle.clear();

        if (dot == std::string::npos || dot == 0) {
            return;
        }

        std::string candidate = s.substr(dot + 1);

        if (candidate.empty() || candidate.size() > 4) {
            return;
        }

        bool xyzw = candidate.find_first_not_of("xyzw") == std::string::npos;
        bool rgba = !m_vertex && candidate.find_first_not_of("rgba") == std::string::npos;

        if (!xyzw && !rgba) {
            return;
        }

        base = s.substr(0, dot);

        for (char c : candidate) {
            swizzle += "xyzw"[strchr(xyzw ? "xyzw" : "rgba", c) - (xyzw ? "xyzw" : "rgba")];
        }
    }

    // Readable register / binding (no swizzle) as a GLSL vec4 expression.
    bool Value(const std::string& s, std::string& out) {
        if (s.compare(0, 7, "vertex.") == 0) {
            return m_vertex ? VertexInput(s, out) : Fail("vertex input in a fragment program");
        }

        if (s.compare(0, 9, "fragment.") == 0) {
            return !m_vertex ? FragmentInput(s, out) : Fail("fragment input in a vertex program");
        }

        if (s.compare(0, 8, "program.") == 0 || s.compare(0, 6, "state.") == 0) {
            return DirectSlot(s, out);
        }

        size_t bracket = s.find('[');
        std::string name = s.substr(0, bracket);
        auto it = m_symbols.find(name);

        if (it == m_symbols.end()) {
            return Fail("unknown name " + name);
        }

        const Symbol& symbol = it->second;

        if (symbol.m_kind != Symbol::Param) {
            if (bracket != std::string::npos || symbol.m_kind == Symbol::Address) {
                return Fail("bad use of " + name);
            }

            out = symbol.m_glsl;
            return true;
        }

        if (bracket == std::string::npos) {
            if (symbol.m_array) {
                return Fail("array used without index: " + name);
            }

            out = std::string(Uniform()) + "[" + std::to_string(symbol.m_slot) + "]";
            return true;
        }

        if (!symbol.m_array || s.back() != ']') {
            return Fail("bad index on " + name);
        }

        std::string index = s.substr(bracket + 1, s.size() - bracket - 2);
        int constant;

        if (ParseInt(index, constant)) {
            if (constant >= symbol.m_size) {
                return Fail("index out of range: " + s);
            }

            out = std::string(Uniform()) + "[" + std::to_string(symbol.m_slot + constant) + "]";
            return true;
        }

        // Relative: A0.x, A0.x+n, A0.x-n.
        size_t dot = index.find(".x");
        auto address = m_symbols.find(index.substr(0, dot));

        if (!m_vertex || dot == std::string::npos || address == m_symbols.end() || address->second.m_kind != Symbol::Address) {
            return Fail("bad relative index " + s);
        }

        std::string rest = index.substr(dot + 2);
        int offset = 0;

        if (!rest.empty()) {
            if ((rest[0] != '+' && rest[0] != '-') || !ParseInt(rest.substr(1), offset)) {
                return Fail("bad relative offset " + s);
            }

            offset = rest[0] == '-' ? -offset : offset;
        }

        int first = symbol.m_slot;
        int lastSlot = symbol.m_slot + symbol.m_size - 1;
        out = std::string(Uniform()) + "[clamp(" + address->second.m_glsl + ".x + " + std::to_string(first + offset) + ", " + std::to_string(first) + ", " + std::to_string(lastSlot) + ")]";
        return true;
    }

    bool Source(const std::string& text, std::string& out) {
        std::string s = text;
        bool negate = false;

        if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
            negate = s[0] == '-';
            s = s.substr(1);
        }

        float literal[4];

        if (!s.empty() && (s[0] == '{' || s[0] == '.' || isdigit(static_cast<unsigned char>(s[0])))) {
            if (!ParseLiteral(s, literal)) {
                return Fail("bad constant " + s);
            }

            out = LiteralExpr(literal);
        } else {
            std::string base;
            std::string swizzle;
            SplitSwizzle(s, base, swizzle);

            if (!Value(base, out)) {
                return false;
            }

            if (swizzle.size() == 1) {
                out += "." + std::string(4, swizzle[0]);
            } else if (swizzle.size() == 4) {
                out += "." + swizzle;
            } else if (!swizzle.empty()) {
                return Fail("bad source swizzle " + text);
            }
        }

        out = negate ? "(-" + out + ")" : "(" + out + ")";
        return true;
    }

    bool Destination(const std::string& s, std::string& lvalue, std::string& mask) {
        std::string base;
        SplitSwizzle(s, base, mask);

        // A write mask is an ordered subset of xyzw.
        if (!mask.empty()) {
            const char* order = "xyzw";
            size_t position = 0;

            for (char c : mask) {
                const char* found = strchr(order + position, c);

                if (!found) {
                    return Fail("bad write mask " + s);
                }

                position = found - order + 1;
            }
        }

        if (mask == "xyzw") {
            mask.clear();
        }

        if (base.compare(0, 7, "result.") == 0) {
            return Output(base, lvalue);
        }

        auto it = m_symbols.find(base);

        if (it == m_symbols.end() || it->second.m_kind == Symbol::Param) {
            return Fail("bad destination " + s);
        }

        lvalue = it->second.m_glsl;
        return true;
    }

    std::string Identifier(const std::string& name) {
        std::string r;

        for (char c : name) {
            r += isalnum(static_cast<unsigned char>(c)) ? c : '_';
        }

        return r;
    }

    bool Declare(const std::string& name, const Symbol& symbol) {
        if (name.empty() || !(isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_' || name[0] == '$')) {
            return Fail("bad name " + name);
        }

        if (!m_symbols.emplace(name, symbol).second) {
            return Fail("redeclared " + name);
        }

        return true;
    }

    bool Param(const std::string& text) {
        size_t equals = text.find('=');

        if (equals == std::string::npos) {
            return Fail("PARAM without binding");
        }

        std::string left = StripSpace(text.substr(0, equals));
        std::string right = StripSpace(text.substr(equals + 1));
        Symbol symbol = { Symbol::Param, "", static_cast<int>(m_program.m_slots.size()), 0, false };
        int declared = -1;
        size_t bracket = left.find('[');

        if (bracket != std::string::npos) {
            symbol.m_array = true;
            std::string size = left.substr(bracket + 1, left.size() - bracket - 2);

            if (left.back() != ']' || (!size.empty() && !ParseInt(size, declared))) {
                return Fail("bad PARAM size " + left);
            }

            left = left.substr(0, bracket);
        }

        std::vector<ArbSlot> slots;

        if (symbol.m_array) {
            if (right.size() < 2 || right.front() != '{' || right.back() != '}') {
                return Fail("PARAM array needs { }");
            }

            for (const std::string& item : SplitTop(right.substr(1, right.size() - 2))) {
                if (!Binding(item, slots)) {
                    return false;
                }
            }

            // ponytail: a short initializer is padded with zeros instead of rejected.
            if (declared > static_cast<int>(slots.size())) {
                ArbSlot zero;
                memset(&zero, 0, sizeof(zero));
                slots.resize(declared, zero);
            }
        } else {
            if (!Binding(right, slots)) {
                return false;
            }

            if (slots.size() != 1) {
                return Fail("PARAM " + left + " is not a single vector");
            }
        }

        symbol.m_size = static_cast<int>(slots.size());
        m_program.m_slots.insert(m_program.m_slots.end(), slots.begin(), slots.end());
        return Declare(left, symbol);
    }

    bool Alias(const std::string& keyword, const std::string& text) {
        size_t equals = text.find('=');

        if (equals == std::string::npos) {
            return Fail(keyword + " without binding");
        }

        std::string name = StripSpace(text.substr(0, equals));
        std::string target = StripSpace(text.substr(equals + 1));

        if (keyword == "ALIAS") {
            auto it = m_symbols.find(target);

            if (it == m_symbols.end()) {
                return Fail("ALIAS of unknown " + target);
            }

            Symbol copy = it->second;
            return Declare(name, copy);
        }

        Symbol symbol = { Symbol::Alias, "", 0, 0, false };

        if (keyword == "OUTPUT") {
            if (!Output(target, symbol.m_glsl)) {
                return false;
            }
        } else if (!Value(target, symbol.m_glsl) || (target.compare(0, 7, "vertex.") && target.compare(0, 9, "fragment."))) {
            return Fail("bad ATTRIB " + target);
        }

        return Declare(name, symbol);
    }

    bool Option(const std::string& name) {
        if (name == "ARB_position_invariant" && m_vertex) {
            m_program.m_positionInvariant = true;
            m_program.m_attribMask |= 1;
        } else if (name == "ARB_fog_linear" && !m_vertex) {
            m_program.m_fog = 1;
        } else if (name == "ARB_fog_exp" && !m_vertex) {
            m_program.m_fog = 2;
        } else if (name == "ARB_fog_exp2" && !m_vertex) {
            m_program.m_fog = 3;
        } else if (name.compare(0, 19, "ARB_precision_hint_") && name != "ARB_fragment_program_shadow") {
            return Fail("unsupported option " + name);
        }

        return true;
    }

    bool Texture(const std::string& opcode, const std::vector<std::string>& operands, const std::string& coord, std::string& expr) {
        if (operands.size() != 4) {
            return Fail(opcode + " needs 4 operands");
        }

        int first = 0;
        int last = 0;
        const std::string& unitText = operands[2];

        if (unitText != "texture" && (unitText.compare(0, 7, "texture") || ParseRange(unitText.substr(7), first, last) != unitText.size() - 7 || first != last || first > 15)) {
            return Fail("bad texture unit " + unitText);
        }

        ArbSampler type;
        const std::string& target = operands[3];

        if (target == "2D") {
            type = ArbSampler_2D;
        } else if (target == "CUBE") {
            type = ArbSampler_Cube;
        } else if (target == "RECT") {
            type = ArbSampler_Rect;
        } else if (target == "SHADOW2D") {
            type = ArbSampler_Shadow2D;
        } else {
            return Fail("unsupported texture target " + target);
        }

        if (m_program.m_samplers[first] && m_program.m_samplers[first] != type) {
            return Fail("texture unit used with two targets");
        }

        m_program.m_samplers[first] = type;
        std::string sampler = "s" + std::to_string(first);

        if (opcode == "TXP") {
            switch (type) {
                case ArbSampler_Cube:
                    expr = "texture(" + sampler + ", " + coord + ".xyz)";
                    break;
                case ArbSampler_Rect:
                    expr = "texture(" + sampler + ", " + coord + ".xy / (" + coord + ".w * vec2(textureSize(" + sampler + ", 0))))";
                    break;
                case ArbSampler_Shadow2D:
                    expr = "vec4(vec3(textureProj(" + sampler + ", " + coord + ")), 1.0)";
                    break;
                default:
                    expr = "textureProj(" + sampler + ", " + coord + ")";
                    break;
            }
        } else if (opcode == "TXB") {
            switch (type) {
                case ArbSampler_Cube:
                    expr = "texture(" + sampler + ", " + coord + ".xyz, " + coord + ".w)";
                    break;
                case ArbSampler_Rect:
                    expr = "texture(" + sampler + ", " + coord + ".xy / vec2(textureSize(" + sampler + ", 0)))";
                    break;
                case ArbSampler_Shadow2D:
                    expr = "vec4(vec3(texture(" + sampler + ", " + coord + ".xyz, " + coord + ".w)), 1.0)";
                    break;
                default:
                    expr = "texture(" + sampler + ", " + coord + ".xy, " + coord + ".w)";
                    break;
            }
        } else {
            switch (type) {
                case ArbSampler_Cube:
                    expr = "texture(" + sampler + ", " + coord + ".xyz)";
                    break;
                case ArbSampler_Rect:
                    expr = "texture(" + sampler + ", " + coord + ".xy / vec2(textureSize(" + sampler + ", 0)))";
                    break;
                case ArbSampler_Shadow2D:
                    // DEPTH_TEXTURE_MODE is LUMINANCE: (r, r, r, 1).
                    expr = "vec4(vec3(texture(" + sampler + ", " + coord + ".xyz)), 1.0)";
                    break;
                default:
                    expr = "texture(" + sampler + ", " + coord + ".xy)";
                    break;
            }
        }

        return true;
    }

    bool Instruction(const std::string& opcodeText, const std::string& operandText) {
        std::string opcode = opcodeText;
        bool saturate = false;

        if (opcode.size() > 4 && opcode.compare(opcode.size() - 4, 4, "_SAT") == 0) {
            saturate = true;
            opcode = opcode.substr(0, opcode.size() - 4);
        }

        std::vector<std::string> operands = SplitTop(operandText);

        for (std::string& operand : operands) {
            operand = StripSpace(operand);
        }

        std::string& body = m_program.m_body;

        if (opcode == "KIL") {
            std::string a;

            if (m_vertex || operands.size() != 1 || !Source(operands[0], a)) {
                return Fail("bad KIL");
            }

            body += "    if (any(lessThan(" + a + ", vec4(0.0)))) discard;\n";
            return true;
        }

        if (operands.size() < 2) {
            return Fail(opcode + " needs operands");
        }

        if (opcode == "ARL") {
            auto it = m_symbols.find(operands[0].substr(0, operands[0].find('.')));
            std::string a;

            if (!m_vertex || it == m_symbols.end() || it->second.m_kind != Symbol::Address || operands.size() != 2 || !Source(operands[1], a)) {
                return Fail("bad ARL");
            }

            body += "    " + it->second.m_glsl + ".x = int(floor(" + a + ".x));\n";
            return true;
        }

        std::string dst;
        std::string mask;

        if (!Destination(operands[0], dst, mask)) {
            return false;
        }

        std::string expr;
        std::vector<std::string> src(operands.size() - 1);

        if (opcode == "SWZ") {
            if (operands.size() != 6) {
                return Fail("bad SWZ");
            }

            std::string reg = operands[1];
            bool negate = !reg.empty() && reg[0] == '-';

            if (!Source(negate ? reg.substr(1) : reg, src[0])) {
                return false;
            }

            expr = "vec4(";

            for (int i = 0; i < 4; i++) {
                std::string c = operands[2 + i];
                bool neg = !c.empty() && c[0] == '-';

                if (neg || (!c.empty() && c[0] == '+')) {
                    c = c.substr(1);
                }

                std::string term;

                if (c == "0" || c == "1") {
                    term = c + ".0";
                } else if (c.size() == 1 && strchr(m_vertex ? "xyzw" : "xyzwrgba", c[0])) {
                    term = src[0] + "." + std::string(1, "xyzwxyzw"[strchr("xyzwrgba", c[0]) - "xyzwrgba"]);
                } else {
                    return Fail("bad SWZ component " + c);
                }

                expr += std::string(i ? ", " : "") + (neg != negate ? "-" : "") + term;
            }

            expr += ")";
        } else if (opcode == "TEX" || opcode == "TXP" || opcode == "TXB") {
            if (m_vertex || !Source(operands[1], src[0]) || !Texture(opcode, operands, src[0], expr)) {
                return Fail("bad " + opcode);
            }
        } else {
            for (size_t i = 0; i < src.size(); i++) {
                if (!Source(operands[i + 1], src[i])) {
                    return false;
                }
            }

            static const struct {
                const char* m_name;
                int m_sources;
                bool m_fragmentOnly;
            } kOps[] = {
                { "ABS", 1, false }, { "ADD", 2, false }, { "CMP", 3, true }, { "COS", 1, true }, { "DP3", 2, false }, { "DP4", 2, false }, { "DPH", 2, false },
                { "DST", 2, false }, { "EX2", 1, false }, { "EXP", 1, false }, { "FLR", 1, false }, { "FRC", 1, false }, { "LG2", 1, false }, { "LIT", 1, false },
                { "LOG", 1, false }, { "LRP", 3, true }, { "MAD", 3, false }, { "MAX", 2, false }, { "MIN", 2, false }, { "MOV", 1, false }, { "MUL", 2, false },
                { "POW", 2, false }, { "RCP", 1, false }, { "RSQ", 1, false }, { "SCS", 1, true }, { "SGE", 2, false }, { "SIN", 1, true }, { "SLT", 2, false },
                { "SUB", 2, false }, { "XPD", 2, false },
            };

            bool known = false;

            for (const auto& op : kOps) {
                if (opcode == op.m_name) {
                    if (static_cast<int>(src.size()) != op.m_sources || (op.m_fragmentOnly && m_vertex)) {
                        return Fail("bad operand count for " + opcode);
                    }

                    known = true;
                    break;
                }
            }

            if (!known) {
                return Fail("unknown instruction " + opcode);
            }

            const std::string a = src[0];
            const std::string b = src.size() > 1 ? src[1] : "";
            const std::string c = src.size() > 2 ? src[2] : "";

            if (opcode == "ABS") expr = "abs(" + a + ")";
            else if (opcode == "ADD") expr = a + " + " + b;
            else if (opcode == "SUB") expr = a + " - " + b;
            else if (opcode == "MUL") expr = a + " * " + b;
            else if (opcode == "MAD") expr = a + " * " + b + " + " + c;
            else if (opcode == "MAX") expr = "max(" + a + ", " + b + ")";
            else if (opcode == "MIN") expr = "min(" + a + ", " + b + ")";
            else if (opcode == "MOV") expr = a;
            else if (opcode == "FLR") expr = "floor(" + a + ")";
            else if (opcode == "FRC") expr = "fract(" + a + ")";
            else if (opcode == "DP3") expr = "vec4(dot(" + a + ".xyz, " + b + ".xyz))";
            else if (opcode == "DP4") expr = "vec4(dot(" + a + ", " + b + "))";
            else if (opcode == "DPH") expr = "vec4(dot(" + a + ".xyz, " + b + ".xyz) + " + b + ".w)";
            else if (opcode == "DST") expr = "vec4(1.0, " + a + ".y * " + b + ".y, " + a + ".z, " + b + ".w)";
            else if (opcode == "EX2") expr = "vec4(exp2(" + a + ".x))";
            else if (opcode == "LG2") expr = "vec4(log2(abs(" + a + ".x)))";
            else if (opcode == "EXP") expr = "arbExp(" + a + ".x)";
            else if (opcode == "LOG") expr = "arbLog(" + a + ".x)";
            else if (opcode == "LIT") expr = "arbLit(" + a + ")";
            else if (opcode == "POW") expr = "vec4(pow(abs(" + a + ".x), " + b + ".x))";
            else if (opcode == "RCP") expr = "vec4(1.0 / " + a + ".x)";
            else if (opcode == "RSQ") expr = "vec4(inversesqrt(abs(" + a + ".x)))";
            else if (opcode == "SGE") expr = "vec4(greaterThanEqual(" + a + ", " + b + "))";
            else if (opcode == "SLT") expr = "vec4(lessThan(" + a + ", " + b + "))";
            else if (opcode == "XPD") expr = "vec4(cross(" + a + ".xyz, " + b + ".xyz), 0.0)";
            else if (opcode == "CMP") expr = "mix(" + c + ", " + b + ", lessThan(" + a + ", vec4(0.0)))";
            else if (opcode == "LRP") expr = "mix(" + c + ", " + b + ", " + a + ")";
            else if (opcode == "COS") expr = "vec4(cos(" + a + ".x))";
            else if (opcode == "SIN") expr = "vec4(sin(" + a + ".x))";
            else if (opcode == "SCS") expr = "vec4(cos(" + a + ".x), sin(" + a + ".x), 0.0, 0.0)";
        }

        if (saturate) {
            expr = "clamp(" + expr + ", 0.0, 1.0)";
        }

        if (mask.empty()) {
            body += "    " + dst + " = " + expr + ";\n";
        } else {
            body += "    " + dst + "." + mask + " = (" + expr + ")." + mask + ";\n";
        }

        return true;
    }

    bool Statement(const std::string& statement, bool& end) {
        std::string s = Trim(statement);

        if (s.empty()) {
            return true;
        }

        size_t space = 0;

        while (space < s.size() && !isspace(static_cast<unsigned char>(s[space]))) {
            space++;
        }

        std::string keyword = s.substr(0, space);
        std::string rest = Trim(s.substr(space));

        if (keyword == "END") {
            end = true;
            return rest.empty() || Fail("text after END");
        }

        if (keyword == "OPTION") {
            return Option(rest);
        }

        if (keyword == "PARAM") {
            return Param(rest);
        }

        if (keyword == "TEMP" || keyword == "ADDRESS") {
            bool address = keyword == "ADDRESS";

            if (address && !m_vertex) {
                return Fail("ADDRESS in a fragment program");
            }

            for (const std::string& part : SplitTop(rest)) {
                std::string name = StripSpace(part);
                Symbol symbol = { address ? Symbol::Address : Symbol::Temp, (address ? "a_" : "t_") + Identifier(name), 0, 0, false };

                if (!Declare(name, symbol)) {
                    return false;
                }

                m_program.m_body += address ? "    ivec4 " + symbol.m_glsl + " = ivec4(0);\n" : "    vec4 " + symbol.m_glsl + " = vec4(0.0);\n";
            }

            return true;
        }

        if (keyword == "ATTRIB" || keyword == "OUTPUT" || keyword == "ALIAS") {
            return Alias(keyword, rest);
        }

        return Instruction(keyword, rest);
    }
};

const char* kHelpers =
    "vec4 arbLit(vec4 s) {\n"
    "    float y = max(s.y, 0.0);\n"
    "    float w = clamp(s.w, -128.0, 128.0);\n"
    "    float spec = y > 0.0 ? exp2(w * log2(y)) : (w == 0.0 ? 1.0 : 0.0);\n"
    "    return vec4(1.0, max(s.x, 0.0), s.x > 0.0 ? spec : 0.0, 1.0);\n"
    "}\n"
    "vec4 arbExp(float x) {\n"
    "    float f = floor(x);\n"
    "    return vec4(exp2(f), x - f, exp2(x), 1.0);\n"
    "}\n"
    "vec4 arbLog(float x) {\n"
    "    float a = abs(x);\n"
    "    float e = floor(log2(a));\n"
    "    return vec4(e, a / exp2(e), log2(a), 1.0);\n"
    "}\n";

}

bool ArbParse(const char* text, size_t length, ArbProgram& program) {
    std::string source(text, length);
    size_t nul = source.find('\0');

    if (nul != std::string::npos) {
        source.resize(nul);
    }

    bool vertex = source.compare(0, 10, "!!ARBvp1.0") == 0;
    bool fragment = source.compare(0, 10, "!!ARBfp1.0") == 0;
    program = ArbProgram();
    program.m_vertex = vertex;

    if (!vertex && !fragment) {
        program.m_errorPosition = 0;
        program.m_error = "missing !!ARBvp1.0 / !!ARBfp1.0 header";
        return false;
    }

    // Comments run to the end of the line; blank them so offsets stay valid for the error position.
    for (size_t i = 10; i < source.size(); i++) {
        if (source[i] == '#') {
            while (i < source.size() && source[i] != '\n') {
                source[i++] = ' ';
            }
        }
    }

    Translator translator(program);
    size_t start = 10;
    bool end = false;

    while (!end && start < source.size()) {
        size_t semicolon = source.find(';', start);
        size_t stop = semicolon == std::string::npos ? source.size() : semicolon;

        if (!translator.Statement(source.substr(start, stop - start), end)) {
            program.m_errorPosition = static_cast<int32_t>(start);
            program.m_error = translator.m_error;
            return false;
        }

        if (!end && semicolon == std::string::npos) {
            program.m_errorPosition = static_cast<int32_t>(start);
            program.m_error = "missing END";
            return false;
        }

        start = stop + 1;
    }

    if (!end) {
        program.m_errorPosition = static_cast<int32_t>(source.size());
        program.m_error = "missing END";
        return false;
    }

    if (!program.m_slots.empty()) {
        program.m_decls += std::string("uniform vec4 ") + (vertex ? "uVC[" : "uFC[") + std::to_string(program.m_slots.size()) + "];\n";
    }

    if (vertex) {
        for (int i = 0; i < 16; i++) {
            if (program.m_attribMask & (1u << i)) {
                program.m_decls += "layout(location = " + std::to_string(i) + ") in vec4 a" + std::to_string(i) + ";\n";
            }
        }

        if (program.m_positionInvariant) {
            program.m_decls += "uniform mat4 uMV;\nuniform mat4 uP;\n";
        }
    } else {
        static const char* kSamplers[5] = { "", "sampler2D", "samplerCube", "sampler2D", "sampler2DShadow" };

        for (int i = 0; i < 16; i++) {
            if (program.m_samplers[i]) {
                program.m_decls += std::string("uniform highp ") + kSamplers[program.m_samplers[i]] + " s" + std::to_string(i) + ";\n";
            }
        }

        if (program.m_fog) {
            program.m_decls += "uniform vec4 uFogColor;\nuniform vec3 uFogParams;\n";
        }
    }

    program.m_valid = true;
    return true;
}

std::string ArbVertexShader(const ArbProgram& vp) {
    std::string s = "#version 300 es\nprecision highp float;\nprecision highp int;\n";
    s += vp.m_decls;
    s += "out vec4 vColor;\nout vec4 vSpec;\nout float vFog;\n";

    for (int i = 0; i < 8; i++) {
        s += "out vec4 vTex" + std::to_string(i) + ";\n";
    }

    s += kHelpers;
    s += "void main() {\n";
    s += "    vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);\n    vec4 oCol0 = vec4(1.0);\n    vec4 oCol1 = vec4(0.0);\n    vec4 oBack = vec4(0.0);\n";
    s += "    vec4 oFog = vec4(0.0);\n    vec4 oPts = vec4(1.0);\n";

    for (int i = 0; i < 8; i++) {
        s += "    vec4 oTex" + std::to_string(i) + " = vec4(0.0, 0.0, 0.0, 1.0);\n";
    }

    s += vp.m_body;
    s += vp.m_positionInvariant ? "    gl_Position = uP * (uMV * a0);\n" : "    gl_Position = oPos;\n";
    s += "    vColor = clamp(oCol0, 0.0, 1.0);\n    vSpec = clamp(oCol1, 0.0, 1.0);\n    vFog = oFog.x;\n    gl_PointSize = oPts.x;\n";

    for (int i = 0; i < 8; i++) {
        s += "    vTex" + std::to_string(i) + " = oTex" + std::to_string(i) + ";\n";
    }

    s += "}\n";
    return s;
}

std::string ArbFragmentShader(const ArbProgram& fp, bool pointSprite, const std::string& extraDecls, const std::string& epilogue) {
    std::string s = "#version 300 es\nprecision highp float;\nprecision highp int;\n";
    s += "in vec4 vColor;\nin vec4 vSpec;\nin float vFog;\n";

    for (int i = 0; i < 8; i++) {
        if (fp.m_texcoordMask & (1u << i)) {
            s += "in vec4 vTex" + std::to_string(i) + ";\n";
        }
    }

    s += fp.m_decls;
    s += extraDecls;
    s += "out vec4 fragColor;\n";
    s += kHelpers;
    s += "void main() {\n";
    s += "    vec4 fCol0 = vColor;\n    vec4 fCol1 = vSpec;\n    vec4 fFog = vec4(vFog, 0.0, 0.0, 1.0);\n";

    for (int i = 0; i < 8; i++) {
        if (fp.m_texcoordMask & (1u << i)) {
            s += "    vec4 fTex" + std::to_string(i) + " = " + (pointSprite ? "vec4(gl_PointCoord, 0.0, 1.0)" : "vTex" + std::to_string(i)) + ";\n";
        }
    }

    s += "    vec4 oColor = vec4(0.0);\n    vec4 oDepth = vec4(gl_FragCoord.z);\n";
    s += fp.m_body;

    switch (fp.m_fog) {
        case 1:
            s += "    float fogF = clamp((uFogParams.y - vFog) / (abs(uFogParams.y - uFogParams.x) < 1e-6 ? 1e-6 : uFogParams.y - uFogParams.x), 0.0, 1.0);\n";
            break;
        case 2:
            s += "    float fogF = clamp(exp(-uFogParams.z * vFog), 0.0, 1.0);\n";
            break;
        case 3:
            s += "    float fogF = clamp(exp(-(uFogParams.z * vFog) * (uFogParams.z * vFog)), 0.0, 1.0);\n";
            break;
        default:
            break;
    }

    if (fp.m_fog) {
        s += "    oColor.rgb = mix(uFogColor.rgb, oColor.rgb, fogF);\n";
    }

    s += epilogue;
    s += "    fragColor = oColor;\n";

    if (fp.m_writesDepth) {
        s += "    gl_FragDepth = oDepth.z;\n";
    }

    s += "}\n";
    return s;
}

#endif
