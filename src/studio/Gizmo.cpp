// Viewport overlays for pose editing: projection helpers, the MMD-style bone overlay, bone picking,
// and the translate/rotate gizmo. Pure ImGui draw-list code (see Gizmo.h for the contract).
#define IMGUI_DEFINE_MATH_OPERATORS
#include "studio/Gizmo.h"
#include "anim/ModelInstance.h"
#include "asset/PmxModel.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace mmdx::studio {

using namespace DirectX;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kRingSamples = 64;
constexpr float kNoDistance = 1e30f;

XMVECTOR Load(const XMFLOAT3& v) { return XMLoadFloat3(&v); }
XMFLOAT3 Store(XMVECTOR v) { XMFLOAT3 r; XMStoreFloat3(&r, v); return r; }

float DotV(const ImVec2& a, const ImVec2& b) { return a.x * b.x + a.y * b.y; }
float LenV(const ImVec2& v) { return std::sqrt(DotV(v, v)); }

float WrapPi(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

float SegmentDistance(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    if (len2 <= 1e-9f) return std::hypot(p.x - a.x, p.y - a.y);
    float t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
    t = std::clamp(t, 0.0f, 1.0f);
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

ImU32 WithAlpha(ImU32 col, uint32_t alpha) { return (col & 0x00FFFFFFu) | (alpha << 24); }
ImU32 AlphaScaled(ImU32 col, float f) { return WithAlpha(col, (uint32_t)(((col >> 24) & 0xFFu) * f)); }

int AxisIndexOf(GizmoPart p) {
    switch (p) {
    case GizmoPart::AxisX: return 0;
    case GizmoPart::AxisY: return 1;
    case GizmoPart::AxisZ: return 2;
    default: return -1;
    }
}

int RingIndexOf(GizmoPart p) {
    switch (p) {
    case GizmoPart::RingX: return 0;
    case GizmoPart::RingY: return 1;
    case GizmoPart::RingZ: return 2;
    default: return -1;
    }
}

// A and B are the two axes spanning the plane handle, in XY / ZX / YZ order.
bool PlaneAxesOf(GizmoPart p, int& a, int& b) {
    switch (p) {
    case GizmoPart::PlaneYZ: a = 1; b = 2; return true;
    case GizmoPart::PlaneZX: a = 2; b = 0; return true;
    case GizmoPart::PlaneXY: a = 0; b = 1; return true;
    default: return false;
    }
}

// C + axes[i] * dist.
XMFLOAT3 AlongAxis(const GizmoFrame& f, int i, float dist) {
    return Store(XMVectorAdd(Load(f.center), XMVectorScale(Load(f.axes[i]), dist)));
}

// World point on the ring of `axis` at `angle` (U = next axis, V = the one after; orthonormal frame).
XMFLOAT3 RingPoint(const GizmoFrame& f, int axis, float angle, float radiusWorld) {
    const XMVECTOR u = Load(f.axes[(axis + 1) % 3]);
    const XMVECTOR v = Load(f.axes[(axis + 2) % 3]);
    const XMVECTOR r = XMVectorAdd(XMVectorScale(u, std::cos(angle)), XMVectorScale(v, std::sin(angle)));
    return Store(XMVectorAdd(Load(f.center), XMVectorScale(r, radiusWorld)));
}

// A ring sample is front when (point - center) . (eye - center) >= 0.
bool FacingEye(const ViewProj& vp, const XMFLOAT3& p, const XMFLOAT3& center) {
    return XMVectorGetX(XMVector3Dot(XMVectorSubtract(Load(p), Load(center)),
                                     XMVectorSubtract(Load(vp.eye), Load(center)))) >= 0.0f;
}

} // namespace

// ---- projection -------------------------------------------------------------------------------------------------

ViewProj MakeViewProj(const XMFLOAT4X4& view, const XMFLOAT3& eye, float fovY, float nearZ, float farZ,
                      float x0, float y0, float w, float h) {
    ViewProj vp;
    vp.view = view;
    vp.eye = eye;
    vp.fovY = fovY;
    vp.nearZ = nearZ;
    vp.farZ = farZ;
    vp.x0 = x0;
    vp.y0 = y0;
    vp.w = w > 0.0f ? w : 1.0f;
    vp.h = h > 0.0f ? h : 1.0f;
    const XMMATRIX v = XMLoadFloat4x4(&vp.view);
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(fovY, vp.w / vp.h, nearZ, farZ);
    XMStoreFloat4x4(&vp.viewProj, XMMatrixMultiply(v, proj));
    return vp;
}

ViewProj MakeOrthoViewProj(const XMFLOAT4X4& view, const XMFLOAT3& eye, float orthoHeight, float nearZ, float farZ,
                           float x0, float y0, float w, float h) {
    ViewProj vp;
    vp.view = view;
    vp.eye = eye;
    vp.nearZ = nearZ;
    vp.farZ = farZ;
    vp.x0 = x0;
    vp.y0 = y0;
    vp.w = w > 0.0f ? w : 1.0f;
    vp.h = h > 0.0f ? h : 1.0f;
    vp.ortho = true;
    vp.orthoHeight = orthoHeight;
    const XMMATRIX v = XMLoadFloat4x4(&vp.view);
    const XMMATRIX proj = XMMatrixOrthographicLH(orthoHeight * vp.w / vp.h, orthoHeight, nearZ, farZ);
    XMStoreFloat4x4(&vp.viewProj, XMMatrixMultiply(v, proj));
    return vp;
}

void OrthoViewMatrix(int kind, const XMFLOAT3& center, XMFLOAT4X4* view, XMFLOAT3* eye) {
    XMFLOAT3 dir{0, 0, -1};  // from the target to the eye
    XMFLOAT3 up{0, 1, 0};
    if (kind == 0) { dir = {0, 1, 0}; up = {0, 0, 1}; }
    else if (kind == 2) { dir = {1, 0, 0}; }
    const XMFLOAT3 e{center.x + dir.x * kOrthoEyeDistance, center.y + dir.y * kOrthoEyeDistance,
                     center.z + dir.z * kOrthoEyeDistance};
    if (eye != nullptr) *eye = e;
    if (view != nullptr) XMStoreFloat4x4(view, XMMatrixLookAtLH(Load(e), Load(center), Load(up)));
}

bool ViewProj::Project(const XMFLOAT3& p, ImVec2& out, float* depth) const {
    const XMVECTOR clip =
        XMVector4Transform(XMVectorSetW(Load(p), 1.0f), XMLoadFloat4x4(&viewProj));
    const float cw = XMVectorGetW(clip);
    if (cw <= nearZ * 0.5f) return false;
    out.x = x0 + (XMVectorGetX(clip) / cw * 0.5f + 0.5f) * w;
    out.y = y0 + (0.5f - XMVectorGetY(clip) / cw * 0.5f) * h;
    if (depth != nullptr)
        *depth = XMVectorGetZ(XMVector3TransformCoord(Load(p), XMLoadFloat4x4(&view)));
    return true;
}

float ViewProj::PixelWorldSize(const XMFLOAT3& p) const {
    if (ortho) return orthoHeight / h;
    const float z = std::max(XMVectorGetZ(XMVector3TransformCoord(Load(p), XMLoadFloat4x4(&view))), nearZ);
    return 2.0f * z * std::tan(fovY * 0.5f) / h;
}

// ---- bone overlay -----------------------------------------------------------------------------------------------

bool BoneShownInOverlay(const PmxModel& model, int bone) {
    if (bone < 0 || bone >= (int)model.bones.size()) return false;
    const uint16_t flags = model.bones[bone].flags;
    if (!(flags & PmxBone_Visible) || !(flags & (PmxBone_Rotatable | PmxBone_Movable | PmxBone_IK))) return false;
    // bones driven by a simulated rigid body (hair, skirts) cannot be posed by hand: keep the overlay readable
    for (const PmxRigidBody& rb : model.rigidBodies)
        if (rb.boneIndex == bone && rb.physicsMode != 0) return false;
    return true;
}

XMFLOAT3 BoneJointWorld(const PmxModel& model, const ModelInstance& inst, int bone) {
    if (bone < 0 || bone >= (int)model.bones.size()) return {0, 0, 0};
    const auto& skin = inst.SkinMatrices();
    return Store(XMVector3TransformCoord(Load(model.bones[bone].position),
                                         XMLoadFloat4x4(&skin[bone])));
}

bool BoneTailWorld(const PmxModel& model, const ModelInstance& inst, int bone, XMFLOAT3& tail) {
    const int n = (int)model.bones.size();
    if (bone < 0 || bone >= n) return false;
    const PmxBone& b = model.bones[bone];
    const auto& skin = inst.SkinMatrices();
    if (b.flags & PmxBone_TailIsBone) {
        const int t = b.tailBoneIndex;
        if (t < 0 || t >= n || t == bone) return false;
        tail = BoneJointWorld(model, inst, t);
        return true;
    }
    if (std::fabs(b.tailOffset.x) < 1e-6f && std::fabs(b.tailOffset.y) < 1e-6f &&
        std::fabs(b.tailOffset.z) < 1e-6f)
        return false;
    tail = Store(XMVector3TransformCoord(XMVectorAdd(Load(b.position), Load(b.tailOffset)),
                                         XMLoadFloat4x4(&skin[bone])));
    return true;
}

// Screen-pixel radius of a bone's joint marker. Markers scale with distance like 3ds Max / Cinema 4D: a joint is
// jointRadius px at kJointRefDistance world units from the camera, shrinking when farther and growing when nearer,
// clamped so markers never vanish or blow up. Ortho views have no perspective: constant screen size.
constexpr float kJointRefDistance = 40.0f;
float JointPixelRadius(const ViewProj& vp, float depth, const BoneOverlayStyle& style) {
    if (vp.ortho || depth <= 0.0f) return style.jointRadius;
    return std::clamp(style.jointRadius * kJointRefDistance / depth, style.minPixelRadius, style.maxPixelRadius);
}

void DrawBoneOverlay(ImDrawList* dl, const ViewProj& vp, const PmxModel& model, const ModelInstance& inst,
                     const std::set<int>& selected, int active, int hovered, const BoneOverlayStyle& style) {
    const auto& bones = model.bones;

    const auto colorOf = [&](int b) -> ImU32 {
        if (active == b) return style.active;
        if (selected.count(b)) return style.selected;
        const uint16_t f = bones[b].flags;
        if (f & PmxBone_IK) return style.ik;
        if (f & PmxBone_Movable) return style.movable;
        return style.normal;
    };

    // Links first, then joints, so joints are drawn on top.
    for (int b = 0; b < (int)bones.size(); ++b) {
        if (!BoneShownInOverlay(model, b)) continue;
        XMFLOAT3 tail;
        if (!BoneTailWorld(model, inst, b, tail)) continue;
        const XMFLOAT3 joint = BoneJointWorld(model, inst, b);
        ImVec2 j, t;
        if (!vp.Project(joint, j) || !vp.Project(tail, t)) continue;
        const ImVec2 d = t - j;
        if (LenV(d) < 2.0f) continue;
        const ImVec2 n = ImVec2(-d.y, d.x) / LenV(d);
        dl->AddQuadFilled(j + n * (style.linkWidth + 1.5f), j - n * (style.linkWidth + 1.5f),
                          t - n * 2.0f, t + n * 2.0f, style.outline);
        const ImU32 colour = colorOf(b);
        dl->AddQuadFilled(j + n * style.linkWidth, j - n * style.linkWidth,
                          t - n * 0.5f, t + n * 0.5f, colour);
    }
    for (int b = 0; b < (int)bones.size(); ++b) {
        if (!BoneShownInOverlay(model, b)) continue;
        const XMFLOAT3 joint = BoneJointWorld(model, inst, b);
        ImVec2 c;
        if (!vp.Project(joint, c)) continue;
        const float r = JointPixelRadius(vp, std::max(XMVectorGetZ(XMVector3TransformCoord(Load(joint), XMLoadFloat4x4(&vp.view))), 0.0f), style);
        const bool movable = (bones[b].flags & PmxBone_Movable) != 0;
        const bool ik = (bones[b].flags & PmxBone_IK) != 0;
        const ImU32 colour = colorOf(b);
        if (movable && !ik) {
            dl->AddRectFilled(c - ImVec2(r + 1.5f, r + 1.5f), c + ImVec2(r + 1.5f, r + 1.5f), style.outline);
            dl->AddRectFilled(c - ImVec2(r, r), c + ImVec2(r, r), colour);
        } else {
            dl->AddCircleFilled(c, r + 1.5f, style.outline, 16);
            dl->AddCircleFilled(c, r, colour, 16);
        }
        if (b == hovered) dl->AddCircle(c, r + 4.0f, style.hovered, 20, 1.5f);
    }
}

int PickBone(const ViewProj& vp, const PmxModel& model, const ModelInstance& inst, ImVec2 mouse,
             const BoneOverlayStyle& style) {
    struct Candidate {
        float d, depth;
        int bone;
    };
    std::vector<Candidate> candidates;
    for (int b = 0; b < (int)model.bones.size(); ++b) {
        if (!BoneShownInOverlay(model, b)) continue;
        const XMFLOAT3 joint = BoneJointWorld(model, inst, b);
        ImVec2 c;
        float depth = 0.0f;
        if (!vp.Project(joint, c, &depth)) continue;
        const float d = std::hypot(mouse.x - c.x, mouse.y - c.y);
        // the pick radius follows the drawn marker size, with the base pickRadius as the floor
        const float r = std::max(style.pickRadius, JointPixelRadius(vp, depth, style));
        if (d <= r) candidates.push_back({d, depth, b});
    }
    if (candidates.empty()) return -1;
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.d < b.d; });
    const float bestD = candidates.front().d;
    const Candidate* best = &candidates.front();
    // ties (joints on top of each other, e.g. helper bones at the head): operable bones first, then the nearer one
    // ("operable" = listed in a display frame, i.e. a bone the model offers for keying, and the Operable flag)
    std::vector<char> listed(model.bones.size(), 0);
    for (const PmxDisplayFrame& df : model.displayFrames)
        for (const auto& it : df.items)
            if (!it.morph && it.index >= 0 && (size_t)it.index < listed.size()) listed[(size_t)it.index] = 1;
    const auto operable = [&](const Candidate& c) {
        return (listed[(size_t)c.bone] || model.displayFrames.empty()) && (model.bones[c.bone].flags & PmxBone_Operable) != 0;
    };
    for (const Candidate& c : candidates) {
        if (c.d > bestD + 1.0f) break;  // sorted: no closer tie can follow
        if (operable(c) != operable(*best)) {
            if (operable(c)) best = &c;
        } else if (c.depth < best->depth) {
            best = &c;
        }
    }
    return best->bone;
}

