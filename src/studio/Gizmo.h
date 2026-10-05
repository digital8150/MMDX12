#pragma once
// Viewport overlays for pose editing: projection helpers, the MMD-style bone overlay (joints + links), bone picking,
// and the translate/rotate gizmo (drawing, hit testing, drag solving). Pure ImGui draw-list code: no App state.
// World space is MMD space (left-handed, +Y up); matrices are DirectXMath row-vector (p' = p * M).
#include "imgui.h"
#include <DirectXMath.h>
#include <cstdint>
#include <set>

namespace mmdx {
class ModelInstance;
struct PmxModel;
}

namespace mmdx::studio {

// The viewport camera, for projecting world points into screen pixels.
struct ViewProj {
    DirectX::XMFLOAT4X4 view{};      // LH view matrix
    DirectX::XMFLOAT4X4 viewProj{};  // view * perspective(fovY, w / h, nearZ, farZ)
    DirectX::XMFLOAT3 eye{};
    float fovY = 0.5f, nearZ = 0.5f, farZ = 3000.0f;
    float x0 = 0, y0 = 0, w = 1, h = 1;  // viewport rectangle in screen pixels

    // Screen position of `p`. False when p is behind the camera (clip w <= nearZ * 0.5). `depth` = view-space z.
    bool Project(const DirectX::XMFLOAT3& p, ImVec2& out, float* depth = nullptr) const;
    // World length of one screen pixel at the depth of `p` (2 * z * tan(fovY / 2) / h, z = view-space depth,
    // at least nearZ). Keeps gizmos a constant screen size.
    float PixelWorldSize(const DirectX::XMFLOAT3& p) const;
};
ViewProj MakeViewProj(const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT3& eye, float fovY, float nearZ, float farZ,
                      float x0, float y0, float w, float h);

// ---- bone overlay -----------------------------------------------------------------------------------------------

struct BoneOverlayStyle {
    float jointRadius = 4.0f;    // screen px (caller scales by DPI)
    float linkWidth = 1.5f;
    float pickRadius = 9.0f;
    ImU32 normal = IM_COL32(90, 170, 255, 235);     // rotatable bones
    ImU32 movable = IM_COL32(120, 220, 140, 235);   // movable (square joint)
    ImU32 ik = IM_COL32(255, 150, 40, 245);         // IK bones (orange)
    ImU32 selected = IM_COL32(255, 70, 70, 255);    // selected bones
    ImU32 active = IM_COL32(255, 220, 60, 255);     // the active (gizmo) bone
    ImU32 hovered = IM_COL32(255, 255, 255, 255);   // hover ring
    ImU32 outline = IM_COL32(10, 14, 20, 200);      // dark outline behind everything (contrast on bright scenes)
};

// A bone is shown when it has PmxBone_Visible and (PmxBone_Rotatable or PmxBone_Movable or PmxBone_IK), and no
// simulated rigid body (physicsMode 1/2) drives it.
bool BoneShownInOverlay(const PmxModel& model, int bone);
// World position of a bone's joint and of its tail (link end). Uses the skinning matrices, so the model's display
// scale is included. Tail: the tail bone's joint (PmxBone_TailIsBone, valid index), else rest position + tailOffset
// transformed by the bone; false when the bone has no tail (TailIsBone with index -1, or a zero offset).
DirectX::XMFLOAT3 BoneJointWorld(const PmxModel& model, const ModelInstance& inst, int bone);
bool BoneTailWorld(const PmxModel& model, const ModelInstance& inst, int bone, DirectX::XMFLOAT3& tail);

// Draws links (joint -> tail, tapered: width linkWidth at the joint, 0.5 px at the tail, with the outline colour
// drawn 1.5 px wider underneath) and joints (circle for rotate-only bones, square for movable bones; IK bones use the
// ik colour; selected / active override the colour; `hovered` gets an extra ring). Bones behind the camera are skipped.
// Draws into `dl` as-is (the caller sets the clip rect).
void DrawBoneOverlay(ImDrawList* dl, const ViewProj& vp, const PmxModel& model, const ModelInstance& inst,
                     const std::set<int>& selected, int active, int hovered, const BoneOverlayStyle& style);
// The shown bone whose projected joint is nearest to `mouse` within style.pickRadius px; ties within 1 px go to
// operable bones (PmxBone_Operable and listed in a display frame) first, then to the bone nearer the camera. -1 if none.
int PickBone(const ViewProj& vp, const PmxModel& model, const ModelInstance& inst, ImVec2 mouse,
             const BoneOverlayStyle& style);

// ---- gizmo ------------------------------------------------------------------------------------------------------

enum class GizmoMode { Rotate, Translate };
enum class GizmoPart : int { None = 0, AxisX, AxisY, AxisZ, PlaneYZ, PlaneZX, PlaneXY, RingX, RingY, RingZ };

struct GizmoFrame {
    DirectX::XMFLOAT3 center{};                     // world
    DirectX::XMFLOAT3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};  // unit world axes (orthonormal)
    bool enabled[3] = {true, true, true};          // per axis: rings / arrows shown and usable (fixed-axis bones: one)
};

