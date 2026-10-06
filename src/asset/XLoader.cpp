// DirectX .x files (MMD accessories) -> PmxModel. Both encodings are reduced to one token
// stream (names, strings, numbers, braces); data objects are then read positionally like the
// D3DX templates define them. Supported: Frame / FrameTransformMatrix hierarchy, Mesh,
// MeshNormals, MeshTextureCoords, MeshMaterialList, Material (+ TextureFilename), named materials
// referenced from material lists. Compressed (tzip/bzip) files are rejected.
#include "asset/PmxModel.h"
#include "asset/BinaryReader.h"
#include "core/TextUtil.h"

#include <DirectXMath.h>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>

namespace mmdx {

using namespace DirectX;

namespace {

struct Token {
    enum Kind { Name, String, Number, OBrace, CBrace, Guid, End } kind = End;
    std::string text;
    double value = 0;
};

// ---- text tokenizer
bool TokenizeText(const uint8_t* p, size_t n, std::vector<Token>& out) {
    size_t i = 0;
    while (i < n) {
        const char c = (char)p[i];
        if (std::isspace((unsigned char)c) || c == ',' || c == ';') { ++i; continue; }
        if (c == '#' || (c == '/' && i + 1 < n && p[i + 1] == '/')) {  // comment to end of line
            while (i < n && p[i] != '\n') ++i;
            continue;
        }
        if (c == '{') { out.push_back({Token::OBrace}); ++i; continue; }
        if (c == '}') { out.push_back({Token::CBrace}); ++i; continue; }
        if (c == '<') {  // GUID
            while (i < n && p[i] != '>') ++i;
            ++i;
            out.push_back({Token::Guid});
            continue;
        }
        if (c == '"') {
            size_t j = i + 1;
            while (j < n && p[j] != '"') ++j;
            out.push_back({Token::String, std::string((const char*)p + i + 1, j - i - 1)});
            i = j + 1;
            continue;
        }
        if (std::isdigit((unsigned char)c) || c == '-' || c == '+' || c == '.') {
            size_t j = i + 1;
            while (j < n && (std::isdigit(p[j]) || p[j] == '.' || p[j] == 'e' || p[j] == 'E' ||
                             ((p[j] == '-' || p[j] == '+') && (p[j - 1] == 'e' || p[j - 1] == 'E'))))
                ++j;
            const std::string s((const char*)p + i, j - i);
            Token t{Token::Number};
            t.value = std::strtod(s.c_str(), nullptr);
            out.push_back(t);
            i = j;
            continue;
        }
        if (std::isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80) {
            size_t j = i + 1;
            while (j < n && (std::isalnum(p[j]) || p[j] == '_' || p[j] == '-' || p[j] == '.' || p[j] >= 0x80)) ++j;
            out.push_back({Token::Name, std::string((const char*)p + i, j - i)});
            i = j;
            continue;
        }
        ++i;  // stray punctuation ([ ] etc. only appear in templates)
    }
    return true;
}

// ---- binary tokenizer (token ids from the DirectX .x binary format)
bool TokenizeBinary(const uint8_t* p, size_t n, bool doubles, std::vector<Token>& out, std::string* error) {
    BinaryReader r(p, n);
    while (r.Remaining() >= 2) {
        uint16_t tok = 0;
        r.Read(tok);
        uint32_t count = 0;
        switch (tok) {
        case 1: {  // name
            r.Read(count);
            if (count > r.Remaining()) break;
            std::string s(count, '\0');
            r.Read(s.data(), count);
            out.push_back({Token::Name, s});
            break;
        }
        case 2: {  // string + terminator token
            r.Read(count);
            if (count > r.Remaining()) break;
            std::string s(count, '\0');
            r.Read(s.data(), count);
            while (!s.empty() && s.back() == '\0') s.pop_back();
            r.Skip(2);
            out.push_back({Token::String, s});
            break;
        }
        case 3: {  // integer
            uint32_t v = 0;
            r.Read(v);
            Token t{Token::Number};
            t.value = v;
            out.push_back(t);
            break;
        }
        case 5:  // guid
            r.Skip(16);
            out.push_back({Token::Guid});
            break;
        case 6: {  // integer list
            r.Read(count);
            if ((uint64_t)count * 4 > r.Remaining()) break;
            for (uint32_t k = 0; k < count; ++k) {
                uint32_t v = 0;
                r.Read(v);
                Token t{Token::Number};
                t.value = v;
                out.push_back(t);
            }
            break;
        }
        case 7: {  // float list
            r.Read(count);
            if ((uint64_t)count * (doubles ? 8 : 4) > r.Remaining()) break;
            for (uint32_t k = 0; k < count; ++k) {
                Token t{Token::Number};
                if (doubles) {
                    double v = 0;
                    r.Read(v);
                    t.value = v;
                } else {
                    float v = 0;
                    r.Read(v);
                    t.value = v;
                }
                out.push_back(t);
            }
            break;
        }
        case 10: out.push_back({Token::OBrace}); break;
        case 11: out.push_back({Token::CBrace}); break;
        case 31: out.push_back({Token::Name, "template"}); break;
        default:
            if (tok >= 12 && tok <= 20) break;   // ( ) [ ] < > . , ;
            if (tok >= 40 && tok <= 52) {        // template member types: only inside templates
                out.push_back({Token::Name, "_type"});
                break;
            }
            if (error) *error = "X: unknown binary token " + std::to_string(tok);
            return false;
        }
        if (r.Failed()) {
            if (error) *error = "X: truncated binary token";
            return false;
        }
    }
    return true;
}

// ---- generic object tree
struct XObject {
    std::string type, name;
    std::vector<double> numbers;
    std::vector<std::string> strings;
    std::vector<std::unique_ptr<XObject>> children;
    std::vector<std::string> references;  // { Name } inside the object
};

class Parser {
public:
    explicit Parser(const std::vector<Token>& t) : t_(t) {}