// ---- gizmo ------------------------------------------------------------------------------------------------------

GizmoPart GizmoHitTest(const ViewProj& vp, const GizmoFrame& f, GizmoMode mode, const GizmoStyle& s, ImVec2 mouse) {
    static const GizmoPart kRings[3] = {GizmoPart::RingX, GizmoPart::RingY, GizmoPart::RingZ};
    static const GizmoPart kAxes[3] = {GizmoPart::AxisX, GizmoPart::AxisY, GizmoPart::AxisZ};
    static const struct {
        GizmoPart part;
        int a, b;
    } kPlanes[3] = {{GizmoPart::PlaneYZ, 1, 2}, {GizmoPart::PlaneZX, 2, 0}, {GizmoPart::PlaneXY, 0, 1}};

    ImVec2 cp;
    if (!vp.Project(f.center, cp)) return GizmoPart::None;
    const float scale = vp.PixelWorldSize(f.center);

    if (mode == GizmoMode::Rotate) {
        GizmoPart best = GizmoPart::None;
        float bestDist = kNoDistance;
        for (int i = 0; i < 3; ++i) {
            if (!f.enabled[i]) continue;
            const float radiusWorld = s.ringRadius * scale;
            ImVec2 pts[kRingSamples];
            bool ok[kRingSamples];
            bool front[kRingSamples];
            for (int k = 0; k < kRingSamples; ++k) {
                const XMFLOAT3 p = RingPoint(f, i, 2.0f * kPi * k / kRingSamples, radiusWorld);
                ok[k] = vp.Project(p, pts[k]);
                front[k] = ok[k] && FacingEye(vp, p, f.center);
            }
            float frontMin = kNoDistance, otherMin = kNoDistance;
            for (int k = 0; k < kRingSamples; ++k) {
                const int k2 = (k + 1) % kRingSamples;
                if (!ok[k] || !ok[k2]) continue;
                const float d = SegmentDistance(mouse, pts[k], pts[k2]);
                if (front[k] && front[k2]) frontMin = std::min(frontMin, d);
                else otherMin = std::min(otherMin, d);
            }
            // Front half preferred: when both a front and a back sample are in range, the front one wins.
            // On an exact tie the later ring wins (rings degenerating to the same screen point).
            const float cand = frontMin <= s.hitDistance ? frontMin : otherMin;
            if (cand <= s.hitDistance && cand <= bestDist) {
                bestDist = cand;
                best = kRings[i];
            }
        }
        return best;
    }

    // Translate: plane handles first, then arrows.
    for (const auto& pl : kPlanes) {
        if (!f.enabled[pl.a] || !f.enabled[pl.b]) continue;
        XMFLOAT3 wc[4];
        const float a0 = s.planeOffset * scale;
        const float a1 = (s.planeOffset + s.planeSize) * scale;
        const XMVECTOR c = Load(f.center);
        const XMVECTOR A = Load(f.axes[pl.a]);
        const XMVECTOR B = Load(f.axes[pl.b]);
        wc[0] = Store(XMVectorAdd(c, XMVectorAdd(XMVectorScale(A, a0), XMVectorScale(B, a0))));
        wc[1] = Store(XMVectorAdd(c, XMVectorAdd(XMVectorScale(A, a1), XMVectorScale(B, a0))));
        wc[2] = Store(XMVectorAdd(c, XMVectorAdd(XMVectorScale(A, a1), XMVectorScale(B, a1))));
        wc[3] = Store(XMVectorAdd(c, XMVectorAdd(XMVectorScale(A, a0), XMVectorScale(B, a1))));
        ImVec2 q[4];
        bool ok = true;
        for (int i = 0; i < 4 && ok; ++i) ok = vp.Project(wc[i], q[i]);
        if (!ok) continue;
        float area2 = 0.0f;
        for (int i = 0; i < 4; ++i) {
            const int j = (i + 1) % 4;
            area2 += q[i].x * q[j].y - q[j].x * q[i].y;
        }
        if (std::fabs(area2) * 0.5f < 40.0f) continue;  // degenerate (edge-on) plane
        int pos = 0, neg = 0;
        for (int i = 0; i < 4; ++i) {
            const int j = (i + 1) % 4;
            const float cross =
                (q[j].x - q[i].x) * (mouse.y - q[i].y) - (q[j].y - q[i].y) * (mouse.x - q[i].x);
            if (cross > 0.0f) ++pos;
            else if (cross < 0.0f) ++neg;
        }
        if (pos == 0 || neg == 0) return pl.part;  // inside the convex quad
    }
    GizmoPart best = GizmoPart::None;
    float bestDist = kNoDistance;
    for (int i = 0; i < 3; ++i) {
        if (!f.enabled[i]) continue;
        ImVec2 e;
        if (!vp.Project(AlongAxis(f, i, s.arrowLength * scale), e)) continue;
        if (LenV(e - cp) < 8.0f) continue;  // degenerate (towards/away from the eye) arrow
        const float d = SegmentDistance(mouse, cp, e);
        if (d <= s.hitDistance && d < bestDist) {
            bestDist = d;
            best = kAxes[i];
        }
    }
    return best;
}

