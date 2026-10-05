#include "studio/StudioPose.h"
#include <cmath>
#include <algorithm>
#include <string_view>

namespace mmdx::studio {

namespace {

bool Differs(const PoseBone& a, const PoseBone& b) {
    if (std::abs(a.t.x - b.t.x) > 1e-6f || std::abs(a.t.y - b.t.y) > 1e-6f || std::abs(a.t.z - b.t.z) > 1e-6f) return true;
    bool d1 = std::abs(a.r.x - b.r.x) > 1e-6f || std::abs(a.r.y - b.r.y) > 1e-6f ||
              std::abs(a.r.z - b.r.z) > 1e-6f || std::abs(a.r.w - b.r.w) > 1e-6f;
    bool d2 = std::abs(a.r.x - (-b.r.x)) > 1e-6f || std::abs(a.r.y - (-b.r.y)) > 1e-6f ||
              std::abs(a.r.z - (-b.r.z)) > 1e-6f || std::abs(a.r.w - (-b.r.w)) > 1e-6f;
    return d1 && d2;
}

bool IsIdentity(const PoseBone& b) {
    if (std::abs(b.t.x) > 1e-6f || std::abs(b.t.y) > 1e-6f || std::abs(b.t.z) > 1e-6f) return false;
    bool i1 = std::abs(b.r.x) <= 1e-6f && std::abs(b.r.y) <= 1e-6f && std::abs(b.r.z) <= 1e-6f && std::abs(b.r.w - 1.0f) <= 1e-6f;
    bool i2 = std::abs(b.r.x) <= 1e-6f && std::abs(b.r.y) <= 1e-6f && std::abs(b.r.z) <= 1e-6f && std::abs(b.r.w - (-1.0f)) <= 1e-6f;
    return i1 || i2;
}

} // namespace

bool SamePoseLayer(const PoseLayer& a, const PoseLayer& b) {
    if (a.frame != b.frame) return false;
    if (a.bones.size() != b.bones.size() || a.morphs.size() != b.morphs.size()) return false;
    for (auto it_a = a.bones.begin(), it_b = b.bones.begin(); it_a != a.bones.end(); ++it_a, ++it_b) {
        if (it_a->first != it_b->first) return false;
        if (it_a->second.t.x != it_b->second.t.x || it_a->second.t.y != it_b->second.t.y || it_a->second.t.z != it_b->second.t.z ||
            it_a->second.r.x != it_b->second.r.x || it_a->second.r.y != it_b->second.r.y ||
            it_a->second.r.z != it_b->second.r.z || it_a->second.r.w != it_b->second.r.w)
            return false;
    }
    for (auto it_a = a.morphs.begin(), it_b = b.morphs.begin(); it_a != a.morphs.end(); ++it_a, ++it_b) {
        if (it_a->first != it_b->first || it_a->second != it_b->second) return false;
    }
    return true;
}

std::string MirrorBoneName(const std::string& name) {
    std::string_view left = "\xE5\xB7\xA6";
    std::string_view right = "\xE5\x8F\xB3";
    if (name.find(left) != std::string::npos || name.find(right) != std::string::npos) {
        std::string res;
        for (size_t i = 0; i < name.size(); ) {
            if (i + 3 <= name.size() && name.substr(i, 3) == left) {
                res += right;
                i += 3;
            } else if (i + 3 <= name.size() && name.substr(i, 3) == right) {
                res += left;
                i += 3;
            } else {
                res += name[i];
                i++;
            }
        }
        return res;
    }

    std::string_view suffixes[] = {"_L", "_R", ".L", ".R", "_l", "_r", ".l", ".r"};
    std::string_view repl_suf[] = {"_R", "_L", ".R", ".L", "_r", "_l", ".r", ".l"};
    for (int i = 0; i < 8; ++i) {
        if (name.size() > suffixes[i].size() && name.ends_with(suffixes[i])) {
            return name.substr(0, name.size() - suffixes[i].size()) + std::string(repl_suf[i]);
        }
    }

    std::string_view words[] = {"Left", "Right", "left", "right"};
    std::string_view repl_words[] = {"Right", "Left", "right", "left"};
    size_t first_pos = std::string::npos;
    int first_idx = -1;
    for (int i = 0; i < 4; ++i) {
        size_t pos = name.find(words[i]);
        if (pos != std::string::npos && (first_pos == std::string::npos || pos < first_pos)) {
            first_pos = pos;
            first_idx = i;
        }
    }
    if (first_idx >= 0) {
        std::string res = name;
        res.replace(first_pos, words[first_idx].size(), repl_words[first_idx]);
        return res;
    }

    return name;
}

int MirrorBoneIndex(const PmxModel& model, int bone) {
    if (bone < 0 || bone >= (int)model.bones.size()) return bone;
    std::string m = MirrorBoneName(model.bones[bone].name);
    if (m == model.bones[bone].name) return bone;
    int idx = model.FindBone(m);
    return idx >= 0 ? idx : bone;
}

PoseBone MirrorPose(const PoseBone& p) {
    return {{-p.t.x, p.t.y, p.t.z}, {p.r.x, -p.r.y, -p.r.z, p.r.w}};
}

int MirrorPoseInto(const PmxModel& model, const std::vector<PoseBone>& current, const std::set<int>* onlyBones, PoseLayer& out) {
    int n = (int)model.bones.size();
    if ((int)current.size() != n) return 0;
    
    std::vector<int> scope;
    if (onlyBones) {
        for (int b : *onlyBones) {
            if (b >= 0 && b < n) scope.push_back(b);
        }
        std::sort(scope.begin(), scope.end());
    } else {
        for (int i = 0; i < n; ++i) scope.push_back(i);
    }
    
    std::map<int, PoseBone> results;
    for (int b : scope) {
        int m = MirrorBoneIndex(model, b);
        PoseBone v = MirrorPose(current[b]);
        if (!(model.bones[m].flags & PmxBone_Movable)) v.t = current[m].t;
        if (!(model.bones[m].flags & PmxBone_Rotatable)) v.r = current[m].r;
        results[m] = v;
    }
    
    int written = 0;
    for (const auto& [m, v] : results) {
        if (Differs(v, current[m])) {
            out.bones[m] = v;
            ++written;
        }
    }
    return written;
}

VpdPose MakeVpdPose(const PmxModel& model, const std::vector<PoseBone>& current, const std::vector<float>& morphWeights, const std::set<int>* onlyBones) {
    VpdPose result;
    result.modelName = model.name;
    int n = (int)model.bones.size();
    if ((int)current.size() == n) {
        if (onlyBones) {
            for (int b : *onlyBones) {
                if (b >= 0 && b < n) {
                    result.bones.push_back({model.bones[b].name, current[b].t, current[b].r});
                }
            }
        } else {
            for (int b = 0; b < n; ++b) {
                if ((model.bones[b].flags & (PmxBone_Rotatable | PmxBone_Movable)) && !IsIdentity(current[b])) {
                    result.bones.push_back({model.bones[b].name, current[b].t, current[b].r});
                }
            }
        }
    }
    if (!onlyBones && morphWeights.size() == model.morphs.size()) {
        for (size_t i = 0; i < model.morphs.size(); ++i) {
            if (morphWeights[i] != 0.0f) {
                result.morphs.push_back({model.morphs[i].name, morphWeights[i]});
            }
        }
    }
    return result;
}

int ApplyVpdPose(const PmxModel& model, const VpdPose& pose, const std::set<int>* onlyBones, PoseLayer& out, std::vector<std::string>* missing) {
    int count = 0;
    for (const auto& b : pose.bones) {
        int idx = model.FindBone(b.name);
        if (idx < 0) {
            if (missing) missing->push_back(b.name);
            continue;
        }
        if (onlyBones && !onlyBones->count(idx)) continue;
        float lenSq = b.rotation.x * b.rotation.x + b.rotation.y * b.rotation.y + b.rotation.z * b.rotation.z + b.rotation.w * b.rotation.w;
        DirectX::XMFLOAT4 r = b.rotation;
        if (lenSq < 1e-8f) {
            r = {0, 0, 0, 1};
        } else {
            float invLen = 1.0f / std::sqrt(lenSq);
            r.x *= invLen; r.y *= invLen; r.z *= invLen; r.w *= invLen;
        }
        out.bones[idx] = {b.translation, r};
        ++count;
    }
    if (!onlyBones) {
        for (const auto& m : pose.morphs) {
            int idx = model.FindMorph(m.name);
            if (idx < 0) {
                if (missing) missing->push_back(m.name);
            } else {
                out.morphs[idx] = m.weight;
                ++count;
            }
        }
    }
    return count;
}

} // namespace mmdx::studio