    bool ParseAll(std::vector<std::unique_ptr<XObject>>& top, std::string* error) {
        while (i_ < t_.size()) {
            if (t_[i_].kind != Token::Name) { ++i_; continue; }
            if (t_[i_].text == "template") {
                SkipBlock();
                continue;
            }
            auto o = ParseObject(error);
            if (!o) return false;
            top.push_back(std::move(o));
        }
        return true;
    }

private:
    const Token& At(size_t k) const {
        static const Token end{};
        return k < t_.size() ? t_[k] : end;
    }

    void SkipBlock() {  // template Name { ... }
        while (i_ < t_.size() && t_[i_].kind != Token::OBrace) ++i_;
        int depth = 0;
        for (; i_ < t_.size(); ++i_) {
            if (t_[i_].kind == Token::OBrace) ++depth;
            else if (t_[i_].kind == Token::CBrace && --depth == 0) { ++i_; return; }
        }
    }

    std::unique_ptr<XObject> ParseObject(std::string* error) {
        if (++depth_ > 64) {
            if (error) *error = "X: objects nested too deeply";
            return nullptr;
        }
        auto o = std::make_unique<XObject>();
        o->type = At(i_++).text;
        if (At(i_).kind == Token::Name) o->name = At(i_++).text;
        if (At(i_).kind != Token::OBrace) {
            if (error) *error = "X: expected '{' after " + o->type;
            return nullptr;
        }
        ++i_;
        if (At(i_).kind == Token::Guid) ++i_;
        while (i_ < t_.size() && At(i_).kind != Token::CBrace) {
            const Token& t = At(i_);
            if (t.kind == Token::Number) { o->numbers.push_back(t.value); ++i_; }
            else if (t.kind == Token::String) { o->strings.push_back(t.text); ++i_; }
            else if (t.kind == Token::Guid) { ++i_; }
            else if (t.kind == Token::OBrace) {  // reference { Name [guid] }
                ++i_;
                if (At(i_).kind == Token::Name) o->references.push_back(At(i_).text);
                while (i_ < t_.size() && At(i_).kind != Token::CBrace) ++i_;
                ++i_;
            } else if (t.kind == Token::Name) {
                auto c = ParseObject(error);
                if (!c) return nullptr;
                o->children.push_back(std::move(c));
            } else {
                ++i_;
            }
        }
        if (At(i_).kind != Token::CBrace) {
            if (error) *error = "X: unexpected end of file in " + o->type;
            return nullptr;
        }
        ++i_;
        --depth_;
        return o;
    }