void DrawGizmo(ImDrawList* dl, const ViewProj& vp, const GizmoFrame& f, GizmoMode mode, const GizmoStyle& s,
               GizmoPart hot, GizmoPart active) {
    static const GizmoPart kRings[3] = {GizmoPart::RingX, GizmoPart::RingY, GizmoPart::RingZ};
    static const GizmoPart kAxes[3] = {GizmoPart::AxisX, GizmoPart::AxisY, GizmoPart::AxisZ};
    static const struct {
        GizmoPart part;
        int a, b, third;
    } kPlanes[3] = {{GizmoPart::PlaneYZ, 1, 2, 0}, {GizmoPart::PlaneZX, 2, 0, 1}, {GizmoPart::PlaneXY, 0, 1, 2}};

    ImVec2 cp;
    if (!vp.Project(f.center, cp)) return;
    const float scale = vp.PixelWorldSize(f.center);

    if (mode == GizmoMode::Rotate) {
        dl->AddCircle(cp, s.ringRadius + 6.0f, s.outerColor, 64, 1.5f);
        ImVec2 pts[3][kRingSamples];
        bool ok[3][kRingSamples];
        bool front[3][kRingSamples];
        for (int i = 0; i < 3; ++i) {
            if (!f.enabled[i]) continue;
            const float radiusWorld = s.ringRadius * scale;
            for (int k = 0; k < kRingSamples; ++k) {
                const XMFLOAT3 p = RingPoint(f, i, 2.0f * kPi * k / kRingSamples, radiusWorld);
                ok[i][k] = vp.Project(p, pts[i][k]);
                front[i][k] = ok[i][k] && FacingEye(vp, p, f.center);
            }
        }
        for (int pass = 0; pass < 2; ++pass) {  // 0: back half, 1: front half
            for (int i = 0; i < 3; ++i) {
                if (!f.enabled[i]) continue;
                const bool isHot = hot == kRings[i] || active == kRings[i];
                const ImU32 colour = isHot ? s.hotColor : s.axisColor[i];
                const float thickness = isHot ? s.thickness + 1.5f : s.thickness;
                for (int k = 0; k < kRingSamples; ++k) {
                    const int k2 = (k + 1) % kRingSamples;
                    if (!ok[i][k] || !ok[i][k2]) continue;
                    const bool segFront = front[i][k] && front[i][k2];
                    if ((pass == 1) != segFront) continue;
                    dl->AddLine(pts[i][k], pts[i][k2], segFront ? colour : AlphaScaled(colour, 0.35f), thickness);
                }
            }
        }
        return;
    }

    // Translate: arrows, then plane handles, then the centre dot.
    for (int i = 0; i < 3; ++i) {
        if (!f.enabled[i]) continue;
        ImVec2 e;
        if (!vp.Project(AlongAxis(f, i, s.arrowLength * scale), e)) continue;
        if (LenV(e - cp) < 8.0f) continue;
        const bool isHot = hot == kAxes[i] || active == kAxes[i];
        const ImU32 colour = isHot ? s.hotColor : s.axisColor[i];
        dl->AddLine(cp, e, colour, s.thickness);
        const float headScale = s.thickness / 2.5f;
        const ImVec2 d = (e - cp) / LenV(e - cp);
        const ImVec2 n(-d.y, d.x);
        dl->AddTriangleFilled(e + d * (12.0f * headScale), e + n * (5.0f * headScale),
                              e - n * (5.0f * headScale), colour);
    }
    for (const auto& pl : kPlanes) {
        if (!f.enabled[pl.a] || !f.enabled[pl.b]) continue;
        XMFLOAT3 wc[4];
        const float a0 = s.planeOffset * scale;
        const float a1 = (s.planeOffset + s.planeSize) * scale;
        const XMVECTOR c = Load(f.center);
        const XMVECTOR A = Load(f.axes[pl.a]);
        const XMVECTOR B = Load(f.axes[pl.b]);
        wc[0] = Store(XMVectorAdd(c, XMVectorAdd(XMVectorScale(A, a0), XMVectorScale(B, a0))));
        wc[1] = Store(XMVectorAdd(c, XMVectorAdd(XMVectorScale(A, a1), XMVectorScale(B, a0))));
        wc[2] = Store(XMVectorAdd(c, XMVectorAdd(XMVectorScale(A, a1), XMVectorScale(B, a1))));
        wc[3] = Store(XMVectorAdd(c, XMVectorAdd(XMVectorScale(A, a0), XMVectorScale(B, a1))));
        ImVec2 q[4];
        bool ok = true;
        for (int i = 0; i < 4 && ok; ++i) ok = vp.Project(wc[i], q[i]);
        if (!ok) continue;
        const bool isHot = hot == pl.part || active == pl.part;
        const ImU32 base = s.axisColor[pl.third];  // the colour of the axis NOT in the plane
        dl->AddQuadFilled(q[0], q[1], q[2], q[3], isHot ? s.hotColor : WithAlpha(base, 64));
        dl->AddQuad(q[0], q[1], q[2], q[3], isHot ? s.hotColor : WithAlpha(base, 200), 1.5f);
    }
    dl->AddCircleFilled(cp, 3.0f, IM_COL32(255, 255, 255, 220));
}