struct GizmoStyle {
    float ringRadius = 70.0f;    // screen px (caller scales by DPI)
    float arrowLength = 80.0f;
    float planeOffset = 16.0f;   // plane handle square spans [planeOffset, planeOffset + planeSize] along both axes
    float planeSize = 18.0f;
    float hitDistance = 7.0f;
    float thickness = 2.5f;
    ImU32 axisColor[3] = {IM_COL32(235, 75, 75, 255), IM_COL32(110, 205, 70, 255), IM_COL32(70, 130, 245, 255)};
    ImU32 hotColor = IM_COL32(255, 220, 60, 255);
    ImU32 outerColor = IM_COL32(255, 255, 255, 110);  // view-aligned circle around the rotate rings (decoration)
};

// Part under the mouse, None if nothing is within style.hitDistance. Rotate mode tests the rings (front half
// preferred: when both a front and a back sample are in range, the front one wins), Translate mode tests the plane
// squares first, then the arrows. Disabled axes, and arrows/planes whose screen projection is degenerate (axis screen
// length < 8 px; plane area < 40 px^2) are skipped.
GizmoPart GizmoHitTest(const ViewProj& vp, const GizmoFrame& f, GizmoMode mode, const GizmoStyle& s, ImVec2 mouse);
// Rotate: three rings (circle of ringRadius px around the axis; the half facing away from the eye drawn at 35% alpha)
// plus the outer circle. Translate: arrows (line + filled triangle head) and plane squares (filled at 25% alpha,
// outlined). `hot` / `active` parts use hotColor.
void DrawGizmo(ImDrawList* dl, const ViewProj& vp, const GizmoFrame& f, GizmoMode mode, const GizmoStyle& s,
               GizmoPart hot, GizmoPart active);

// State captured when a drag starts; the solvers below return the TOTAL change since the start.
struct GizmoDrag {
    GizmoPart part = GizmoPart::None;
    GizmoFrame frame;          // gizmo frame at the start
    ImVec2 startMouse{};
    float worldLength = 1.0f;  // world length of arrowLength px at the start (translate)
    ImVec2 screenA{}, screenB{};  // translate: screen delta of (axis * worldLength) for the dragged axis (A) / plane axes (A, B)
    ImVec2 tangent{};          // rotate: unit screen direction in which the grab point moves for a positive angle
    float radiusPx = 1.0f;     // rotate: ring radius in px
    bool screenAngle = false;  // rotate: tangent degenerate -> use the mouse angle around the projected centre
    ImVec2 centerPx{};
    float angleSign = 1.0f;    // rotate, screenAngle mode
};
GizmoDrag BeginGizmoDrag(const ViewProj& vp, const GizmoFrame& f, GizmoMode mode, const GizmoStyle& s, GizmoPart part,
                         ImVec2 mouse);
// World displacement for Axis*/Plane* parts (zero for others).
DirectX::XMFLOAT3 GizmoDragTranslation(const GizmoDrag& d, ImVec2 mouse);
// Rotation angle in radians about the ring's world axis (written to `axis`) for Ring* parts (0 for others). A positive
// angle means XMQuaternionRotationAxis(axis, angle) rotates the grabbed ring point towards where the mouse moved.
float GizmoDragAngle(const GizmoDrag& d, ImVec2 mouse, DirectX::XMFLOAT3* axis);

} // namespace mmdx::studio
