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
    bool ortho = false;              // orthographic view (Studio quad view): orthoHeight world units across the height
    float orthoHeight = 1.0f;

    // Screen position of `p`. False when p is behind the camera (clip w <= nearZ * 0.5). `depth` = view-space z.
    bool Project(const DirectX::XMFLOAT3& p, ImVec2& out, float* depth = nullptr) const;
    // World length of one screen pixel at the depth of `p` (2 * z * tan(fovY / 2) / h, z = view-space depth,
    // at least nearZ). Keeps gizmos a constant screen size.
    float PixelWorldSize(const DirectX::XMFLOAT3& p) const;
};
ViewProj MakeViewProj(const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT3& eye, float fovY, float nearZ, float farZ,
                      float x0, float y0, float w, float h);
// Orthographic counterpart for the quad view's top / front / left views (orthoHeight world units across the height).
ViewProj MakeOrthoViewProj(const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT3& eye, float orthoHeight, float nearZ,
                           float farZ, float x0, float y0, float w, float h);
// The quad view's orthographic cameras: kind 0 top (looking down, the model's front at the bottom), 1 front (looking
// along +Z at a model that faces -Z), 2 left (from +X, the model's left side). The eye sits kOrthoEyeDistance away.
inline constexpr float kOrthoEyeDistance = 1000.0f;
void OrthoViewMatrix(int kind, const DirectX::XMFLOAT3& center, DirectX::XMFLOAT4X4* view, DirectX::XMFLOAT3* eye);

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

// ---- camera path ----

// Clips the world segment a-b against the viewing camera's near plane (view-space z >= vp.nearZ) and projects the
// visible part. False when the whole segment is behind the near plane. The projection is the same as Project().
bool ProjectSegment(const ViewProj& vp, const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, ImVec2& pa, ImVec2& pb);

struct CameraPathStyle {
    float lineWidth = 2.0f;        // screen px (caller scales by DPI)
    float keyRadius = 4.0f;
    float currentRadius = 5.5f;
    ImU32 path = IM_COL32(255, 196, 64, 230);       // eye path
    ImU32 key = IM_COL32(255, 255, 255, 240);       // key positions (eye at the key frame)
    ImU32 selectedKey = IM_COL32(255, 90, 90, 255); // keys in `selectedKeys`
    ImU32 current = IM_COL32(64, 220, 255, 255);    // camera at the current frame: dot + frustum
    ImU32 target = IM_COL32(64, 220, 255, 160);     // line eye -> look-at target of the current frame
    ImU32 outline = IM_COL32(10, 14, 20, 190);      // dark outline under lines and dots (contrast)
};

// One sample of the motion camera (built by the caller with CameraMotion::Evaluate + ToView).
struct CameraPathPoint {
    DirectX::XMFLOAT3 eye{};
    DirectX::XMFLOAT3 target{};
    DirectX::XMFLOAT4X4 view{};    // the motion camera's LH view matrix (for the frustum)
    float fovY = 0.5f;             // radians
};

// Draws the motion camera's path as seen from the viewing camera `vp`:
// - `path`: eye samples in frame order (one per frame typically); consecutive samples are joined by line segments
//   (outline colour 2 px wider underneath, then the path colour). A segment whose two eyes are farther apart than
//   `cutDistance` world units is a camera cut: it is NOT drawn (MMD camera cuts are 1-frame jumps).
// - `keys`: eye positions at the key frames, drawn as filled circles (keyRadius, outline ring); keys whose index is in
//   `selectedKeys` use selectedKey and radius keyRadius + 1.5.
// - `current`: the camera at the current frame: a filled circle (currentRadius) at its eye, a thin line from the eye
//   to its target (the target colour) with a small 3 px dot at the target, and its view frustum drawn as a wireframe
//   pyramid: apex at the eye, a rectangle at distance `frustumLength` world units in front of it with half-height
//   `frustumLength * tan(fovY / 2)` and half-width = half-height * `aspect`, the 4 edges apex->corners and the rectangle,
//   plus a small filled triangle above the rectangle's top edge marking "up" (like Blender's camera gizmo). The
//   frustum corners are computed in the motion camera's view space and transformed to world with the inverse of
//   `current.view`. Pass `current == nullptr` to skip it.
// Every line goes through ProjectSegment (clipped at the near plane), every dot through Project (skipped when behind).
void DrawCameraPath(ImDrawList* dl, const ViewProj& vp, const CameraPathPoint* path, int pathCount,
                    const DirectX::XMFLOAT3* keys, int keyCount, const std::set<int>& selectedKeys,
                    const CameraPathPoint* current, float aspect, float frustumLength, float cutDistance,
                    const CameraPathStyle& style);
// Index of the key (in `keys`) whose projected position is nearest to `mouse` within `radius` px, -1 if none.
int PickCameraKey(const ViewProj& vp, const DirectX::XMFLOAT3* keys, int keyCount, ImVec2 mouse, float radius);

} // namespace mmdx::studio