// ---- drag -------------------------------------------------------------------------------------------------------

GizmoDrag BeginGizmoDrag(const ViewProj& vp, const GizmoFrame& f, GizmoMode mode, const GizmoStyle& s, GizmoPart part,
                         ImVec2 mouse) {
    (void)mode;
    GizmoDrag d;
    d.part = part;
    d.frame = f;
    d.startMouse = mouse;
    d.worldLength = s.arrowLength * vp.PixelWorldSize(f.center);
    ImVec2 cp;
    if (!vp.Project(f.center, cp)) {
        d.part = GizmoPart::None;
        return d;
    }
    d.centerPx = cp;
    const float scale = vp.PixelWorldSize(f.center);

    const int axis = AxisIndexOf(part);
    if (axis >= 0) {
        ImVec2 e;
        d.screenA = vp.Project(AlongAxis(f, axis, d.worldLength), e) ? e - cp : ImVec2(0, 0);
        return d;
    }
    int a = 0, b = 0;
    if (PlaneAxesOf(part, a, b)) {
        ImVec2 ea, eb;
        d.screenA = vp.Project(AlongAxis(f, a, d.worldLength), ea) ? ea - cp : ImVec2(0, 0);
        d.screenB = vp.Project(AlongAxis(f, b, d.worldLength), eb) ? eb - cp : ImVec2(0, 0);
        return d;
    }
    const int ring = RingIndexOf(part);
    if (ring < 0) return d;

    d.radiusPx = s.ringRadius;
    const float radiusWorld = s.ringRadius * scale;
    float bestD = kNoDistance;
    XMFLOAT3 grab = f.center;
    for (int k = 0; k < kRingSamples; ++k) {
        const XMFLOAT3 p = RingPoint(f, ring, 2.0f * kPi * k / kRingSamples, radiusWorld);
        ImVec2 q;
        if (!vp.Project(p, q)) continue;
        const float dist = std::hypot(mouse.x - q.x, mouse.y - q.y);
        if (dist < bestD) {
            bestD = dist;
            grab = p;
        }
    }
    const XMVECTOR r = XMVectorSubtract(Load(grab), Load(f.center));
    const XMVECTOR axisV = Load(f.axes[ring]);
    const XMFLOAT3 pr = Store(XMVectorAdd(Load(f.center), r));
    const XMVECTOR r2 = XMVector3Rotate(r, XMQuaternionRotationAxis(axisV, 0.01f));
    const XMFLOAT3 pr2 = Store(XMVectorAdd(Load(f.center), r2));
    ImVec2 s0, s1;
    if (vp.Project(pr, s0) && vp.Project(pr2, s1)) {
        const ImVec2 sd = s1 - s0;
        if (LenV(sd) >= 1e-3f * d.radiusPx) {
            d.tangent = sd / LenV(sd);
            d.screenAngle = false;
            return d;
        }
    }
    // Edge-on ring at the grab point: fall back to the mouse angle around the projected centre,
    // with the sign chosen so a positive angle still moves the grab point with the mouse.
    d.screenAngle = true;
    const XMVECTOR r3 = XMVector3Rotate(r, XMQuaternionRotationAxis(axisV, 0.2f));
    const XMFLOAT3 pr3 = Store(XMVectorAdd(Load(f.center), r3));
    ImVec2 s2, s3;
    if (vp.Project(pr, s2) && vp.Project(pr3, s3)) {
        const float a0 = std::atan2(s2.y - cp.y, s2.x - cp.x);
        const float a1 = std::atan2(s3.y - cp.y, s3.x - cp.x);
        d.angleSign = WrapPi(a1 - a0) > 0.0f ? 1.0f : -1.0f;
    } else {
        d.angleSign = 1.0f;
    }
    return d;
}