    const std::vector<Token>& t_;
    size_t i_ = 0;
    int depth_ = 0;
};

// ---- conversion
struct Converter {
    PmxModel& out;
    std::map<std::string, const XObject*> namedMaterials;
    std::map<std::string, int32_t> textureIndex;

    int32_t Texture(const std::string& name) {
        if (name.empty()) return -1;
        auto it = textureIndex.find(name);
        if (it != textureIndex.end()) return it->second;
        out.textures.push_back(name);
        return textureIndex[name] = (int32_t)out.textures.size() - 1;
    }

    PmxMaterial MakeMaterial(const XObject* m) {
        PmxMaterial mat;
        const auto num = [&](size_t k, double def) { return m && k < m->numbers.size() ? (float)m->numbers[k] : (float)def; };
        // faceColor rgba, power, specular rgb, emissive rgb (MMD draws the emissive colour as ambient)
        mat.diffuse = {num(0, 1), num(1, 1), num(2, 1), num(3, 1)};
        mat.specularPower = num(4, 0);
        mat.specular = {num(5, 0), num(6, 0), num(7, 0)};
        mat.ambient = {num(8, 0), num(9, 0), num(10, 0)};
        mat.flags = PmxMat_GroundShadow | PmxMat_CastShadow | PmxMat_ReceiveShadow;
        if (mat.diffuse.w < 1.0f) mat.flags |= PmxMat_DoubleSided;
        mat.edgeColor = {0, 0, 0, 1};
        mat.edgeSize = 0;
        if (m)
            for (const auto& c : m->children)
                if (c->type == "TextureFilename" && !c->strings.empty()) {
                    std::string tex = SjisToUtf8(c->strings[0]), sphere;
                    if (const size_t star = tex.find('*'); star != std::string::npos) {
                        sphere = tex.substr(star + 1);
                        tex = tex.substr(0, star);
                    } else {
                        const std::string lower = ToLowerAscii(tex);
                        if (lower.size() > 4 && (lower.ends_with(".sph") || lower.ends_with(".spa"))) std::swap(tex, sphere);
                    }
                    mat.textureIndex = Texture(tex);
                    if (!sphere.empty()) {
                        mat.sphereTextureIndex = Texture(sphere);
                        mat.sphereMode = ToLowerAscii(sphere).ends_with(".spa") ? PmxSphereMode::Add : PmxSphereMode::Multiply;
                    }
                }
        return mat;
    }

