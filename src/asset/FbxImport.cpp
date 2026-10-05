// FBX / OBJ front end: ufbx -> ImpScene (converted by ufbx to right-handed +Y up, metres).
#include <ufbx.h>

#include "asset/BinaryReader.h"
#include "asset/ImportScene.h"
#include "core/TextUtil.h"

#include <cstring>
#include <unordered_map>

namespace mmdx {

using namespace DirectX;

namespace {

XMFLOAT4X4 ToRowMatrix(const ufbx_matrix& m) {
    // ufbx_matrix columns are the x/y/z axes and the translation (column vectors); for row
    // vectors they become the rows.
    return XMFLOAT4X4((float)m.m00, (float)m.m10, (float)m.m20, 0, (float)m.m01, (float)m.m11, (float)m.m21, 0,
                      (float)m.m02, (float)m.m12, (float)m.m22, 0, (float)m.m03, (float)m.m13, (float)m.m23, 1);
}

struct CornerKey {
    uint32_t position, normal, uv;
    bool operator==(const CornerKey&) const = default;
};
struct CornerKeyHash {
    size_t operator()(const CornerKey& k) const {
        return std::hash<uint64_t>()(((uint64_t)k.position << 32) ^ ((uint64_t)k.normal << 16) ^ k.uv ^ ((uint64_t)k.uv << 48));
    }
};

std::string Str(const ufbx_string& s) { return std::string(s.data, s.length); }

std::filesystem::path FindTextureFile(const ufbx_texture* tex, const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> candidates;
    if (tex->filename.length) candidates.push_back(Utf8ToPath(Str(tex->filename)));
    if (tex->relative_filename.length) candidates.push_back(dir / Utf8ToPath(Str(tex->relative_filename)));
    if (tex->absolute_filename.length) candidates.push_back(Utf8ToPath(Str(tex->absolute_filename)));
    std::filesystem::path base;
    for (const auto& c : candidates)
        if (!c.filename().empty()) {
            base = c.filename();
            break;
        }
    if (base.empty() && tex->name.length) base = Utf8ToPath(Str(tex->name)).filename();
    if (!base.empty()) {
        for (const char* sub : {"", "textures", "Textures", "tex", "texture"}) candidates.push_back(dir / sub / base);
        // Exporters often put embedded media in "<file>.fbm".
        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_directory(ec) && ToLowerAscii(PathToUtf8(it->path().extension())) == ".fbm")
                candidates.push_back(it->path() / base);
    }
    for (const auto& c : candidates) {
        std::error_code ec;
        if (!c.empty() && std::filesystem::is_regular_file(c, ec)) return c;
    }
    return {};
}

} // namespace