DirectX::XMFLOAT3 GizmoDragTranslation(const GizmoDrag& d, ImVec2 mouse) {
    const int axis = AxisIndexOf(d.part);
    const ImVec2 delta = mouse - d.startMouse;
    if (axis >= 0) {
        const float den = DotV(d.screenA, d.screenA);
        if (den < 1e-6f) return {0, 0, 0};
        const float u = DotV(delta, d.screenA) / den;
        return Store(XMVectorScale(Load(d.frame.axes[axis]), u * d.worldLength));
    }
    int a = 0, b = 0;
    if (PlaneAxesOf(d.part, a, b)) {
        // Solve [screenA screenB] * (u, v) = delta.
        const float det = d.screenA.x * d.screenB.y - d.screenA.y * d.screenB.x;
        if (std::fabs(det) < 1e-6f) return {0, 0, 0};
        const float u = (delta.x * d.screenB.y - d.screenB.x * delta.y) / det;
        const float v = (d.screenA.x * delta.y - delta.x * d.screenA.y) / det;
        const XMVECTOR t = XMVectorAdd(XMVectorScale(Load(d.frame.axes[a]), u),
                                       XMVectorScale(Load(d.frame.axes[b]), v));
        return Store(XMVectorScale(t, d.worldLength));
    }
    return {0, 0, 0};
}