    void Mesh(const XObject& mesh, FXMMATRIX world) {
        const std::vector<double>& n = mesh.numbers;
        size_t k = 0;
        const auto next = [&]() { return k < n.size() ? n[k++] : 0.0; };
        const uint32_t nv = (uint32_t)next();
        if ((uint64_t)nv * 3 > n.size()) return;
        std::vector<XMFLOAT3> pos(nv);
        for (XMFLOAT3& p : pos) {
            const float x = (float)next(), y = (float)next(), z = (float)next();
            XMStoreFloat3(&p, XMVector3TransformCoord(XMVectorSet(x, y, z, 1), world));
        }
        const uint32_t nf = (uint32_t)next();
        std::vector<std::vector<uint32_t>> faces(nf);
        for (auto& f : faces) {
            const uint32_t c = (uint32_t)next();
            if (c > 64) return;
            f.resize(c);
            for (uint32_t& v : f) v = (uint32_t)next();
        }
        // normals (own index lists) / uvs (per position) / per-face material
        std::vector<XMFLOAT3> normals;
        std::vector<std::vector<uint32_t>> normalFaces;
        std::vector<XMFLOAT2> uvs;
        std::vector<uint32_t> faceMaterial(nf, 0);
        std::vector<PmxMaterial> mats;
        const XMMATRIX normalMat = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
        for (const auto& c : mesh.children) {
            const std::vector<double>& m = c->numbers;
            size_t q = 0;
            const auto nx = [&]() { return q < m.size() ? m[q++] : 0.0; };
            if (c->type == "MeshNormals") {
                normals.resize((size_t)nx());
                for (XMFLOAT3& v : normals) {
                    const float x = (float)nx(), y = (float)nx(), z = (float)nx();
                    XMStoreFloat3(&v, XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(x, y, z, 0), normalMat)));
                }
                normalFaces.resize((size_t)nx());
                for (auto& f : normalFaces) {
                    f.resize(std::min<size_t>((size_t)nx(), 64));
                    for (uint32_t& v : f) v = (uint32_t)nx();
                }
            } else if (c->type == "MeshTextureCoords") {
                uvs.resize((size_t)nx());
                for (XMFLOAT2& v : uvs) {
                    v.x = (float)nx();
                    v.y = (float)nx();
                }
            } else if (c->type == "MeshMaterialList") {
                const uint32_t nm = (uint32_t)nx();
                const uint32_t ni = (uint32_t)nx();
                for (uint32_t f = 0; f < ni && f < nf; ++f) faceMaterial[f] = (uint32_t)nx();
                // materials in order: inline objects and references (a file uses one style)
                std::vector<const XObject*> list;
                for (const auto& mc : c->children)
                    if (mc->type == "Material") list.push_back(mc.get());
                for (const std::string& ref : c->references)
                    if (auto it = namedMaterials.find(ref); it != namedMaterials.end()) list.push_back(it->second);
                for (uint32_t i = 0; i < nm; ++i) mats.push_back(MakeMaterial(i < list.size() ? list[i] : nullptr));
            }
        }
        if (mats.empty()) mats.push_back(MakeMaterial(nullptr));
        if (normalFaces.size() != faces.size()) normalFaces.clear();

        // flat normals where the file has none
        std::vector<XMFLOAT3> faceNormal(nf, XMFLOAT3{0, 1, 0});
        for (uint32_t f = 0; f < nf; ++f) {
            const auto& fv = faces[f];
            if (fv.size() < 3 || fv[0] >= nv || fv[1] >= nv || fv[2] >= nv) continue;
            const XMVECTOR a = XMLoadFloat3(&pos[fv[0]]), b = XMLoadFloat3(&pos[fv[1]]), cc = XMLoadFloat3(&pos[fv[2]]);
            XMStoreFloat3(&faceNormal[f], XMVector3Normalize(XMVector3Cross(b - a, cc - a)));
        }

        // one PMX material per (mesh, material) with its faces fan-triangulated; vertices are split
        // per (position, normal) pair
        for (uint32_t mi = 0; mi < mats.size(); ++mi) {
            std::map<std::pair<uint32_t, uint32_t>, uint32_t> vertexOf;
            const size_t indexStart = out.indices.size();
            const auto vertex = [&](uint32_t f, uint32_t corner) -> uint32_t {
                const uint32_t p = faces[f][corner];
                uint32_t nIdx = UINT32_MAX;
                if (!normalFaces.empty() && corner < normalFaces[f].size() && normalFaces[f][corner] < normals.size())
                    nIdx = normalFaces[f][corner];
                const auto key = std::make_pair(p, nIdx == UINT32_MAX ? 0x80000000u | f : nIdx);
                if (auto it = vertexOf.find(key); it != vertexOf.end()) return it->second;
                PmxVertex v;
                v.position = pos[p];
                v.normal = nIdx == UINT32_MAX ? faceNormal[f] : normals[nIdx];
                v.uv = p < uvs.size() ? uvs[p] : XMFLOAT2{0, 0};
                v.deform = PmxDeform::BDEF1;
                v.boneIndex[0] = 0;
                v.boneWeight[0] = 1.0f;
                v.edgeScale = 1.0f;
                out.vertices.push_back(v);
                return vertexOf[key] = (uint32_t)out.vertices.size() - 1;
            };
            for (uint32_t f = 0; f < nf; ++f) {
                if (faceMaterial[f] != mi || faces[f].size() < 3) continue;
                bool ok = true;
                for (uint32_t v : faces[f]) ok &= v < nv;
                if (!ok) continue;
                for (uint32_t c = 1; c + 1 < faces[f].size(); ++c) {
                    out.indices.push_back(vertex(f, 0));
                    out.indices.push_back(vertex(f, c));
                    out.indices.push_back(vertex(f, c + 1));
                }
            }
            const uint32_t count = (uint32_t)(out.indices.size() - indexStart);
            if (count == 0) continue;
            PmxMaterial m = mats[mi];
            m.indexCount = count;
            m.name = mesh.name.empty() ? "Material" + std::to_string(out.materials.size() + 1)
                                       : SjisToUtf8(mesh.name) + "_" + std::to_string(mi);
            m.nameEn = m.name;
            out.materials.push_back(std::move(m));
        }
    }