bool ImportUfbx(const std::filesystem::path& path, ImpScene& scene, std::string* error) {
    std::vector<uint8_t> fileData;
    std::string readErr;
    if (!ReadWholeFile(path, fileData, &readErr)) {
        if (error) *error = readErr;
        return false;
    }
    const std::string pathUtf8 = PathToUtf8(path);
    ufbx_load_opts opts{};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0f;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    opts.generate_missing_normals = true;
    opts.ignore_animation = true;
    opts.load_external_files = true;  // OBJ .mtl
    opts.ignore_missing_external_files = true;
    opts.filename = {pathUtf8.c_str(), pathUtf8.size()};
    ufbx_error err{};
    ufbx_scene* fbx = ufbx_load_memory(fileData.data(), fileData.size(), &opts, &err);
    if (!fbx) {
        if (error) *error = std::string(err.description.data, err.description.length);
        return false;
    }
    struct Guard {
        ufbx_scene* s;
        ~Guard() { ufbx_free_scene(s); }
    } guard{fbx};

    // Nodes (ufbx lists parents before children).
    scene.nodes.resize(fbx->nodes.count);
    for (size_t i = 0; i < fbx->nodes.count; ++i) {
        const ufbx_node* n = fbx->nodes.data[i];
        ImpNode& out = scene.nodes[i];
        out.name = n->name.length ? Str(n->name) : (n->is_root ? "root" : "node" + std::to_string(i));
        out.parent = n->parent ? (int)n->parent->typed_id : -1;
        out.local = ToRowMatrix(n->node_to_parent);
    }

    // Materials and textures.
    const std::filesystem::path dir = path.parent_path();
    std::unordered_map<const ufbx_texture*, int> texCache;
    auto importTexture = [&](const ufbx_texture* tex) -> int {
        if (!tex) return -1;
        if (tex->type != UFBX_TEXTURE_FILE && tex->file_textures.count > 0) tex = tex->file_textures.data[0];
        if (auto it = texCache.find(tex); it != texCache.end()) return it->second;
        ImpTexture t;
        t.label = tex->relative_filename.length ? Str(tex->relative_filename) : Str(tex->name);
        if (tex->content.size > 0) {
            const uint8_t* p = (const uint8_t*)tex->content.data;
            t.bytes.assign(p, p + tex->content.size);
        } else {
            t.file = FindTextureFile(tex, dir);
            if (t.file.empty()) scene.warnings.push_back("texture not found: " + t.label);
        }
        const int index = (int)scene.textures.size();
        scene.textures.push_back(std::move(t));
        texCache[tex] = index;
        return index;
    };
    std::unordered_map<const ufbx_material*, int> matIndex;
    for (size_t i = 0; i < fbx->materials.count; ++i) {
        const ufbx_material* m = fbx->materials.data[i];
        ImpMaterial out;
        out.name = Str(m->name);
        const ufbx_material_map* color = &m->pbr.base_color;
        if (!color->has_value && !color->texture) color = &m->fbx.diffuse_color;
        if (color->has_value) {
            const ufbx_vec4 c = color->value_vec4;
            out.baseColor = {(float)c.x, (float)c.y, (float)c.z, 1.0f};
            if (color == &m->pbr.base_color && m->pbr.base_factor.has_value) {
                const float f = (float)m->pbr.base_factor.value_real;
                if (f > 0) out.baseColor = {out.baseColor.x * f, out.baseColor.y * f, out.baseColor.z * f, 1.0f};
            }
        }
        const ufbx_texture* tex = m->pbr.base_color.texture ? m->pbr.base_color.texture : m->fbx.diffuse_color.texture;
        out.texture = importTexture(tex);
        if (out.texture >= 0) out.baseColor = {1, 1, 1, 1};  // FBX diffuse colours are often a dark placeholder under a texture
        if (m->pbr.opacity.has_value && m->pbr.opacity.value_real < 0.999) {
            out.baseColor.w = (float)m->pbr.opacity.value_real;
            out.blend = true;
        }
        if (m->pbr.opacity.texture) out.blend = true;
        out.doubleSided = m->features.double_sided.enabled;
        matIndex[m] = (int)scene.materials.size();
        scene.materials.push_back(std::move(out));
    }

    // Meshes, split by material.
    std::vector<uint32_t> tri;
    for (size_t ni = 0; ni < fbx->nodes.count; ++ni) {
        const ufbx_node* node = fbx->nodes.data[ni];
        const ufbx_mesh* mesh = node->mesh;
        if (!mesh || mesh->num_faces == 0) continue;
        tri.resize(mesh->max_face_triangles * 3);

        // Skin.
        int skinIndex = -1;
        const ufbx_skin_deformer* skin = mesh->skin_deformers.count > 0 ? mesh->skin_deformers.data[0] : nullptr;
        if (skin && skin->clusters.count > 0) {
            ImpSkin s;
            for (size_t c = 0; c < skin->clusters.count; ++c) {
                const ufbx_skin_cluster* cl = skin->clusters.data[c];
                s.joints.push_back(cl->bone_node ? (int)cl->bone_node->typed_id : (int)ni);
                s.inverseBind.push_back(ToRowMatrix(cl->geometry_to_bone));
            }
            skinIndex = (int)scene.skins.size();
            scene.skins.push_back(std::move(s));
        }

        // Morph targets: logical vertex -> offset, per channel.
        struct Channel {
            std::string name;
            std::unordered_map<uint32_t, XMFLOAT3> offsets;
        };
        std::vector<Channel> channels;
        for (size_t b = 0; b < mesh->blend_deformers.count; ++b) {
            const ufbx_blend_deformer* bd = mesh->blend_deformers.data[b];
            for (size_t c = 0; c < bd->channels.count; ++c) {
                const ufbx_blend_channel* ch = bd->channels.data[c];
                const ufbx_blend_shape* shape = ch->target_shape;
                if (!shape && ch->keyframes.count > 0) shape = ch->keyframes.data[ch->keyframes.count - 1].shape;
                if (!shape) continue;
                Channel out;
                out.name = Str(ch->name);
                if (const size_t dot = out.name.rfind('.'); dot != std::string::npos) out.name = out.name.substr(dot + 1);
                for (size_t k = 0; k < shape->num_offsets; ++k) {
                    const uint32_t v = shape->offset_vertices.data[k];
                    if (v >= mesh->num_vertices) continue;
                    const ufbx_vec3 o = shape->position_offsets.data[k];
                    out.offsets[v] = {(float)o.x, (float)o.y, (float)o.z};
                }
                channels.push_back(std::move(out));
            }
        }

        const size_t matCount = std::max<size_t>(mesh->materials.count, 1);
        for (size_t mi = 0; mi < matCount; ++mi) {
            ImpMesh out;
            out.name = Str(node->name);
            out.node = (int)ni;
            out.skin = skinIndex;
            out.morphGroup = "u" + std::to_string(mesh->typed_id);
            if (mesh->materials.count > 0) {
                auto it = matIndex.find(mesh->materials.data[mi]);
                out.material = it != matIndex.end() ? it->second : -1;
            }
            std::unordered_map<CornerKey, uint32_t, CornerKeyHash> dedup;
            std::vector<uint32_t> logical;  // output vertex -> mesh vertex
            for (size_t f = 0; f < mesh->num_faces; ++f) {
                if (mesh->materials.count > 0 && mesh->face_material.count > f && mesh->face_material.data[f] != mi) continue;
                const uint32_t triCount = ufbx_triangulate_face(tri.data(), tri.size(), mesh, mesh->faces.data[f]);
                for (uint32_t k = 0; k < triCount * 3; ++k) {
                    const uint32_t corner = tri[k];
                    const uint32_t pi = mesh->vertex_position.indices.data[corner];
                    const uint32_t ni2 = mesh->vertex_normal.exists ? mesh->vertex_normal.indices.data[corner] : 0;
                    const uint32_t ui = mesh->vertex_uv.exists ? mesh->vertex_uv.indices.data[corner] : 0;
                    auto [it, inserted] = dedup.try_emplace(CornerKey{pi, ni2, ui}, (uint32_t)out.positions.size());
                    if (inserted) {
                        const ufbx_vec3 p = mesh->vertex_position.values.data[pi];
                        out.positions.push_back({(float)p.x, (float)p.y, (float)p.z});
                        if (mesh->vertex_normal.exists) {
                            const ufbx_vec3 n = mesh->vertex_normal.values.data[ni2];
                            out.normals.push_back({(float)n.x, (float)n.y, (float)n.z});
                        }
                        if (mesh->vertex_uv.exists) {
                            const ufbx_vec2 t = mesh->vertex_uv.values.data[ui];
                            out.uvs.push_back({(float)t.x, 1.0f - (float)t.y});  // FBX UV origin is bottom-left
                        }
                        logical.push_back(mesh->vertex_indices.data[corner]);
                    }
                    out.indices.push_back(it->second);
                }
            }
            if (out.indices.empty()) continue;
            if (skinIndex >= 0) {
                out.joints.resize(out.positions.size());
                out.weights.resize(out.positions.size());
                for (size_t v = 0; v < out.positions.size(); ++v) {
                    std::array<int, 4> j{-1, -1, -1, -1};
                    XMFLOAT4 w{0, 0, 0, 0};
                    float* wp = &w.x;
                    const uint32_t lv = logical[v];
                    if (lv < skin->vertices.count) {
                        const ufbx_skin_vertex sv = skin->vertices.data[lv];
                        // Weights are sorted by decreasing weight; keep the largest four.
                        for (uint32_t k = 0; k < sv.num_weights && k < 4; ++k) {
                            const ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + k];
                            j[k] = (int)sw.cluster_index;
                            wp[k] = (float)sw.weight;
                        }
                    }
                    out.joints[v] = j;
                    out.weights[v] = w;
                }
            }
            for (const Channel& ch : channels) {
                ImpMorphTarget mt;
                mt.name = ch.name;
                for (size_t v = 0; v < logical.size(); ++v)
                    if (auto it = ch.offsets.find(logical[v]); it != ch.offsets.end()) {
                        mt.vertices.push_back((uint32_t)v);
                        mt.offsets.push_back(it->second);
                    }
                out.morphs.push_back(std::move(mt));
            }
            scene.meshes.push_back(std::move(out));
        }
    }
    scene.UpdateWorld();
    return true;
}

} // namespace mmdx