float GizmoDragAngle(const GizmoDrag& d, ImVec2 mouse, XMFLOAT3* axis) {
    const int ring = RingIndexOf(d.part);
    if (ring < 0) return 0.0f;
    if (axis != nullptr) *axis = d.frame.axes[ring];
    if (d.screenAngle) {
        const float a0 = std::atan2(d.startMouse.y - d.centerPx.y, d.startMouse.x - d.centerPx.x);
        const float a1 = std::atan2(mouse.y - d.centerPx.y, mouse.x - d.centerPx.x);
        return d.angleSign * WrapPi(a1 - a0);
    }
    return DotV(mouse - d.startMouse, d.tangent) / d.radiusPx;
}

// ---- camera path ------------------------------------------------------------------------------------------------

bool ProjectSegment(const ViewProj& vp, const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, ImVec2& pa, ImVec2& pb) {
    const float za = XMVectorGetZ(XMVector3TransformCoord(Load(a), XMLoadFloat4x4(&vp.view)));
    const float zb = XMVectorGetZ(XMVector3TransformCoord(Load(b), XMLoadFloat4x4(&vp.view)));
    const float clipZ = vp.nearZ * 1.001f;
    if (za < clipZ && zb < clipZ) return false;

    XMFLOAT3 ca = a;
    XMFLOAT3 cb = b;
    if (za < clipZ) {
        const float t = (clipZ - za) / (zb - za);
        ca.x = a.x + t * (b.x - a.x);
        ca.y = a.y + t * (b.y - a.y);
        ca.z = a.z + t * (b.z - a.z);
    } else if (zb < clipZ) {
        const float t = (clipZ - za) / (zb - za);
        cb.x = a.x + t * (b.x - a.x);
        cb.y = a.y + t * (b.y - a.y);
        cb.z = a.z + t * (b.z - a.z);
    }
    return vp.Project(ca, pa) && vp.Project(cb, pb);
}