    void Walk(const XObject& o, FXMMATRIX parent) {
        if (o.type == "Mesh") {
            Mesh(o, parent);
            return;
        }
        if (o.type != "Frame") return;
        XMMATRIX local = XMMatrixIdentity();
        for (const auto& c : o.children)
            if (c->type == "FrameTransformMatrix" && c->numbers.size() >= 16) {
                XMFLOAT4X4 m;
                for (int i = 0; i < 16; ++i) (&m._11)[i] = (float)c->numbers[i];
                local = XMLoadFloat4x4(&m);
            }
        const XMMATRIX world = local * parent;  // row vectors: child, then parent
        for (const auto& c : o.children) Walk(*c, world);
    }
};

}  // namespace

bool LoadXFile(const std::filesystem::path& path, PmxModel& out, std::string* error) {
    try {
        std::vector<uint8_t> data;
        if (!ReadWholeFile(path, data, error)) return false;
        if (data.size() < 16 || std::memcmp(data.data(), "xof ", 4) != 0) {
            if (error) *error = "X: not a DirectX .x file";
            return false;
        }
        const std::string format((const char*)data.data() + 8, 4), floatBits((const char*)data.data() + 12, 4);
        std::vector<Token> tokens;
        if (format == "txt ") {
            TokenizeText(data.data() + 16, data.size() - 16, tokens);
        } else if (format == "bin ") {
            if (!TokenizeBinary(data.data() + 16, data.size() - 16, floatBits == "0064", tokens, error)) return false;
        } else {
            if (error) *error = "X: compressed .x files (" + format + ") are not supported";
            return false;
        }
        std::vector<std::unique_ptr<XObject>> top;
        Parser parser(tokens);
        if (!parser.ParseAll(top, error)) return false;

        out = PmxModel{};
        out.sourcePath = std::filesystem::absolute(path);
        out.name = PathToUtf8(path.stem());
        out.nameEn = out.name;
        PmxBone root;
        root.name = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC";  // センター
        root.nameEn = "center";
        root.flags = PmxBone_Rotatable | PmxBone_Movable | PmxBone_Visible | PmxBone_Operable;
        out.bones.push_back(root);
        Converter conv{out};
        for (const auto& o : top)
            if (o->type == "Material" && !o->name.empty()) conv.namedMaterials[o->name] = o.get();
        for (const auto& o : top) conv.Walk(*o, XMMatrixIdentity());
        if (out.vertices.empty()) {
            if (error) *error = "X: no mesh";
            return false;
        }
        PmxDisplayFrame rootFrame;
        rootFrame.name = rootFrame.nameEn = "Root";
        rootFrame.special = true;
        rootFrame.items.push_back({false, 0});
        out.displayFrames.push_back(std::move(rootFrame));
        return true;
    } catch (const std::exception& e) {
        if (error) *error = std::string("X: ") + e.what();
        return false;
    }
}

}  // namespace mmdx