void DrawCameraPath(ImDrawList* dl, const ViewProj& vp, const CameraPathPoint* path, int pathCount,
                    const DirectX::XMFLOAT3* keys, int keyCount, const std::set<int>& selectedKeys,
                    const CameraPathPoint* current, float aspect, float frustumLength, float cutDistance,
                    const CameraPathStyle& style) {
    const auto dist2 = [](const XMFLOAT3& a, const XMFLOAT3& b) {
        return (a.x - b.x)*(a.x - b.x) + (a.y - b.y)*(a.y - b.y) + (a.z - b.z)*(a.z - b.z);
    };
    const float cutSq = cutDistance * cutDistance;

    for (int pass = 0; pass < 2; ++pass) {
        const float thick = pass == 0 ? style.lineWidth + 2.0f : style.lineWidth;
        const ImU32 col = pass == 0 ? style.outline : style.path;
        for (int i = 0; i < pathCount - 1; ++i) {
            if (dist2(path[i].eye, path[i+1].eye) > cutSq) continue;
            ImVec2 pa, pb;
            if (ProjectSegment(vp, path[i].eye, path[i+1].eye, pa, pb)) {
                dl->AddLine(pa, pb, col, thick);
            }
        }
    }

    if (current) {
        ImVec2 pa, pb;
        if (ProjectSegment(vp, current->eye, current->target, pa, pb)) {
            dl->AddLine(pa, pb, style.target, style.lineWidth);
        }
        ImVec2 pt;
        if (vp.Project(current->target, pt)) {
            dl->AddCircleFilled(pt, 3.0f, style.target);
            dl->AddCircle(pt, 3.0f, style.outline, 0, 1.5f);
        }

        const XMMATRIX invView = XMMatrixInverse(nullptr, XMLoadFloat4x4(&current->view));
        const float hh = frustumLength * std::tan(current->fovY * 0.5f);
        const float hw = hh * aspect;
        const XMFLOAT3 cornersV[5] = {
            {0, 0, 0},
            {-hw, hh, frustumLength},
            {hw, hh, frustumLength},
            {hw, -hh, frustumLength},
            {-hw, -hh, frustumLength}
        };
        XMFLOAT3 cornersW[5];
        for (int i = 0; i < 5; ++i) {
            cornersW[i] = Store(XMVector3TransformCoord(Load(cornersV[i]), invView));
        }
        // the far plane is a translucent face: it shows what the lens covers (only when every corner is in front)
        {
            ImVec2 q[4];
            bool all = true;
            for (int i = 0; i < 4; ++i) all = all && vp.Project(cornersW[i + 1], q[i]);
            if (all) dl->AddConvexPolyFilled(q, 4, (style.current & 0x00FFFFFFu) | (44u << 24));
        }

        for (int pass = 0; pass < 2; ++pass) {
            const float thick = pass == 0 ? style.lineWidth + 2.0f : style.lineWidth;
            const ImU32 col = pass == 0 ? style.outline : style.current;
            for (int i = 1; i <= 4; ++i) {
                if (ProjectSegment(vp, cornersW[0], cornersW[i], pa, pb)) {
                    dl->AddLine(pa, pb, col, thick);
                }
                const int next = i == 4 ? 1 : i + 1;
                if (ProjectSegment(vp, cornersW[i], cornersW[next], pa, pb)) {
                    dl->AddLine(pa, pb, col, thick);
                }
            }
        }

        const XMFLOAT3 triV[3] = {
            {0, hh + hh * 0.2f, frustumLength},
            {-hw * 0.1f, hh, frustumLength},
            {hw * 0.1f, hh, frustumLength}
        };
        XMFLOAT3 triW[3];
        ImVec2 ptri[3];
        bool triOk = true;
        for (int i = 0; i < 3; ++i) {
            triW[i] = Store(XMVector3TransformCoord(Load(triV[i]), invView));
            if (!vp.Project(triW[i], ptri[i])) triOk = false;
        }
        if (triOk) {
            dl->AddTriangleFilled(ptri[0], ptri[1], ptri[2], style.current);
            dl->AddTriangle(ptri[0], ptri[1], ptri[2], style.outline, 1.5f);
        }
    }

    for (int i = 0; i < keyCount; ++i) {
        ImVec2 pk;
        if (vp.Project(keys[i], pk)) {
            const bool sel = selectedKeys.count(i) > 0;
            const float r = sel ? style.keyRadius + 1.5f : style.keyRadius;
            const ImU32 col = sel ? style.selectedKey : style.key;
            dl->AddCircleFilled(pk, r + 1.5f, style.outline);
            dl->AddCircleFilled(pk, r, col);
        }
    }

    if (current) {
        ImVec2 pc;
        if (vp.Project(current->eye, pc)) {
            dl->AddCircleFilled(pc, style.currentRadius + 1.5f, style.outline);
            dl->AddCircleFilled(pc, style.currentRadius, style.current);
        }
    }
}

int PickCameraKey(const ViewProj& vp, const DirectX::XMFLOAT3* keys, int keyCount, ImVec2 mouse, float radius) {
    int best = -1;
    float bestD = radius;
    for (int i = 0; i < keyCount; ++i) {
        ImVec2 pk;
        if (vp.Project(keys[i], pk)) {
            const float d = std::hypot(mouse.x - pk.x, mouse.y - pk.y);
            if (d <= bestD) {
                if (d < bestD || best == -1) {
                    bestD = d;
                    best = i;
                }
            }
        }
    }
    return best;
}

// ---- mouse ray and plane solvers (cone angle, range) --------------------------------------------

bool BuildMouseRay(const ViewProj& vp, ImVec2 mouse, MouseRay& outRay) {
    if (vp.w <= 0.0f || vp.h <= 0.0f) return false;
    const float ndcX = (mouse.x - vp.x0) / vp.w * 2.0f - 1.0f;
    const float ndcY = 1.0f - (mouse.y - vp.y0) / vp.h * 2.0f;

    const XMMATRIX v = XMLoadFloat4x4(&vp.view);
    XMVECTOR det;
    const XMMATRIX invV = XMMatrixInverse(&det, v);
    if (XMVectorGetX(XMVectorIsNaN(det)) != 0 || XMVectorGetX(XMVectorEqual(det, XMVectorZero())) != 0) return false;

    if (!vp.ortho) {
        const float tanHalfFovY = std::tan(vp.fovY * 0.5f);
        const float tanHalfFovX = tanHalfFovY * (vp.w / vp.h);
        const XMVECTOR viewDir = XMVector3Normalize(XMVectorSet(ndcX * tanHalfFovX, ndcY * tanHalfFovY, 1.0f, 0.0f));
        const XMVECTOR worldDir = XMVector3Normalize(XMVector3TransformNormal(viewDir, invV));
        outRay.origin = vp.eye;
        XMStoreFloat3(&outRay.dir, worldDir);
        return true;
    } else {
        const float halfH = vp.orthoHeight * 0.5f;
        const float halfW = halfH * (vp.w / vp.h);
        const XMVECTOR viewDir = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
        const XMVECTOR worldDir = XMVector3Normalize(XMVector3TransformNormal(viewDir, invV));
        const XMVECTOR viewOrigin = XMVectorSet(ndcX * halfW, ndcY * halfH, 0.0f, 1.0f);
        const XMVECTOR worldOrigin = XMVector3TransformCoord(viewOrigin, invV);
        XMStoreFloat3(&outRay.origin, worldOrigin);
        XMStoreFloat3(&outRay.dir, worldDir);
        return true;
    }
}

bool IntersectRayPlane(const MouseRay& ray, const DirectX::XMFLOAT3& planePoint,
                       const DirectX::XMFLOAT3& planeNormal, DirectX::XMFLOAT3& outHit, float* outT) {
    const XMVECTOR N = XMVector3Normalize(XMLoadFloat3(&planeNormal));
    const XMVECTOR P0 = XMLoadFloat3(&planePoint);
    const XMVECTOR R0 = XMLoadFloat3(&ray.origin);
    const XMVECTOR D = XMLoadFloat3(&ray.dir);

    const float denom = XMVectorGetX(XMVector3Dot(D, N));
    if (std::fabs(denom) < 1e-6f) return false;

    const float numer = XMVectorGetX(XMVector3Dot(XMVectorSubtract(P0, R0), N));
    const float t = numer / denom;
    if (t < 0.0f) return false;
    if (outT) *outT = t;

    const XMVECTOR hit = XMVectorAdd(R0, XMVectorScale(D, t));
    XMStoreFloat3(&outHit, hit);
    return true;
}

bool SolveConeAngle(const ViewProj& vp, const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& aim,
                    ImVec2 mouse, float& outOuterAngle) {
    const XMVECTOR posV = XMLoadFloat3(&pos);
    const XMVECTOR aimV = XMLoadFloat3(&aim);
    XMVECTOR axisV = XMVectorSubtract(aimV, posV);
    const float aimDist = XMVectorGetX(XMVector3Length(axisV));
    if (aimDist < 1e-4f) return false;
    axisV = XMVectorScale(axisV, 1.0f / aimDist);

    MouseRay ray;
    if (!BuildMouseRay(vp, mouse, ray)) return false;

    DirectX::XMFLOAT3 norm;
    XMStoreFloat3(&norm, axisV);
    DirectX::XMFLOAT3 hit;
    if (!IntersectRayPlane(ray, aim, norm, hit)) return false;

    const XMVECTOR hitV = XMLoadFloat3(&hit);
    const XMVECTOR offsetV = XMVectorSubtract(hitV, aimV);
    const float r = XMVectorGetX(XMVector3Length(offsetV));

    const float angle = std::atan2(r, aimDist);
    const float minRad = 1.0f * kPi / 180.0f;
    const float maxRad = 89.0f * kPi / 180.0f;
    outOuterAngle = std::clamp(angle, minRad, maxRad);
    return true;
}

bool SolveRangeDistance(const ViewProj& vp, const DirectX::XMFLOAT3& pos, ImVec2 mouse, float& outDistance) {
    const XMVECTOR posV = XMLoadFloat3(&pos);
    XMVECTOR normV;
    if (vp.ortho) {
        const XMMATRIX v = XMLoadFloat4x4(&vp.view);
        const XMMATRIX invV = XMMatrixInverse(nullptr, v);
        normV = XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), invV));
    } else {
        normV = XMVectorSubtract(XMLoadFloat3(&vp.eye), posV);
        if (XMVectorGetX(XMVector3LengthSq(normV)) < 1e-6f) return false;
        normV = XMVector3Normalize(normV);
    }

    MouseRay ray;
    if (!BuildMouseRay(vp, mouse, ray)) return false;

    DirectX::XMFLOAT3 norm;
    XMStoreFloat3(&norm, normV);
    DirectX::XMFLOAT3 hit;
    if (!IntersectRayPlane(ray, pos, norm, hit)) return false;

    const float dist = XMVectorGetX(XMVector3Length(XMVectorSubtract(XMLoadFloat3(&hit), posV)));
    outDistance = std::max(0.1f, dist);
    return true;
}

bool SolveSpotRange(const ViewProj& vp, const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& axis,
                    ImVec2 mouse, float& outRange) {
    XMVECTOR axisV = XMLoadFloat3(&axis);
    if (XMVectorGetX(XMVector3LengthSq(axisV)) < 1e-6f) return false;
    axisV = XMVector3Normalize(axisV);
    const XMVECTOR posV = XMLoadFloat3(&pos);

    XMVECTOR camDir;
    if (vp.ortho) {
        const XMMATRIX v = XMLoadFloat4x4(&vp.view);
        const XMMATRIX invV = XMMatrixInverse(nullptr, v);
        camDir = XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), invV));
    } else {
        camDir = XMVectorSubtract(XMLoadFloat3(&vp.eye), posV);
        if (XMVectorGetX(XMVector3LengthSq(camDir)) > 1e-6f) camDir = XMVector3Normalize(camDir);
        else camDir = axisV;
    }

    const XMVECTOR side = XMVector3Cross(camDir, axisV);
    XMVECTOR planeNorm;
    if (XMVectorGetX(XMVector3LengthSq(side)) > 1e-4f) {
        planeNorm = XMVector3Normalize(XMVector3Cross(axisV, side));
    } else {
        planeNorm = camDir;
    }

    MouseRay ray;
    if (!BuildMouseRay(vp, mouse, ray)) return false;

    DirectX::XMFLOAT3 norm;
    XMStoreFloat3(&norm, planeNorm);
    DirectX::XMFLOAT3 hit;
    if (!IntersectRayPlane(ray, pos, norm, hit)) return false;

    const XMVECTOR vHit = XMVectorSubtract(XMLoadFloat3(&hit), posV);
    const float range = XMVectorGetX(XMVector3Dot(vHit, axisV));
    outRange = std::max(0.1f, range);
    return true;
}

} // namespace mmdx::studio

