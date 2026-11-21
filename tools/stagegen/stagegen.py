#!/usr/bin/env python3
"""stagegen: writes procedural MMDX test stages (GLB + stage.json) for tools/stage_check.

    python tools/stagegen/stagegen.py [--out captures/stages] [--only cyclorama,mirror_room,glass_room,led_stage]

Everything is generated from numpy arrays: no Blender, no pygltflib, no third-party glTF writer. The GLB is written
by hand (a JSON chunk + a BIN chunk, 4-byte padded) and is a valid glTF 2.0 asset with named nodes and materials,
triangle normals and UVs (documents: docs/stage_format.md).

Stages:
  cyclorama   curved floor-to-wall cove, 3 colour variants (cove + key / fill light colours), 7 named lights (4 spots)
  mirror_room a box room, one wall material flagged "mirror", a wood floor with PBR values
  led_stage   a raised stage with 3 screen quads (screen ids main / left / right), truss bars, 8 spots, fog
  gobo_test   S7b: two spot gobos on a floor + a coloured one on a back wall (spinning / keyed), haze on
"""

import argparse
import json
import math
import os
import struct
import zlib

import numpy as np

# ---------------------------------------------------------------------------------------------------------------------
# glTF / GLB writing
# ---------------------------------------------------------------------------------------------------------------------


class Part:
    """One drawable: a triangle list of one material, instanced at one named node."""

    def __init__(self, node, material, positions, normals, uvs, indices):
        self.node = node
        self.material = material
        self.positions = np.asarray(positions, dtype=np.float32).reshape(-1, 3)
        self.normals = np.asarray(normals, dtype=np.float32).reshape(-1, 3)
        self.uvs = np.asarray(uvs, dtype=np.float32).reshape(-1, 2)
        self.indices = np.asarray(indices, dtype=np.uint32).reshape(-1)
        assert len(self.positions) == len(self.normals) == len(self.uvs)
        assert len(self.indices) % 3 == 0


class Gltf:
    """Collects parts + materials and writes a .glb."""

    def __init__(self, scene_name):
        self.scene_name = scene_name
        self.parts = []
        self.materials = {}   # name -> glTF material dict

    def add(self, part):
        self.parts.append(part)

    def material(self, name, base_color=(0.5, 0.5, 0.5, 1.0), metallic=0.0, roughness=0.5,
                 emissive=(0.0, 0.0, 0.0), double_sided=False, alpha=1.0):
        mat = {
            "name": name,
            "pbrMetallicRoughness": {
                "baseColorFactor": [float(base_color[0]), float(base_color[1]), float(base_color[2]), float(alpha)],
                "metallicFactor": float(metallic),
                "roughnessFactor": float(roughness),
            },
            "doubleSided": bool(double_sided),
        }
        if any(c > 0 for c in emissive):
            mat["emissiveFactor"] = [float(emissive[0]), float(emissive[1]), float(emissive[2])]
        if alpha < 1.0:
            mat["alphaMode"] = "BLEND"
        self.materials[name] = mat
        return name

    # -- binary layout ---------------------------------------------------------------------------------------------
    def _flush(self, blob):
        while len(blob) % 4:
            blob.append(0)
        return blob

    def build(self):
        material_names = list(self.materials.keys())
        mat_index = {name: i for i, name in enumerate(material_names)}

        bin_blob = bytearray()
        buffer_views = []
        accessors = []

        def add_view(data, target):
            offset = len(bin_blob)
            bin_blob.extend(data.tobytes())
            self._flush(bin_blob)
            view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data.tobytes()), "target": target}
            buffer_views.append(view)
            return len(buffer_views) - 1

        def add_accessor(array, comp_type, type_name, target, normalized=False, minmax=False):
            view = add_view(array, target)
            acc = {"bufferView": view, "componentType": comp_type, "count": int(array.shape[0]), "type": type_name}
            if normalized:
                acc["normalized"] = True
            if minmax:
                acc["min"] = [float(v) for v in array.min(axis=0)]
                acc["max"] = [float(v) for v in array.max(axis=0)]
            accessors.append(acc)
            return len(accessors) - 1

        meshes = []
        nodes = []
        for part in self.parts:
            pos = add_accessor(part.positions, 5126, "VEC3", 34962, minmax=True)
            nrm = add_accessor(part.normals, 5126, "VEC3", 34962)
            uv = add_accessor(part.uvs, 5126, "VEC2", 34962)
            idx = add_accessor(part.indices.astype(np.uint32), 5125, "SCALAR", 34963)
            meshes.append({
                "name": part.node,
                "primitives": [{
                    "attributes": {"POSITION": pos, "NORMAL": nrm, "TEXCOORD_0": uv},
                    "indices": idx,
                    "material": mat_index[part.material],
                    "mode": 4,
                }],
            })
            nodes.append({"name": part.node, "mesh": len(meshes) - 1})

        gltf = {
            "asset": {"version": "2.0", "generator": "mmdx stagegen"},
            "scene": 0,
            "scenes": [{"name": self.scene_name, "nodes": list(range(len(nodes)))}],
            "nodes": nodes,
            "meshes": meshes,
            "materials": [self.materials[n] for n in material_names],
            "accessors": accessors,
            "bufferViews": buffer_views,
            "buffers": [{"byteLength": len(bin_blob)}],
        }
        json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
        while len(json_bytes) % 4:
            json_bytes += b" "
        bin_bytes = bytes(bin_blob)
        total = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)
        out = bytearray()
        out += struct.pack("<4sII", b"glTF", 2, total)
        out += struct.pack("<II", len(json_bytes), 0x4E4F534A) + json_bytes   # 'JSON'
        out += struct.pack("<II", len(bin_bytes), 0x004E4942) + bin_bytes     # 'BIN'
        return bytes(out)


def write_png(path, width, height, rgb):
    """A minimal RGB PNG (zlib + CRC), for the LED textures the screens reference."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    raw = bytearray()
    for y in range(height):
        raw.append(0)   # filter: none
        for x in range(width):
            raw.extend(rgb(x, y))
    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


# ---------------------------------------------------------------------------------------------------------------------
# geometry helpers (metres, +Y up, +Z toward the audience; the importer scales by 12.5 and mirrors Z)
# ---------------------------------------------------------------------------------------------------------------------


def grid_part(node, material, corners, nu, nv, flip=False, uv_scale=(1.0, 1.0)):
    """A quad patch subdivided nu x nv from 3 corners (bilinear): p(u,v) = p00 + u*(p10-p00) + v*(p01-p00)."""
    p00, p10, p01 = (np.asarray(c, dtype=np.float64) for c in corners)
    positions, uvs = [], []
    for j in range(nv + 1):
        for i in range(nu + 1):
            u, v = i / nu, j / nv
            positions.append(p00 + (p10 - p00) * u + (p01 - p00) * v)
            uvs.append((u * uv_scale[0], v * uv_scale[1]))
    positions = np.array(positions)
    n = np.cross(p10 - p00, p01 - p00)
    n /= np.linalg.norm(n)
    normals = np.tile(n, (len(positions), 1))
    indices = []
    for j in range(nv):
        for i in range(nu):
            a = j * (nu + 1) + i
            b, c, d = a + 1, a + (nu + 1), a + (nu + 1) + 1
            if flip:
                indices += [a, d, b, a, c, d]
            else:
                indices += [a, b, d, a, d, c]
    return Part(node, material, positions, normals, uvs, indices)


def sweep_profile(node, material, profile, x0, x1, closed_normal_inside=True, uv_scale=(1.0, 1.0)):
    """Sweeps a (y, z) polyline along X (in [-x0, x1]): one quad strip per segment.

    profile: list of (y, z, ny, nz) with the outward normal per point.
    """
    xs = [x0, x1]
    positions, uvs, indices = [], [], []
    length = 0.0
    along = [0.0]
    for k in range(1, len(profile)):
        dy = profile[k][0] - profile[k - 1][0]
        dz = profile[k][1] - profile[k - 1][1]
        length += math.hypot(dy, dz)
        along.append(length)
    for k, (y, z, ny, nz) in enumerate(profile):
        for x in xs:
            positions.append((x, y, z))
            uvs.append(((x - x0) / max(1e-6, (x1 - x0)) * uv_scale[0], along[k] * uv_scale[1]))
    for k in range(len(profile) - 1):
        a = 2 * k            # x0, profile k
        b = 2 * k + 1        # x1, profile k
        c = a + 2            # x0, profile k+1
        d = b + 2            # x1, profile k+1
        # the sweep's triangles as seen from the profile's normal side
        if (profile[k][2] * (profile[k + 1][1] - profile[k][1]) -
                profile[k][3] * (profile[k + 1][0] - profile[k][0])) >= 0:
            indices += [a, b, d, a, d, c]
        else:
            indices += [a, d, b, a, c, d]
    positions = np.array(positions)
    normals = np.array([(ny, nz) for (_, _, ny, nz) in profile for _ in xs])
    normals = np.stack([np.zeros(len(normals)), normals[:, 0], normals[:, 1]], axis=1)
    if closed_normal_inside:
        pass
    return Part(node, material, positions, normals, np.array(uvs), indices)


def box_parts(node_prefix, material, center, size, uv_scale=(1.0, 1.0), skip=()):
    """Six faces of an axis-aligned box, each its own part (named <node_prefix>_<face>)."""
    cx, cy, cz = center
    sx, sy, sz = (s / 2.0 for s in size)
    faces = {
        "px": ((cx + sx, cy - sy, cz - sz), (cx + sx, cy - sy, cz + sz), (cx + sx, cy + sy, cz - sz), False),
        "nx": ((cx - sx, cy - sy, cz + sz), (cx - sx, cy - sy, cz - sz), (cx - sx, cy + sy, cz + sz), False),
        "py": ((cx - sx, cy + sy, cz - sz), (cx + sx, cy + sy, cz - sz), (cx - sx, cy + sy, cz + sz), False),
        "ny": ((cx - sx, cy - sy, cz + sz), (cx + sx, cy - sy, cz + sz), (cx - sx, cy - sy, cz - sz), False),
        "pz": ((cx - sx, cy - sy, cz + sz), (cx + sx, cy - sy, cz + sz), (cx - sx, cy + sy, cz + sz), False),
        "nz": ((cx + sx, cy - sy, cz - sz), (cx - sx, cy - sy, cz - sz), (cx + sx, cy + sy, cz - sz), False),
    }
    out = []
    for name, (p00, p10, p01, flip) in faces.items():
        if name in skip:
            continue
        out.append(grid_part(f"{node_prefix}_{name}", material, (p00, p10, p01), 1, 1, flip, uv_scale))
    return out


def light(kind, name, intensity=1.0, color=(1.0, 1.0, 1.0), position=None, aim=None, direction=None,
          cone=(0.24, 0.15), range_=140.0, shadow="soft", aim_mode="manual", sway_phase=0.0):
    """One stage.json "lights" entry: exactly the .mmdxproj editor.lights schema."""
    values = {"color": list(color), "intensity": float(intensity), "range": float(range_)}
    if position is not None:
        values["position"] = list(position)
    if aim is not None:
        values["aim"] = list(aim)
        values["coneOuter"] = float(cone[0])
        values["coneInner"] = float(cone[1])
    if direction is not None:
        values["direction"] = list(direction)
    entry = {
        "kind": kind,
        "name": name,
        "enabled": True,
        "values": values,
        "shadow": {"type": shadow},
    }
    if kind == "spot":
        entry["spot"] = {"aim": aim_mode, "swayPhase": sway_phase}
    return entry


# ---------------------------------------------------------------------------------------------------------------------
# stage 1: cyclorama
# ---------------------------------------------------------------------------------------------------------------------


def build_cyclorama(out_dir):
    gltf = Gltf("cyclorama")
    cove = gltf.material("cove", base_color=(0.82, 0.82, 0.85, 1.0), roughness=0.45, metallic=0.0)
    floor = gltf.material("floor", base_color=(0.30, 0.31, 0.33, 1.0), roughness=0.28, metallic=0.0)
    wall = gltf.material("wall", base_color=(0.78, 0.78, 0.80, 1.0), roughness=0.6, metallic=0.0)

    half_width, depth, wall_height, radius, segments = 12.0, 14.0, 12.0, 6.0, 24
    z_curve = -depth + radius          # the cove starts here on the floor
    z_wall = -depth                    # ... and ends here at the wall
    # floor: from z = +2 (front, toward the camera) back to z_curve
    gltf.add(grid_part("Cove_Floor", floor,
                       ((-half_width, 0.0, 2.0), (half_width, 0.0, 2.0), (-half_width, 0.0, z_curve)),
                       8, 8, flip=False, uv_scale=(6.0, 6.0)))
    # cove: a quarter circle from (y=0, z=z_curve) to (y=radius, z=z_wall); inward normal (+y / +z)
    profile = []
    for i in range(segments + 1):
        t = math.pi / 2 * i / segments
        y = radius - radius * math.cos(t)
        z = z_curve - radius * math.sin(t)
        ny, nz = math.cos(t), math.sin(t)   # the surface normal points away from the centre (0, radius, z_curve)
        profile.append((y, z, ny, nz))
    gltf.add(sweep_profile("Cove_Curve", cove, profile, -half_width, half_width, uv_scale=(6.0, 0.5)))
    # wall above the cove
    gltf.add(grid_part("Cove_Wall", wall,
                       ((-half_width, radius, z_wall), (half_width, radius, z_wall), (-half_width, wall_height, z_wall)),
                       8, 4, flip=False, uv_scale=(6.0, 3.0)))

    stage_dir = os.path.join(out_dir, "cyclorama")
    os.makedirs(stage_dir, exist_ok=True)
    with open(os.path.join(stage_dir, "cyclorama.glb"), "wb") as f:
        f.write(gltf.build())

    lights = [
        light("sun", "key", intensity=1.1, color=(1.0, 0.96, 0.90), direction=(-0.35, -1.0, 0.25)),
        light("point", "fill", intensity=0.6, color=(0.92, 0.95, 1.0), position=(0.0, 34.0, -40.0), range_=180.0,
              shadow="nocast"),
        light("ambient", "ambient", intensity=0.18, color=(1.0, 1.0, 1.0)),
        light("spot", "spot_l", intensity=2.4, color=(1.0, 0.95, 0.88), position=(-45.0, 62.0, 40.0),
              aim=(-22.0, 0.0, -30.0), cone=(0.26, 0.17), aim_mode="sway", sway_phase=0.0),
        light("spot", "spot_r", intensity=2.4, color=(0.95, 0.97, 1.0), position=(45.0, 62.0, 40.0),
              aim=(22.0, 0.0, -30.0), cone=(0.26, 0.17), aim_mode="sway", sway_phase=1.3),
        light("spot", "spot_back_l", intensity=1.6, color=(0.55, 0.85, 1.0), position=(-50.0, 58.0, -110.0),
              aim=(-10.0, 4.0, -40.0), cone=(0.3, 0.2), shadow="nocast"),
        light("spot", "spot_back_r", intensity=1.6, color=(1.0, 0.6, 0.78), position=(50.0, 58.0, -110.0),
              aim=(10.0, 4.0, -40.0), cone=(0.3, 0.2), shadow="nocast"),
    ]
    manifest = {
        "format": "mmdx-stage",
        "formatVersion": 1,
        "id": "cyclorama",
        "version": "1.0.0",
        "name": {"ko": "사이클로라마 댄스 스튜디오", "en": "Cyclorama dance studio", "ja": "サイクロラマ ダンススタジオ"},
        "description": {"ko": "바닥과 벽이 하나로 이어지는 무한 배경 댄스 스튜디오.",
                        "en": "A dance studio with an infinite floor-to-wall cove."},
        "authors": [{"name": "MMDX stagegen", "role": "procedural"}],
        "license": "CC0-1.0",
        "tags": ["studio", "dance", "cove", "test"],
        "models": [{"file": "cyclorama.glb", "name": "Cove", "role": "floor"}],
        "materials": [
            {"match": "cove", "baseColor": [0.82, 0.82, 0.85, 1.0], "roughness": 0.45, "metallic": 0.0,
             "receiveShadow": True, "castShadow": False},
            {"match": "floor", "roughness": 0.28, "metallic": 0.0},
            {"match": "wall", "roughness": 0.6},
        ],
        "lights": lights,
        "sockets": [
            {"name": "world", "position": [0.0, 0.0, -20.0]},
            {"name": "cove_center", "position": [0.0, 0.0, -75.0], "rotation": [0.0, 180.0, 0.0]},
            {"name": "wall_node", "node": "Cove_Wall"},
        ],
        "cameras": [
            {"name": "wide", "position": [0.0, 16.0, 130.0], "target": [0.0, 11.0, -60.0], "fov": 42.0},
            {"name": "close", "position": [0.0, 13.0, 40.0], "target": [0.0, 11.0, -40.0], "fov": 55.0},
        ],
        "environment": {
            "fog": {"density": 0.0006, "color": [0.62, 0.65, 0.72], "anisotropy": 0.3, "heightFalloff": 0.02},
            "ambient": {"intensity": 0.18},
        },
        "groups": [
            {"id": "look", "label": {"ko": "분위기", "en": "Look"}},
            {"id": "lights", "label": {"ko": "조명", "en": "Lights"}},
        ],
        "params": [
            {"key": "coveColor", "type": "color", "label": {"ko": "배경 색", "en": "Cove colour"},
             "group": "look", "default": [0.82, 0.82, 0.85, 1.0]},
            {"key": "floorColor", "type": "color", "label": {"ko": "바닥 색", "en": "Floor colour"},
             "group": "look", "default": [0.30, 0.31, 0.33, 1.0]},
            {"key": "keyColor", "type": "color", "label": {"ko": "키 라이트", "en": "Key light"}, "group": "lights",
             "default": [1.0, 0.96, 0.90, 1.0]},
            {"key": "keyIntensity", "type": "float", "label": {"ko": "키 세기", "en": "Key intensity"},
             "group": "lights", "default": 1.1, "min": 0.0, "max": 3.0},
            {"key": "fillColor", "type": "color", "label": {"ko": "필 라이트", "en": "Fill light"}, "group": "lights",
             "default": [0.92, 0.95, 1.0, 1.0]},
            {"key": "fillIntensity", "type": "float", "label": {"ko": "필 세기", "en": "Fill intensity"},
             "group": "lights", "default": 0.6, "min": 0.0, "max": 2.0},
            {"key": "fogDensity", "type": "float", "label": {"ko": "안개", "en": "Fog"}, "group": "look",
             "default": 0.004, "min": 0.0, "max": 0.05},
            {"key": "showScreens", "type": "bool", "label": {"ko": "밝은 배경", "en": "Bright cove"},
             "group": "look", "default": False},
        ],
        "variants": [
            {"id": "studio", "label": {"ko": "스튜디오", "en": "Studio"},
             "values": {"coveColor": [0.82, 0.82, 0.85, 1.0], "keyColor": [1.0, 0.96, 0.90, 1.0],
                        "fillColor": [0.92, 0.95, 1.0, 1.0], "fogDensity": 0.004}},
            {"id": "daylight", "label": {"ko": "주광", "en": "Daylight"},
             "values": {"coveColor": [0.92, 0.94, 0.98, 1.0], "keyColor": [1.0, 0.99, 0.97, 1.0],
                        "fillColor": [0.86, 0.93, 1.0, 1.0], "keyIntensity": 1.6, "fogDensity": 0.001}},
            {"id": "night", "label": {"ko": "야간", "en": "Night"},
             "values": {"coveColor": [0.05, 0.06, 0.10, 1.0], "keyColor": [0.45, 0.62, 1.0, 1.0],
                        "fillColor": [1.0, 0.45, 0.7, 1.0], "keyIntensity": 0.5, "fogDensity": 0.012}},
        ],
        "defaultVariant": "studio",
        "bindings": [
            {"param": "coveColor", "target": "materials.cove.baseColor"},
            {"param": "floorColor", "target": "materials.floor.baseColor"},
            {"param": "keyColor", "target": "lights.key.color"},
            {"param": "keyIntensity", "target": "lights.key.intensity"},
            {"param": "fillColor", "target": "lights.fill.color"},
            {"param": "fillIntensity", "target": "lights.fill.intensity"},
            {"param": "fogDensity", "target": "environment.fog.density"},
            {"param": "showScreens", "target": "materials.wall.emissiveStrength", "scale": 1.5},
        ],
        "cues": [
            {"param": "keyIntensity", "keys": [
                {"frame": 0, "value": 1.1}, {"frame": 300, "value": 1.5},
                {"frame": 600, "value": 0.9}, {"frame": 900, "value": 1.1}]},
            {"param": "fogDensity", "stepped": True,
             "keys": [{"frame": 0, "value": 0.004}, {"frame": 900, "value": 0.012}]},
        ],
    }
    with open(os.path.join(stage_dir, "stage.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    return stage_dir


# ---------------------------------------------------------------------------------------------------------------------
# stage 2: mirror room
# ---------------------------------------------------------------------------------------------------------------------


def build_mirror_room(out_dir):
    gltf = Gltf("mirror_room")
    wood = gltf.material("wood_floor", base_color=(0.34, 0.21, 0.12, 1.0), roughness=0.38, metallic=0.0)
    paint = gltf.material("wall_paint", base_color=(0.72, 0.71, 0.70, 1.0), roughness=0.7, metallic=0.0)
    mirror = gltf.material("mirror_wall", base_color=(0.90, 0.92, 0.95, 1.0), roughness=0.02, metallic=1.0)
    ceiling = gltf.material("ceiling", base_color=(0.85, 0.85, 0.86, 1.0), roughness=0.8, metallic=0.0)

    width, height, depth = 14.0, 5.0, 16.0
    # floor (inward, +y), ceiling, and the four walls (inward normals)
    gltf.add(grid_part("Floor", wood, ((-width / 2, 0.0, -depth / 2), (-width / 2, 0.0, depth / 2),
                                      (width / 2, 0.0, -depth / 2)), 8, 8, flip=False, uv_scale=(7.0, 8.0)))
    gltf.add(grid_part("Ceiling", ceiling, ((-width / 2, height, depth / 2), (-width / 2, height, -depth / 2),
                                           (width / 2, height, depth / 2)), 4, 4, flip=False))
    # the mirror is the back wall (-z); it faces +z (into the room)
    gltf.add(grid_part("Mirror_Wall", mirror, ((-width / 2, 0.0, -depth / 2), (width / 2, 0.0, -depth / 2),
                                              (-width / 2, height, -depth / 2)), 4, 2, flip=False))
    # side walls: left (-x) faces +x, right (+x) faces -x
    gltf.add(grid_part("Wall_Left", paint, ((-width / 2, 0.0, depth / 2), (-width / 2, 0.0, -depth / 2),
                                           (-width / 2, height, depth / 2)), 4, 2, flip=False))
    gltf.add(grid_part("Wall_Right", paint, ((width / 2, 0.0, -depth / 2), (width / 2, 0.0, depth / 2),
                                            (width / 2, height, -depth / 2)), 4, 2, flip=False))
    # front wall (+z, behind the camera): faces -z, so the room can be seen from any angle
    gltf.add(grid_part("Wall_Front", paint, ((width / 2, 0.0, depth / 2), (-width / 2, 0.0, depth / 2),
                                            (width / 2, height, depth / 2)), 4, 2, flip=False))
    # a ballet barre along the mirror, and a bench: props parts
    for part in box_parts("Barre", paint, (0.0, 1.05, -depth / 2 + 0.35), (width - 2.0, 0.08, 0.08),
                          skip=("py", "ny")):
        gltf.add(part)
    for part in box_parts("Bench", wood, (width / 2 - 1.6, 0.22, 1.5), (2.4, 0.35, 0.6), skip=("py", "ny")):
        gltf.add(part)

    stage_dir = os.path.join(out_dir, "mirror_room")
    os.makedirs(stage_dir, exist_ok=True)
    with open(os.path.join(stage_dir, "mirror_room.glb"), "wb") as f:
        f.write(gltf.build())

    manifest = {
        "format": "mmdx-stage",
        "formatVersion": 1,
        "id": "mirror_room",
        "version": "1.0.0",
        "name": {"ko": "거울 연습실", "en": "Mirror practice room", "ja": "鏡の練習室"},
        "description": {"ko": "거울 벽과 나무 바닥이 있는 연습실.", "en": "A practice room with a mirror wall."},
        "authors": [{"name": "MMDX stagegen", "role": "procedural"}],
        "license": "CC0-1.0",
        "tags": ["practice", "mirror", "interior", "test"],
        "models": [{"file": "mirror_room.glb", "name": "Room", "role": "floor"}],
        "materials": [
            {"match": "mirror_wall", "baseColor": [0.90, 0.92, 0.95, 1.0], "roughness": 0.02, "metallic": 1.0,
             "mirror": True, "castShadow": False},
            {"match": "wood_floor", "baseColor": [0.34, 0.21, 0.12, 1.0], "roughness": 0.38, "metallic": 0.0,
             "receiveShadow": True},
            {"match": "wall_paint", "roughness": 0.7},
            {"match": "ceiling", "castShadow": False},
        ],
        "lights": [
            light("ambient", "ambient", intensity=0.22),
            light("point", "ceiling_l", intensity=1.4, color=(1.0, 0.97, 0.92), position=(-55.0, 55.0, 20.0),
                  range_=160.0),
            light("point", "ceiling_r", intensity=1.4, color=(1.0, 0.97, 0.92), position=(55.0, 55.0, 20.0),
                  range_=160.0),
            light("point", "ceiling_back", intensity=1.0, color=(0.96, 0.98, 1.0), position=(0.0, 55.0, -80.0),
                  range_=160.0, shadow="nocast"),
            light("spot", "window_sun", intensity=1.8, color=(1.0, 0.93, 0.82), position=(-120.0, 90.0, 60.0),
                  aim=(0.0, 0.0, -20.0), cone=(0.32, 0.22)),
        ],
        "sockets": [
            {"name": "world", "position": [0.0, 0.0, 0.0]},
            {"name": "mirror", "node": "Mirror_Wall"},
            {"name": "floor_center", "node": "Floor"},
        ],
        "cameras": [
            {"name": "wide", "position": [0.0, 18.0, 115.0], "target": [0.0, 14.0, -30.0], "fov": 45.0},
            {"name": "mirror_eye", "position": [0.0, 20.0, 40.0], "target": [0.0, 20.0, -100.0], "fov": 38.0},
        ],
        "environment": {"fog": {"density": 0.002, "color": [0.70, 0.72, 0.76], "anisotropy": 0.2}},
        "params": [
            {"key": "mirrorTint", "type": "color", "label": {"ko": "거울 색", "en": "Mirror tint"},
             "default": [0.90, 0.92, 0.95, 1.0]},
            {"key": "mirrorAmount", "type": "enum", "label": {"ko": "거울", "en": "Mirror"}, "default": "on",
             "options": [{"value": "off", "label": {"ko": "끔", "en": "Off"}},
                         {"value": "on", "label": {"ko": "켬", "en": "On"}}]},
            {"key": "lampIntensity", "type": "float", "label": {"ko": "조명 세기", "en": "Lamp intensity"},
             "default": 1.4, "min": 0.0, "max": 4.0},
            {"key": "floorGloss", "type": "float", "label": {"ko": "바닥 광택", "en": "Floor gloss"},
             "default": 0.38, "min": 0.02, "max": 1.0},
        ],
        "variants": [
            {"id": "rehearsal", "label": {"ko": "연습", "en": "Rehearsal"},
             "values": {"lampIntensity": 1.4, "floorGloss": 0.38}},
            {"id": "curtain_up", "label": {"ko": "공연", "en": "Show"},
             "values": {"mirrorTint": [0.55, 0.60, 0.72, 1.0], "lampIntensity": 2.6, "floorGloss": 0.15}},
        ],
        "defaultVariant": "rehearsal",
        "bindings": [
            {"param": "mirrorTint", "target": "materials.mirror_wall.baseColor"},
            {"param": "mirrorAmount", "target": "materials.mirror_wall.mirror"},
            {"param": "lampIntensity", "target": "lights.ceiling_l.intensity"},
            {"param": "lampIntensity", "target": "lights.ceiling_r.intensity"},
            {"param": "lampIntensity", "target": "lights.ceiling_back.intensity", "scale": 0.7},
            {"param": "floorGloss", "target": "materials.wood_floor.roughness"},
        ],
    }
    with open(os.path.join(stage_dir, "stage.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    return stage_dir


def build_glass_room(out_dir):
    """#31 fixture: a room split by one flat, free-standing glass pane (transmission 1, ior 1.5, smooth). Raster shows
    the planar reflection of the room on the pane (Fresnel weighted, strongest at grazing angles); a red block behind
    the pane shows what is seen through it."""
    gltf = Gltf("glass_room")
    wood = gltf.material("wood_floor", base_color=(0.34, 0.21, 0.12, 1.0), roughness=0.38, metallic=0.0)
    paint = gltf.material("wall_paint", base_color=(0.72, 0.71, 0.70, 1.0), roughness=0.7, metallic=0.0)
    accent = gltf.material("accent", base_color=(0.80, 0.12, 0.10, 1.0), roughness=0.5, metallic=0.0)
    glass = gltf.material("glass_pane", base_color=(0.97, 0.985, 1.0, 1.0), roughness=0.0, metallic=0.0)
    ceiling = gltf.material("ceiling", base_color=(0.85, 0.85, 0.86, 1.0), roughness=0.8, metallic=0.0)

    width, height, depth = 14.0, 5.0, 16.0
    gltf.add(grid_part("Floor", wood, ((-width / 2, 0.0, -depth / 2), (-width / 2, 0.0, depth / 2),
                                      (width / 2, 0.0, -depth / 2)), 8, 8, flip=False, uv_scale=(7.0, 8.0)))
    gltf.add(grid_part("Ceiling", ceiling, ((-width / 2, height, depth / 2), (-width / 2, height, -depth / 2),
                                           (width / 2, height, depth / 2)), 4, 4, flip=False))
    gltf.add(grid_part("Wall_Back", paint, ((-width / 2, 0.0, -depth / 2), (width / 2, 0.0, -depth / 2),
                                           (-width / 2, height, -depth / 2)), 4, 2, flip=False))
    gltf.add(grid_part("Wall_Left", paint, ((-width / 2, 0.0, depth / 2), (-width / 2, 0.0, -depth / 2),
                                           (-width / 2, height, depth / 2)), 4, 2, flip=False))
    gltf.add(grid_part("Wall_Right", paint, ((width / 2, 0.0, -depth / 2), (width / 2, 0.0, depth / 2),
                                            (width / 2, height, -depth / 2)), 4, 2, flip=False))
    gltf.add(grid_part("Wall_Front", paint, ((width / 2, 0.0, depth / 2), (-width / 2, 0.0, depth / 2),
                                            (width / 2, height, depth / 2)), 4, 2, flip=False))
    # the pane: 9 x 3.6 at z = -3, facing +z (toward the default camera)
    gltf.add(grid_part("Glass_Pane", glass, ((-4.5, 0.2, -3.0), (4.5, 0.2, -3.0), (-4.5, 3.8, -3.0)), 2, 1,
                       flip=False))
    for part in box_parts("Block", accent, (0.0, 0.8, -5.5), (2.0, 1.6, 1.0), skip=("ny",)):
        gltf.add(part)
    for part in box_parts("Bench", wood, (3.5, 0.22, 3.0), (2.4, 0.35, 0.6), skip=("py", "ny")):
        gltf.add(part)

    stage_dir = os.path.join(out_dir, "glass_room")
    os.makedirs(stage_dir, exist_ok=True)
    with open(os.path.join(stage_dir, "glass_room.glb"), "wb") as f:
        f.write(gltf.build())

    manifest = {
        "format": "mmdx-stage",
        "formatVersion": 1,
        "id": "glass_room",
        "version": "1.0.0",
        "name": {"ko": "유리 칸막이 방", "en": "Glass partition room", "ja": "ガラス仕切りの部屋"},
        "description": {"ko": "평평한 유리 한 장이 방을 나누는 테스트 무대.", "en": "A test room split by one flat glass pane."},
        "authors": [{"name": "MMDX stagegen", "role": "procedural"}],
        "license": "CC0-1.0",
        "tags": ["glass", "interior", "test"],
        "models": [{"file": "glass_room.glb", "name": "Room", "role": "floor"}],
        "materials": [
            {"match": "glass_pane", "baseColor": [0.97, 0.985, 1.0, 1.0], "transmission": 1.0, "ior": 1.5,
             "roughness": 0.0, "castShadow": False},
            {"match": "wood_floor", "roughness": 0.38, "receiveShadow": True},
            {"match": "wall_paint", "roughness": 0.7},
            {"match": "ceiling", "castShadow": False},
        ],
        "lights": [
            light("ambient", "ambient", intensity=0.22),
            light("point", "ceiling_l", intensity=1.4, color=(1.0, 0.97, 0.92), position=(-55.0, 55.0, 40.0),
                  range_=160.0),
            light("point", "ceiling_r", intensity=1.4, color=(1.0, 0.97, 0.92), position=(55.0, 55.0, 40.0),
                  range_=160.0),
            light("point", "ceiling_back", intensity=1.0, color=(0.96, 0.98, 1.0), position=(0.0, 55.0, -80.0),
                  range_=160.0, shadow="nocast"),
        ],
        "sockets": [{"name": "world", "position": [0.0, 0.0, 0.0]}],
        "cameras": [
            {"name": "wide", "position": [0.0, 18.0, 115.0], "target": [0.0, 14.0, -30.0], "fov": 45.0},
            {"name": "grazing", "position": [-70.0, 20.0, 20.0], "target": [10.0, 18.0, -38.0], "fov": 40.0},
        ],
    }
    with open(os.path.join(stage_dir, "stage.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    return stage_dir


# ---------------------------------------------------------------------------------------------------------------------
# stage 3: led stage
# ---------------------------------------------------------------------------------------------------------------------


def build_led_stage(out_dir):
    gltf = Gltf("led_stage")
    deck = gltf.material("stage_deck", base_color=(0.10, 0.10, 0.11, 1.0), roughness=0.22, metallic=0.2)
    skirt = gltf.material("stage_skirt", base_color=(0.05, 0.05, 0.06, 1.0), roughness=0.6, metallic=0.1)
    truss = gltf.material("truss_metal", base_color=(0.55, 0.56, 0.58, 1.0), roughness=0.35, metallic=1.0)
    led_main = gltf.material("led_main", base_color=(0.02, 0.02, 0.03, 1.0), roughness=0.35, metallic=0.0,
                            emissive=(0.4, 0.5, 0.8), double_sided=True)
    led_left = gltf.material("led_left", base_color=(0.02, 0.02, 0.03, 1.0), roughness=0.35, metallic=0.0,
                            emissive=(0.3, 0.6, 0.7), double_sided=True)
    led_right = gltf.material("led_right", base_color=(0.02, 0.02, 0.03, 1.0), roughness=0.35, metallic=0.0,
                             emissive=(0.8, 0.3, 0.5), double_sided=True)

    deck_w, deck_h, deck_d = 18.0, 1.2, 10.0
    # the raised stage: top, front and side faces (the back is against the LED wall)
    gltf.add(grid_part("Stage_Top", deck, ((-deck_w / 2, deck_h, -deck_d / 2), (deck_w / 2, deck_h, -deck_d / 2),
                                          (-deck_w / 2, deck_h, deck_d / 2)), 9, 5, uv_scale=(9.0, 5.0)))
    gltf.add(grid_part("Stage_Front", skirt, ((-deck_w / 2, deck_h, deck_d / 2), (deck_w / 2, deck_h, deck_d / 2),
                                             (-deck_w / 2, 0.0, deck_d / 2)), 9, 1))
    gltf.add(grid_part("Stage_Left", skirt, ((-deck_w / 2, deck_h, -deck_d / 2), (-deck_w / 2, deck_h, deck_d / 2),
                                            (-deck_w / 2, 0.0, -deck_d / 2)), 5, 1))
    gltf.add(grid_part("Stage_Right", skirt, ((deck_w / 2, deck_h, deck_d / 2), (deck_w / 2, deck_h, -deck_d / 2),
                                             (deck_w / 2, 0.0, deck_d / 2)), 5, 1))
    # three screen quads, tilted slightly toward the audience, standing behind the stage
    z_back = -deck_d / 2 - 3.0
    gltf.add(grid_part("Screen_Main", led_main,
                       ((-6.0, 1.2, z_back), (6.0, 1.2, z_back), (-6.0, 8.2, z_back)), 6, 4,
                       uv_scale=(6.0, 4.0)))
    gltf.add(grid_part("Screen_Left", led_left,
                       ((-13.0, 1.2, z_back + 0.5), (-7.2, 1.2, z_back + 0.5), (-13.0, 6.6, z_back + 0.5)), 3, 3,
                       uv_scale=(3.0, 3.0)))
    gltf.add(grid_part("Screen_Right", led_right,
                       ((7.2, 1.2, z_back + 0.5), (13.0, 1.2, z_back + 0.5), (7.2, 6.6, z_back + 0.5)), 3, 3,
                       uv_scale=(3.0, 3.0)))
    # truss: two horizontal beams over the stage and four posts
    for name, center, size in (
            ("Truss_Front", (0.0, 8.6, deck_d / 2 - 1.0), (deck_w + 1.0, 0.35, 0.35)),
            ("Truss_Back", (0.0, 8.6, z_back + 1.5), (deck_w + 1.0, 0.35, 0.35)),
            ("Truss_Post_LF", (-(deck_w / 2 + 0.2), 4.3, deck_d / 2 - 1.0), (0.25, 8.6, 0.25)),
            ("Truss_Post_RF", (deck_w / 2 + 0.2, 4.3, deck_d / 2 - 1.0), (0.25, 8.6, 0.25)),
            ("Truss_Post_LB", (-(deck_w / 2 + 0.2), 4.3, z_back + 1.5), (0.25, 8.6, 0.25)),
            ("Truss_Post_RB", (deck_w / 2 + 0.2, 4.3, z_back + 1.5), (0.25, 8.6, 0.25))):
        for part in box_parts(name, truss, center, size):
            gltf.add(part)

    stage_dir = os.path.join(out_dir, "led_stage")
    os.makedirs(stage_dir, exist_ok=True)
    with open(os.path.join(stage_dir, "led_stage.glb"), "wb") as f:
        f.write(gltf.build())
    # the LED textures the screens reference (a soft gradient)
    def gradient(x, y):
        return (int(40 + 120 * x / 63), int(60 + 90 * y / 35), 180)
    write_png(os.path.join(out_dir, "led_stage", "textures", "led_main.png"), 64, 36, gradient)

    spots = []
    for i in range(8):
        side = -1.0 if i % 2 == 0 else 1.0
        row = i // 2
        spots.append(light(
            "spot", f"spot_{i + 1}", intensity=2.8,
            color=[(0.25, 0.8, 0.78), (0.95, 0.35, 0.62), (1.0, 0.95, 0.88), (0.55, 0.55, 1.0)][i % 4],
            position=[side * (30.0 + 18.0 * row), 105.0, 45.0 - 8.0 * row],
            aim=[side * 8.0, 8.0, -45.0], cone=(0.22, 0.13), aim_mode="sway", sway_phase=i * 1.1))

    manifest = {
        "format": "mmdx-stage",
        "formatVersion": 1,
        "id": "led_stage",
        "version": "1.0.0",
        "name": {"ko": "LED 아레나 스테이지", "en": "LED arena stage", "ja": "LEDアリーナステージ"},
        "description": {"ko": "LED 스크린 세 면과 트러스, 8개의 무빙 스팟이 있는 아레나 무대.",
                        "en": "An arena stage with three LED screens, truss bars and eight moving spots."},
        "authors": [{"name": "MMDX stagegen", "role": "procedural"}],
        "license": "CC0-1.0",
        "tags": ["arena", "led", "concert", "test"],
        "models": [{"file": "led_stage.glb", "name": "Arena", "role": "floor"}],
        "materials": [
            {"match": "stage_deck", "baseColor": [0.10, 0.10, 0.11, 1.0], "roughness": 0.22, "metallic": 0.2},
            {"match": "stage_skirt", "roughness": 0.6},
            {"match": "truss_metal", "metallic": 1.0, "roughness": 0.35},
            {"match": "led_*", "screen": "main", "castShadow": False},
            {"match": "led_left", "screen": "left", "castShadow": False},
            {"match": "led_right", "screen": "right", "castShadow": False},
        ],
        "lights": [
            light("sun", "key", intensity=0.14, color=(0.55, 0.60, 0.75), direction=(0.2, -1.0, 0.35)),
            light("ambient", "ambient", intensity=0.08, color=(1.0, 1.0, 1.0)),
            light("point", "crowd_fill", intensity=0.5, color=(0.85, 0.90, 1.0), position=(0.0, 18.0, 120.0),
                  range_=220.0, shadow="nocast"),
            *spots,
        ],
        "sockets": [
            {"name": "world", "position": [0.0, 15.0, -100.0]},
            {"name": "truss_center", "node": "Truss_Front"},
            {"name": "stage_center", "position": [0.0, 15.0, 0.0]},
            {"name": "screen_main", "node": "Screen_Main"},
        ],
        "cameras": [
            {"name": "wide", "position": [0.0, 28.0, 190.0], "target": [0.0, 32.0, -40.0], "fov": 40.0},
            {"name": "stage", "position": [0.0, 24.0, 70.0], "target": [0.0, 30.0, -60.0], "fov": 50.0},
            {"name": "crowd", "position": [0.0, 45.0, 300.0], "target": [0.0, 25.0, -20.0], "fov": 32.0},
        ],
        "screens": [
            {"id": "main", "materials": ["led_main"], "source": "image", "file": "textures/led_main.png",
             "width": 2048, "height": 1024, "brightness": 1.6, "emitLight": True},
            {"id": "left", "materials": ["led_left"], "source": "camera", "camera": "stage", "width": 1024,
             "height": 1024, "brightness": 1.2, "emitLight": True},
            {"id": "right", "materials": ["led_right"], "source": "none", "width": 1024, "height": 1024,
             "brightness": 1.0, "emitLight": False},
        ],
        "environment": {
            "fog": {"density": 0.020, "color": [0.55, 0.60, 0.72], "anisotropy": 0.55, "heightFalloff": 0.03},
            "ambient": {"intensity": 0.08, "skyZenith": [0.03, 0.04, 0.08], "ground": [0.05, 0.05, 0.06]},
        },
        "groups": [
            {"id": "screens", "label": {"ko": "스크린", "en": "Screens"}},
            {"id": "lights", "label": {"ko": "조명", "en": "Lights"}},
        ],
        "params": [
            {"key": "screenBrightness", "type": "float", "label": {"ko": "스크린 밝기", "en": "Screen brightness"},
             "group": "screens", "default": 1.6, "min": 0.0, "max": 4.0},
            {"key": "mainScreen", "type": "enum", "label": {"ko": "메인 소스", "en": "Main source"},
             "group": "screens", "default": "image",
             "options": [{"value": "none"}, {"value": "image"}, {"value": "camera"}, {"value": "video"}]},
            {"key": "spotLevel", "type": "float", "label": {"ko": "스팟 세기", "en": "Spot level"},
             "group": "lights", "default": 2.8, "min": 0.0, "max": 6.0},
            {"key": "haze", "type": "float", "label": {"ko": "헤이즈", "en": "Haze"}, "group": "lights",
             "default": 0.020, "min": 0.0, "max": 0.08},
        ],
        "variants": [
            {"id": "concert", "label": {"ko": "콘서트", "en": "Concert"},
             "values": {"screenBrightness": 1.6, "spotLevel": 2.8, "haze": 0.020}},
            {"id": "rehearsal", "label": {"ko": "리허설", "en": "Rehearsal"},
             "values": {"screenBrightness": 0.6, "spotLevel": 1.2, "haze": 0.004}},
            {"id": "blackout", "label": {"ko": "블랙아웃", "en": "Blackout"},
             "values": {"screenBrightness": 0.0, "spotLevel": 0.0, "haze": 0.03}},
        ],
        "defaultVariant": "concert",
        "bindings": [
            {"param": "screenBrightness", "target": "screens.main.brightness"},
            {"param": "screenBrightness", "target": "screens.left.brightness", "scale": 0.75},
            {"param": "mainScreen", "target": "screens.main.source"},
            {"param": "spotLevel", "target": "lights.spot_1.intensity"},
            {"param": "spotLevel", "target": "lights.spot_2.intensity"},
            {"param": "spotLevel", "target": "lights.spot_3.intensity"},
            {"param": "spotLevel", "target": "lights.spot_4.intensity"},
            {"param": "spotLevel", "target": "lights.spot_5.intensity"},
            {"param": "spotLevel", "target": "lights.spot_6.intensity"},
            {"param": "spotLevel", "target": "lights.spot_7.intensity"},
            {"param": "spotLevel", "target": "lights.spot_8.intensity"},
            {"param": "haze", "target": "environment.fog.density"},
        ],
        "cues": [
            {"param": "spotLevel", "stepped": False, "keys": [
                {"frame": 0, "value": 0.4}, {"frame": 120, "value": 3.2},
                {"frame": 240, "value": 1.2}, {"frame": 360, "value": 3.6}]},
            {"param": "screenBrightness", "keys": [
                {"frame": 0, "value": 0.8}, {"frame": 120, "value": 2.4}, {"frame": 360, "value": 1.4}]},
        ],
    }
    with open(os.path.join(stage_dir, "stage.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    return stage_dir


def build_pack_test(out_dir):
    """S6 fixture: one bundled pack of each role (docs/stage_format.md `packs`).

    screen  packs/led     screen.hlsl draws animated bars from `time` and the `bars` param (source "program")
    surface packs/tint    surface.hlsl tints the material its globs claim; every other material keeps the default
    world   packs/snow    the built-in snow pack, copied unchanged, at the "world" socket
    """
    stage_id = "pack_test"
    gltf = Gltf(stage_id)
    deck = gltf.material("stage_deck", base_color=(0.18, 0.19, 0.22, 1.0), roughness=0.3, metallic=0.2)
    cove = gltf.material("cove_wall", base_color=(0.72, 0.73, 0.78, 1.0), roughness=0.55, metallic=0.0)
    led = gltf.material("led_main", base_color=(0.02, 0.02, 0.03, 1.0), roughness=0.35, metallic=0.0,
                        emissive=(0.35, 0.45, 0.8), double_sided=True)

    deck_w, deck_d = 14.0, 9.0
    gltf.add(grid_part("Stage_Top", deck, ((-deck_w / 2, 0.0, -deck_d / 2), (deck_w / 2, 0.0, -deck_d / 2),
                                          (-deck_w / 2, 0.0, deck_d / 2)), 7, 5, uv_scale=(7.0, 5.0)))
    z_back = -deck_d / 2
    gltf.add(grid_part("Cove_Wall", cove, ((-deck_w / 2, 0.0, z_back), (deck_w / 2, 0.0, z_back),
                                          (-deck_w / 2, 9.0, z_back)), 6, 4, uv_scale=(6.0, 4.0)))
    gltf.add(grid_part("Screen_Main", led, ((-6.0, 1.0, z_back + 0.2), (6.0, 1.0, z_back + 0.2),
                                            (-6.0, 7.0, z_back + 0.2)), 6, 3))

    stage_dir = os.path.join(out_dir, stage_id)
    os.makedirs(os.path.join(stage_dir, "packs"), exist_ok=True)
    with open(os.path.join(stage_dir, stage_id + ".glb"), "wb") as f:
        f.write(gltf.build())

    # ---- packs/screen: animated bars from `time` and the `bars` param --------------------------------------------------
    screen_dir = os.path.join(stage_dir, "packs", "screen")
    os.makedirs(screen_dir, exist_ok=True)
    with open(os.path.join(screen_dir, "pack.json"), "w", encoding="utf-8") as f:
        json.dump({
            "format": 1,
            "id": "pack_test_screen",
            "version": "1.0.0",
            "apiVersion": 6,
            "type": "screen",
            "name": {"ko": "테스트 바 화면", "en": "Test bar screen"},
            "description": {"ko": "시간에 따라 흐르는 막대를 그리는 테스트 화면 팩.",
                            "en": "A test screen pack that draws bars scrolling with time."},
            "authors": [{"name": "MMDX stagegen", "role": "procedural"}],
            "license": "CC0-1.0",
            "tags": ["screen", "test"],
            "params": [
                {"key": "bars", "type": "float", "label": {"ko": "막대 수", "en": "Bars"},
                 "default": 8.0, "min": 1.0, "max": 16.0, "step": 1.0},
                {"key": "speed", "type": "float", "label": {"ko": "속도", "en": "Speed"},
                 "default": 0.35, "min": 0.0, "max": 2.0},
            ],
        }, f, ensure_ascii=False, indent=2)
    with open(os.path.join(screen_dir, "screen.hlsl"), "w", encoding="utf-8") as f:
        f.write("""// pack_test_screen's screen.hlsl: PackScreen (shaders/screen_api.hlsli, docs/shader_screen_api.md).
// Bars scroll with the song / timeline time and their count comes from the pack's `bars` parameter, so a still
// capture of an offline render shows a fixed pattern (time is the rendered frame's seconds).
#define P_BARS PackParamInt(0)     // bars: how many stripes across the screen
#define P_SPEED PackParam(1)       // speed: bars per second

float4 PackScreen(PackScreenInput i) {
    // three bands: the top third is the raw bar pattern, the middle adds a gradient, the bottom stays dark (a
    // reference the surface pack's tint does not reach)
    const float bars = max((float)P_BARS, 1.0);
    const float phase = i.time * P_SPEED * bars;
    const float bar = frac(i.uv.x * bars + phase);
    const float lit = smoothstep(0.0, 0.08, bar) * smoothstep(1.0, 0.92, bar);
    float3 c = lerp(float3(0.02, 0.03, 0.06), float3(0.35, 0.75, 1.0), lit);
    c *= lerp(0.55, 1.0, i.uv.y);                  // a gentle vertical falloff, like an LED wall
    if (i.uv.y < 0.3) c *= 0.25;                   // lower band: dim
    return float4(c, 1.0);
}
""")

    # ---- packs/surface: tints the materials it claims ---------------------------------------------------------------
    surf_dir = os.path.join(stage_dir, "packs", "surface")
    os.makedirs(surf_dir, exist_ok=True)
    with open(os.path.join(surf_dir, "pack.json"), "w", encoding="utf-8") as f:
        json.dump({
            "format": 1,
            "id": "pack_test_surface",
            "version": "1.0.0",
            "apiVersion": 6,
            "type": "surface",
            "name": {"ko": "테스트 틴트", "en": "Test tint"},
            "description": {"ko": "선택한 스테이지 머티리얼을 색으로 물들이는 테스트 표면 팩.",
                            "en": "A test surface pack that tints the stage materials it claims."},
            "authors": [{"name": "MMDX stagegen", "role": "procedural"}],
            "license": "CC0-1.0",
            "tags": ["surface", "test"],
            # a colour parameter reads through PackParamColor, which needs the params.ext block
            "requires": ["params.ext"],
            # no class rule names a stage material: ClassifyStage falls back to the first rule's class, which keeps
            # the claimed materials shaded by the pack
            "classes": [{"class": "weapon", "match": ["cove_wall"]}],
            "params": [
                {"key": "tint", "type": "color", "label": {"ko": "색", "en": "Tint"},
                 "default": [0.95, 0.45, 0.25]},
                {"key": "amount", "type": "float", "label": {"ko": "강도", "en": "Amount"},
                 "default": 0.85, "min": 0.0, "max": 1.0},
            ],
        }, f, ensure_ascii=False, indent=2)
    with open(os.path.join(surf_dir, "surface.hlsl"), "w", encoding="utf-8") as f:
        f.write("""// pack_test_surface's surface.hlsl: PackShade (shaders/pack_api.hlsli).
// Mixes the MMD lit colour with the `tint` parameter by `amount`, so the material it claims (cove_wall) reads clearly
// different from the parts the pack leaves alone.
#define P_TINT PackParamColor(0)    // tint (linear)
#define P_AMOUNT PackParam(1)       // amount 0..1

PackResult PackShade(PackSurface s) {
    PackResult r;
    r.alpha = s.alpha;
    r.reflectivity = 0.0;
    r.noAo = false;
    float3 lit = PackMmdLit(s);
    float3 tinted = lerp(lit, P_TINT * dot(lit, float3(0.299, 0.587, 0.114)) * 2.0, saturate(P_AMOUNT));
    r.color = SrgbToLinear(saturate(tinted)) * gSunIntensity;
    r.color += SrgbToLinear(saturate(tinted)) * PunctualDiffuse(s.worldPos, s.N, 1.0, 1.0);
    return r;
}
""")
    # the PT / offline GI form of the same tint (S6b): a stage part's surface pack shades PT / GI through
    # pt_surface.hlsl exactly like a character's pack, so the offline stills show the tint too.
    with open(os.path.join(surf_dir, "pt_surface.hlsl"), "w", encoding="utf-8") as f:
        f.write("""// pack_test_surface's pt_surface.hlsl: PackEvaluate (shaders/pt_pack_api.hlsli, docs/shader_pt_api.md).
// The PT / offline GI form of surface.hlsl's tint: the same `tint` / `amount` mix, so the offline stills show the
// material this pack claims in the same colour the raster / RT pass draws.
#define P_TINT PackParamColor(0)    // tint (linear)
#define P_AMOUNT PackParam(1)       // amount 0..1

PtPackOut PackEvaluate(PtPackIn i) {
    PtPackOut o;
    float3 tinted = lerp(i.baseColor, P_TINT * dot(i.baseColor, float3(0.299, 0.587, 0.114)) * 2.0,
                         saturate(P_AMOUNT));
    o.albedo = tinted;                     // the GI diffuse bounces carry the tint
    // the shaded side keeps the tint's hue (a unit-luminance copy, so the multiplier never brightens the terminator)
    float3 tintHue = P_TINT / max(max(P_TINT.r, max(P_TINT.g, P_TINT.b)), 1e-4);
    o.shadowTint = lerp(float3(0.7, 0.7, 0.75), tintHue, saturate(P_AMOUNT));
    o.shadowBias = 0.0;
    o.terminator = float2(0.0, 0.0);       // engine default softness
    o.specular = float3(0.0, 0.0, 0.0);
    o.flatFace = false;
    return o;
}
""")

    # ---- packs/world: the built-in snow pack, copied unchanged --------------------------------------------------------
    snow_src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "shaders", "packs", "snow")
    snow_src = os.path.normpath(snow_src)
    world_dir = os.path.join(stage_dir, "packs", "world")
    os.makedirs(world_dir, exist_ok=True)
    if os.path.isdir(snow_src):
        for name in os.listdir(snow_src):
            src = os.path.join(snow_src, name)
            if os.path.isfile(src) and (name == "pack.json" or name.endswith(".hlsl")):
                with open(src, "rb") as fi, open(os.path.join(world_dir, name), "wb") as fo:
                    fo.write(fi.read())
    else:
        raise SystemExit(f"the built-in snow pack was not found at {snow_src}")

    manifest = {
        "format": "mmdx-stage",
        "formatVersion": 1,
        "id": stage_id,
        "version": "1.0.0",
        "name": {"ko": "팩 테스트 스테이지", "en": "Pack test stage", "ja": "パックテストステージ"},
        "description": {"ko": "S6 번들 팩 픽스처: 프로그램 스크린, 표면 팩, 소켓에 붙은 월드 팩.",
                        "en": "The S6 bundled-pack fixture: a program screen, a surface pack and a world pack at a socket."},
        "authors": [{"name": "MMDX stagegen", "role": "procedural"}],
        "license": "CC0-1.0",
        "tags": ["test", "packs", "fixture"],
        "models": [{"file": stage_id + ".glb", "name": "Pack room", "role": "floor"}],
        "materials": [
            {"match": "stage_deck", "roughness": 0.3, "metallic": 0.2},
            {"match": "cove_wall", "roughness": 0.55},
            {"match": "led_main", "screen": "main", "castShadow": False},
        ],
        "lights": [
            light("sun", "key", intensity=0.5, color=(0.8, 0.84, 0.95), direction=(-0.25, -1.0, 0.35)),
            light("ambient", "ambient", intensity=0.12, color=(1.0, 1.0, 1.0)),
            light("spot", "front", intensity=2.0, color=(1.0, 0.94, 0.86), position=(-24.0, 40.0, 44.0),
                  aim=(0.0, 4.0, -20.0), cone=(0.3, 0.2)),
        ],
        "sockets": [
            {"name": "world", "position": [0.0, 6.0, -50.0]},
            {"name": "stage_center", "node": "Stage_Top"},
        ],
        "cameras": [
            {"name": "wide", "position": [0.0, 20.0, 120.0], "target": [0.0, 20.0, -40.0], "fov": 40.0},
        ],
        "screens": [
            {"id": "main", "materials": ["led_main"], "source": "program", "file": "packs/screen",
             "width": 1024, "height": 512, "brightness": 1.4, "emitLight": True},
        ],
        "packs": [
            {"folder": "packs/surface", "role": "surface", "materials": ["cove_wall"],
             "params": {"amount": 0.85}},
            {"folder": "packs/world", "role": "world", "socket": "world",
             "params": {"density": 0.85, "flake_size": 0.08}},
        ],
        "params": [
            {"key": "barCount", "type": "float", "label": {"ko": "막대 수", "en": "Bar count"},
             "default": 8.0, "min": 1.0, "max": 16.0},
            {"key": "tintAmount", "type": "float", "label": {"ko": "틴트 강도", "en": "Tint amount"},
             "default": 0.85, "min": 0.0, "max": 1.0},
        ],
        "variants": [
            {"id": "mixed", "label": {"ko": "혼합", "en": "Mixed"},
             "values": {"barCount": 8.0, "tintAmount": 0.85}},
            {"id": "calm", "label": {"ko": "차분", "en": "Calm"},
             "values": {"barCount": 14.0, "tintAmount": 0.35}},
        ],
        "defaultVariant": "mixed",
        # `packs.<index>` is the array position: 0 = the surface pack, 1 = the world pack
        "bindings": [
            {"param": "tintAmount", "target": "packs.0.params.amount"},
        ],
        "cues": [
            {"param": "barCount", "keys": [{"frame": 0, "value": 8.0}, {"frame": 120, "value": 14.0}]},
        ],
    }
    # the screen's bars parameter: the pack is named by `screens[].source` (no packs[] entry), so its params come
    # from a packs[] entry on the same folder — bind the exposed barCount there
    manifest["packs"].append({"folder": "packs/screen", "role": "screen", "screen": "main",
                              "params": {"bars": 8.0, "speed": 0.35}})
    manifest["bindings"].append({"param": "barCount", "target": "packs.2.params.bars"})
    with open(os.path.join(stage_dir, "stage.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    return stage_dir


def build_gobo_test(out_dir):
    """S7b fixture: spot gobos projected onto a floor and a back wall.

    Two gobo images are written next to stage.json (`gobos/*.png`): a soft ring stencil for the floor spots and a
    coloured stripe stencil for the wall spot. glTF geometry is in metres (the importer scales it by 12.5 to MMD
    units); `lights` / `cameras` are in MMD units, as in every other stage here. One spot spins (`gobo.spin`, keyed
    through a cue) so the pattern's motion is covered too.
    """
    stage_id = "gobo_test"
    gltf = Gltf(stage_id)
    floor = gltf.material("floor_stone", base_color=(0.62, 0.62, 0.64, 1.0), roughness=0.55, metallic=0.0)
    wall = gltf.material("back_wall", base_color=(0.70, 0.69, 0.71, 1.0), roughness=0.75, metallic=0.0)
    truss = gltf.material("truss_metal", base_color=(0.55, 0.56, 0.58, 1.0), roughness=0.35, metallic=1.0)

    # metres: a 12 x 9 m room with a 5 m back wall (150 x 112 MMD units, the character ~1.6 m = 20 units)
    w, d, h = 12.0, 9.0, 5.0
    z_back = -d / 2
    # corner order picks the normal: +y for the floor (z edge first), +z for the wall (audience side)
    gltf.add(grid_part("Floor", floor, ((-w / 2, 0.0, -d / 2), (-w / 2, 0.0, d / 2), (w / 2, 0.0, -d / 2)), 6, 5,
                       uv_scale=(6.0, 5.0)))
    gltf.add(grid_part("Wall", wall, ((-w / 2, 0.0, z_back), (w / 2, 0.0, z_back), (-w / 2, h, z_back)), 6, 3,
                       uv_scale=(6.0, 3.0)))
    for part in box_parts("Truss", truss, (0.0, h + 0.2, z_back + 1.0), (w, 0.2, 0.2)):
        gltf.add(part)

    stage_dir = os.path.join(out_dir, stage_id)
    os.makedirs(stage_dir, exist_ok=True)
    with open(os.path.join(stage_dir, stage_id + ".glb"), "wb") as f:
        f.write(gltf.build())

    # ---- the gobo images: a soft ring (floor) and coloured stripes (wall) -------------------------------------------
    def ring(x, y, size=128):
        """Concentric bands filling the whole stencil: uv radius 1 is the cone's edge (gobo.hlsli GoboUv)."""
        cx = cy = (size - 1) / 2.0
        r = math.sqrt((x - cx) ** 2 + (y - cy) ** 2) / (size * 0.5)
        ringIndex = int(r / 0.2)
        v = 235 if ringIndex % 2 == 0 else 25
        if r < 0.09:
            v = 235   # a solid centre dot
        return (v, v, v)

    def stripes(x, y, size=128):
        # vertical bars in the red / blue channels, green darker: a coloured gobo tints the spot
        on = ((x * 8) // size) % 2 == 0
        return ((230 if on else 20), (90 if on else 10), (200 if on else 40))

    write_png(os.path.join(stage_dir, "gobos", "ring.png"), 128, 128, ring)
    write_png(os.path.join(stage_dir, "gobos", "stripes.png"), 128, 128, stripes)

    lights = [
        light("sun", "key", intensity=0.14, color=(0.55, 0.60, 0.75), direction=(0.2, -1.0, 0.35)),
        light("ambient", "ambient", intensity=0.04, color=(1.0, 1.0, 1.0)),
        # floor spots: the ring stencil straight down from the truss, one of them spinning (MMD units)
        light("spot", "floor_ring_a", intensity=40.0, color=(1.0, 0.96, 0.90), position=(-38.0, 105.0, 30.0),
              aim=(-38.0, 0.0, 30.0), cone=(0.26, 0.16), range_=320.0),
        light("spot", "floor_ring_b", intensity=40.0, color=(0.85, 0.92, 1.0), position=(38.0, 105.0, 30.0),
              aim=(38.0, 0.0, 30.0), cone=(0.26, 0.16), range_=320.0),
        # wall spot: the coloured stripe stencil across the back wall
        light("spot", "wall_stripes", intensity=14.0, color=(0.95, 0.95, 1.0), position=(0.0, 62.0, 20.0),
              aim=(0.0, 42.0, z_back * 12.5), cone=(0.34, 0.20), range_=320.0),
    ]
    # The ring is rotation-invariant, so the spinning / coloured one is the striped stencil (its rotation is visible):
    # floor_ring_a = the still ring, floor_ring_b = the coloured stripes spinning + slightly out of focus, wall_stripes =
    # the same stencil rotated 90 degrees across the back wall.
    lights[2]["gobo"] = {"file": "gobos/ring.png", "rotation": 0.0, "scale": 1.0, "spin": 0.0, "focus": 1.0}
    lights[3]["gobo"] = {"file": "gobos/stripes.png", "rotation": 20.0, "scale": 1.2, "spin": 45.0, "focus": 0.8}
    lights[4]["gobo"] = {"file": "gobos/stripes.png", "rotation": 90.0, "scale": 1.4, "spin": 0.0, "focus": 1.0}

    manifest = {
        "format": "mmdx-stage",
        "formatVersion": 1,
        "id": stage_id,
        "version": "1.0.0",
        "name": {"ko": "고보 테스트 스테이지", "en": "Gobo test stage", "ja": "ゴボテストステージ"},
        "description": {"ko": "스팟 고보(투영 텍스처) 테스트: 바닥의 링과 회전하는 색 줄무늬, 벽의 돌아간 줄무늬, 안개 켜짐.",
                        "en": "Spot gobo test: a ring and spinning coloured stripes on the floor, the stripes rotated on the wall."},
        "authors": [{"name": "MMDX stagegen", "role": "procedural"}],
        "license": "CC0-1.0",
        "tags": ["gobo", "spot", "test"],
        "models": [{"file": stage_id + ".glb", "name": "Room", "role": "floor"}],
        "materials": [
            {"match": "floor_stone", "baseColor": [0.62, 0.62, 0.64, 1.0], "roughness": 0.55},
            {"match": "back_wall", "baseColor": [0.70, 0.69, 0.71, 1.0], "roughness": 0.75},
            {"match": "truss_metal", "metallic": 1.0, "roughness": 0.35},
        ],
        "lights": lights,
        "sockets": [
            {"name": "world", "position": [0.0, 40.0, -160.0]},
            {"name": "stage_center", "position": [0.0, 25.0, 0.0]},
        ],
        "cameras": [
            {"name": "wide", "position": [0.0, 75.0, 210.0], "target": [0.0, 40.0, -30.0], "fov": 42.0},
            {"name": "floor", "position": [0.0, 55.0, 95.0], "target": [0.0, 5.0, -10.0], "fov": 50.0},
        ],
        "environment": {
            "fog": {"density": 0.0012, "color": [0.55, 0.60, 0.72], "anisotropy": 0.5, "heightFalloff": 0.008},
            "ambient": {"intensity": 0.03, "skyZenith": [0.03, 0.04, 0.08], "ground": [0.05, 0.05, 0.06]},
        },
        "groups": [{"id": "gobos", "label": {"ko": "고보", "en": "Gobos"}}],
        "params": [
            {"key": "goboSpin", "type": "float", "label": {"ko": "고보 회전 속도", "en": "Gobo spin"},
             "group": "gobos", "default": 45.0, "min": -180.0, "max": 180.0},
            {"key": "goboFocus", "type": "float", "label": {"ko": "고보 초점", "en": "Gobo focus"},
             "group": "gobos", "default": 1.0, "min": 0.0, "max": 1.0},
        ],
        "variants": [
            {"id": "default", "label": {"ko": "기본", "en": "Default"},
             "values": {"goboSpin": 45.0, "goboFocus": 1.0}},
        ],
        "defaultVariant": "default",
        "bindings": [
            {"param": "goboSpin", "target": "lights.floor_ring_b.gobo.spin"},
            {"param": "goboFocus", "target": "lights.floor_ring_b.gobo.focus"},
        ],
        "cues": [
            {"param": "goboSpin", "keys": [{"frame": 0, "value": 0.0}, {"frame": 300, "value": 90.0},
                                           {"frame": 600, "value": 0.0}]},
        ],
    }
    with open(os.path.join(stage_dir, "stage.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    return stage_dir


def build_screen_light_test(out_dir):
    """S8b fixture: a bright image screen behind the performer over a floor, so the screen's own average colour shows on
    the floor and the character in raster / RT exactly where PT puts it.

    Geometry is in metres (the importer scales by 12.5); `lights` / `cameras` are in MMD units like every other stage.
    The screen is a 6 x 3.5 m panel 4 m behind the performer, facing MMD +z (toward the performer and the default
    camera), the floor is 16 x 16 m, and the screen's image is a saturated colour split (amber / cyan) so its light is
    unmistakable against the neutral key.
    """
    stage_id = "screen_light_test"
    gltf = Gltf(stage_id)
    floor = gltf.material("floor_light", base_color=(0.62, 0.62, 0.63, 1.0), roughness=0.55, metallic=0.0)
    screen = gltf.material("led_test", base_color=(0.02, 0.02, 0.03, 1.0), roughness=0.35, metallic=0.0,
                           emissive=(0.5, 0.4, 0.2), double_sided=True)

    # The floor only: no wall, so nothing but the floor and the performer is lit (the QA capture reads the screen's
    # light on the floor and the character without a wall in between).
    w, d = 16.0, 16.0
    gltf.add(grid_part("Floor", floor, ((-w / 2, 0.0, -d / 2), (-w / 2, 0.0, d / 2), (w / 2, 0.0, -d / 2)),
                       8, 8, uv_scale=(8.0, 8.0)))
    # The screen: 6 x 3.5 m, 4 m behind the performer, facing MMD +z (toward the performer and the default camera).
    # The importer mirrors glTF z (z -> -z) and flips the vertex normals with it, so the panel's glTF z = -4 lands at
    # MMD z = +4 (behind the performer) and the corner order below (glTF normal -z) becomes MMD +z.
    sy, sz = 3.5, 4.0
    gltf.add(grid_part("Screen_Test", screen, ((3.0, 0.4, sz), (-3.0, 0.4, sz), (3.0, 0.4 + sy, sz)), 6, 4,
                       uv_scale=(1.0, 1.0)))

    stage_dir = os.path.join(out_dir, stage_id)
    os.makedirs(stage_dir, exist_ok=True)
    with open(os.path.join(stage_dir, stage_id + ".glb"), "wb") as f:
        f.write(gltf.build())

    # the screen's image: amber on the left, cyan on the right, with a soft gradient (a big, bright, unmistakable panel)
    def split(x, y, width=64, height=36):
        fx = x / (width - 1)
        fy = y / (height - 1)
        if fx < 0.5:
            return (int(235 - 40 * fy), int(150 + 60 * fy), int(40 + 30 * fy))
        return (int(30 + 40 * fy), int(190 + 50 * fy), int(230 - 20 * fy))

    write_png(os.path.join(stage_dir, "textures", "led_test.png"), 64, 36, split)

    manifest = {
        "format": "mmdx-stage",
        "formatVersion": 1,
        "id": stage_id,
        "version": "1.0.0",
        "name": {"ko": "스크린 조명 테스트", "en": "Screen light test", "ja": "スクリーンライトテスト"},
        "description": {"ko": "S8b: 캐릭터 옆의 밝은 이미지 스크린이 바닥과 캐릭터를 비춘다 (래스터 / RT / PT 비교).",
                        "en": "S8b: a bright image screen beside the performer lights the floor and the character "
                              "(raster / RT / PT comparison)."},
        "authors": [{"name": "MMDX stagegen", "role": "procedural"}],
        "license": "CC0-1.0",
        "tags": ["screen", "led", "light", "test"],
        "models": [{"file": stage_id + ".glb", "name": "Room", "role": "floor"}],
        "materials": [
            {"match": "floor_light", "baseColor": [0.62, 0.62, 0.63, 1.0], "roughness": 0.55},
            {"match": "led_test", "screen": "main", "castShadow": False},
        ],
        "lights": [
            # a dim neutral key + ambient only: the screen's own light must read
            light("sun", "key", intensity=0.08, color=(0.65, 0.68, 0.80), direction=(0.2, -1.0, 0.35)),
            light("ambient", "ambient", intensity=0.05, color=(1.0, 1.0, 1.0)),
        ],
        "sockets": [
            {"name": "world", "position": [0.0, 30.0, -140.0]},
            {"name": "stage_center", "position": [0.0, 15.0, 0.0]},
        ],
        "cameras": [
            {"name": "wide", "position": [0.0, 22.0, 150.0], "target": [-10.0, 24.0, -40.0], "fov": 40.0},
            {"name": "screen", "position": [-30.0, 20.0, 90.0], "target": [-25.0, 22.0, -40.0], "fov": 42.0},
        ],
        "screens": [
            {"id": "main", "materials": ["led_test"], "source": "image", "file": "textures/led_test.png",
             "width": 2048, "height": 1024, "brightness": 2.0, "emitLight": True},
        ],
        "environment": {
            "fog": {"density": 0.0008, "color": [0.55, 0.60, 0.72], "anisotropy": 0.5, "heightFalloff": 0.006},
            "ambient": {"intensity": 0.05, "skyZenith": [0.03, 0.04, 0.08], "ground": [0.05, 0.05, 0.06]},
        },
    }
    with open(os.path.join(stage_dir, "stage.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    return stage_dir


def build_sky_test(out_dir):
    """S9b-1 fixture: an open floor with a few pillars under a Physical Sky light whose time of day is keyed
    6.5 -> 12 -> 18.5 over the first 30 s (a low orange dawn sun, a blue noon, a low sunset), no fog.

    Geometry is in metres (the importer scales by 12.5 to MMD units); `lights` / `cameras` are in MMD units like every
    other stage. There is no sun light: the sky light owns the sun (direction from the time of day, colour from the
    model), and no ambient light either: the model writes the ambient colours itself.
    """
    stage_id = "sky_test"
    gltf = Gltf(stage_id)
    floor = gltf.material("floor_open", base_color=(0.58, 0.58, 0.60, 1.0), roughness=0.62, metallic=0.0)
    pillar = gltf.material("pillar_stone", base_color=(0.72, 0.71, 0.68, 1.0), roughness=0.55, metallic=0.0)

    w, d = 26.0, 26.0
    gltf.add(grid_part("Floor", floor, ((-w / 2, 0.0, -d / 2), (-w / 2, 0.0, d / 2), (w / 2, 0.0, -d / 2)),
                       10, 10, uv_scale=(10.0, 10.0)))
    # four pillars on a 2 x 2 ring: they throw the sun's shadow across the floor through the day
    for i, (px, pz) in enumerate(((-9.0, -9.0), (9.0, -9.0), (-9.0, 9.0), (9.0, 9.0))):
        for part in box_parts(f"Pillar_{i}", pillar, (px, 2.5, pz), (0.45, 2.5, 0.45)):
            gltf.add(part)

    stage_dir = os.path.join(out_dir, stage_id)
    os.makedirs(stage_dir, exist_ok=True)
    with open(os.path.join(stage_dir, stage_id + ".glb"), "wb") as f:
        f.write(gltf.build())

    manifest = {
        "format": "mmdx-stage",
        "formatVersion": 1,
        "id": stage_id,
        "version": "1.0.0",
        "name": {"ko": "하늘 조명 테스트", "en": "Physical sky test", "ja": "物理スカイテスト"},
        "description": {"ko": "S9b-1: 하늘 조명 하나가 배경과 태양 색을 정한다. 시간대 6.5 -> 12 -> 18.5 (30초).",
                        "en": "S9b-1: one Physical Sky light drives the background and the sun colour; the time of "
                              "day runs 6.5 -> 12 -> 18.5 over 30 s."},
        "authors": [{"name": "MMDX stagegen", "role": "procedural"}],
        "license": "CC0-1.0",
        "tags": ["sky", "light", "test"],
        "models": [{"file": stage_id + ".glb", "name": "Room", "role": "floor"}],
        "materials": [
            {"match": "floor_open", "baseColor": [0.58, 0.58, 0.60, 1.0], "roughness": 0.62},
            {"match": "pillar_stone", "baseColor": [0.72, 0.71, 0.68, 1.0], "roughness": 0.55},
        ],
        "lights": [
            # one Physical Sky light, no sun light: the sky owns the sun. The ambient light only supplies the
            # hemisphere fill's strength (the sky light writes its colours), so the floor and the pillars read even
            # when the sun is near the horizon.
            {"kind": "sky", "name": "sky", "enabled": True,
             "values": {"skyTimeOfDay": 6.5, "skyTurbidity": 3.2, "skyHaze": 0.35, "skyNorthDeg": 35.0,
                        "skyGround": [0.34, 0.33, 0.31], "skyIntensity": 1.0, "skySunIntensity": 1.0,
                        "skyMaxElevationDeg": 60.0}},
            {"kind": "ambient", "name": "ambient", "enabled": True, "values": {"intensity": 0.18}},
        ],
        "sockets": [
            {"name": "world", "position": [0.0, 30.0, -160.0]},
            {"name": "stage_center", "position": [0.0, 15.0, 0.0]},
        ],
        "cameras": [
            # raised toward the sky: the dawn / noon / sunset half of the frame must be sky, not floor
            {"name": "wide", "position": [0.0, 26.0, 180.0], "target": [0.0, 52.0, -70.0], "fov": 50.0},
        ],
        "environment": {
            # no fog: the sky's own colour must read on the floor and the pillars (S9's haze is not this fixture)
            "fog": {"density": 0.0, "color": [0.6, 0.65, 0.75], "anisotropy": 0.5, "heightFalloff": 0.008},
        },
        "groups": [{"id": "sky", "label": {"ko": "하늘", "en": "Sky"}}],
        "params": [
            {"key": "timeOfDay", "type": "float", "label": {"ko": "시간", "en": "Time of day"},
             "group": "sky", "default": 12.0, "min": 0.0, "max": 24.0},
        ],
        "variants": [{"id": "default", "label": {"ko": "기본", "en": "Default"}, "values": {"timeOfDay": 6.5}}],
        "defaultVariant": "default",
        "bindings": [{"param": "timeOfDay", "target": "lights.sky.sky.timeOfDay"}],
        # dawn -> noon -> sunset over the first 30 s (30 fps). The last key is sunset itself (the sun exactly on the
        # horizon): a little later and the sun's colour is already gone and the frame is a dark dusk.
        "cues": [
            {"param": "timeOfDay", "keys": [{"frame": 0, "value": 6.5}, {"frame": 450, "value": 12.0},
                                            {"frame": 900, "value": 18.0}]},
        ],
    }
    with open(os.path.join(stage_dir, "stage.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    return stage_dir


def build_sky_cloud_test(out_dir):
    """S9b-4 fixture: the sky_test floor + pillars under a Physical Sky light whose cloud layer is on (coverage 0.5,
    softness 0.3, wind 90 deg / 25 m/s) and whose time of day runs 7 -> 12 -> 17 over the first 30 s.

    Geometry is in metres (the importer scales by 12.5 to MMD units); `lights` / `cameras` are in MMD units like every
    other stage. As in sky_test there is no sun light and no fog: the sky light owns the sun and the background.
    """
    stage_id = "sky_cloud_test"
    gltf = Gltf(stage_id)
    floor = gltf.material("floor_open", base_color=(0.58, 0.58, 0.60, 1.0), roughness=0.62, metallic=0.0)
    pillar = gltf.material("pillar_stone", base_color=(0.72, 0.71, 0.68, 1.0), roughness=0.55, metallic=0.0)

    w, d = 26.0, 26.0
    gltf.add(grid_part("Floor", floor, ((-w / 2, 0.0, -d / 2), (-w / 2, 0.0, d / 2), (w / 2, 0.0, -d / 2)),
                       10, 10, uv_scale=(10.0, 10.0)))
    for i, (px, pz) in enumerate(((-9.0, -9.0), (9.0, -9.0), (-9.0, 9.0), (9.0, 9.0))):
        for part in box_parts(f"Pillar_{i}", pillar, (px, 2.5, pz), (0.45, 2.5, 0.45)):
            gltf.add(part)

    stage_dir = os.path.join(out_dir, stage_id)
    os.makedirs(stage_dir, exist_ok=True)
    with open(os.path.join(stage_dir, stage_id + ".glb"), "wb") as f:
        f.write(gltf.build())

    manifest = {
        "format": "mmdx-stage",
        "formatVersion": 1,
        "id": stage_id,
        "version": "1.0.0",
        "name": {"ko": "구름 하늘 테스트", "en": "Cloudy sky test", "ja": "クラウドスカイテスト"},
        "description": {"ko": "S9b-4: 하늘 조명의 구름층 (피복 0.5, 부드러움 0.3, 바람 90도 25 m/s). 시간대 7 -> 12 -> 17 (30초).",
                        "en": "S9b-4: a cloud layer on the Physical Sky light (coverage 0.5, softness 0.3, wind "
                              "90 deg / 25 m/s); the time of day runs 7 -> 12 -> 17 over 30 s."},
        "authors": [{"name": "MMDX stagegen", "role": "procedural"}],
        "license": "CC0-1.0",
        "tags": ["sky", "cloud", "light", "test"],
        "models": [{"file": stage_id + ".glb", "name": "Room", "role": "floor"}],
        "materials": [
            {"match": "floor_open", "baseColor": [0.58, 0.58, 0.60, 1.0], "roughness": 0.62},
            {"match": "pillar_stone", "baseColor": [0.72, 0.71, 0.68, 1.0], "roughness": 0.55},
        ],
        "lights": [
            # one Physical Sky light with clouds, no sun light (the sky owns the sun). The ambient light only supplies
            # the hemisphere fill's strength, as in sky_test.
            {"kind": "sky", "name": "sky", "enabled": True,
             "values": {"skyTimeOfDay": 7.0, "skyTurbidity": 3.2, "skyHaze": 0.35, "skyNorthDeg": 35.0,
                        "skyGround": [0.34, 0.33, 0.31], "skyIntensity": 1.0, "skySunIntensity": 1.0,
                        "skyMaxElevationDeg": 60.0, "cloudCoverage": 0.5, "cloudOpacity": 1.0,
                        "cloudSoftness": 0.3, "cloudHeightKm": 2.0, "cloudScaleKm": 1.5,
                        "cloudWindDeg": 90.0, "cloudWindSpeed": 25.0}},
            {"kind": "ambient", "name": "ambient", "enabled": True, "values": {"intensity": 0.18}},
        ],
        "sockets": [
            {"name": "world", "position": [0.0, 30.0, -160.0]},
            {"name": "stage_center", "position": [0.0, 15.0, 0.0]},
        ],
        "cameras": [
            # raised toward the sky, as sky_test: most of the frame is the cloud layer
            {"name": "wide", "position": [0.0, 26.0, 180.0], "target": [0.0, 52.0, -70.0], "fov": 50.0},
        ],
        "environment": {
            "fog": {"density": 0.0, "color": [0.6, 0.65, 0.75], "anisotropy": 0.5, "heightFalloff": 0.008},
        },
        "groups": [{"id": "sky", "label": {"ko": "하늘", "en": "Sky"}}],
        "params": [
            {"key": "timeOfDay", "type": "float", "label": {"ko": "시간", "en": "Time of day"},
             "group": "sky", "default": 12.0, "min": 0.0, "max": 24.0},
        ],
        "variants": [{"id": "default", "label": {"ko": "기본", "en": "Default"}, "values": {"timeOfDay": 7.0}}],
        "defaultVariant": "default",
        "bindings": [{"param": "timeOfDay", "target": "lights.sky.sky.timeOfDay"}],
        # morning -> noon -> late afternoon over the first 30 s (30 fps): the clouds drift 0.75 km toward +x meanwhile
        "cues": [
            {"param": "timeOfDay", "keys": [{"frame": 0, "value": 7.0}, {"frame": 450, "value": 12.0},
                                            {"frame": 900, "value": 17.0}]},
        ],
    }
    with open(os.path.join(stage_dir, "stage.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    return stage_dir


def write_sky_map_hdr(path, width=1024, height=512):
    """S9b-3: a Radiance RGBE .hdr equirect test panorama (flat scanlines, 4 bytes per texel).

    The content matches the shader's mapping (shaders/sky_map.hlsli): u = atan2(x, z) / 2PI + 0.5 (u = 0.5 faces +Z),
    v = acos(y) / PI (v = 0 is the zenith). Above the horizon the sky lerps from a hazy horizon to a deep zenith blue;
    below it is a dull ground. Three 10 deg tall / 20 deg wide colour markers just above the horizon (red at +Z, green
    at +X, blue at -X) make the panorama's orientation readable, and a small bright sun sits at u = 0.625, 35 deg up.
    """
    import math

    horizon = (0.70, 0.75, 0.80)
    zenith = (0.15, 0.30, 0.70)
    ground = (0.20, 0.18, 0.15)
    markers = [(0.50, (4.0, 0.2, 0.2)), (0.75, (0.2, 4.0, 0.2)), (0.25, (0.2, 0.2, 4.0))]
    sun_u, sun_elev = 0.625, 35.0
    rows = []
    for y in range(height):
        v = (y + 0.5) / height
        dir_y = math.cos(math.pi * v)          # dir.y = cos(theta), theta = PI v
        elev = math.degrees(math.asin(max(-1.0, min(1.0, dir_y))))
        row = bytearray()
        for x in range(width):
            u = (x + 0.5) / width
            if dir_y >= 0.0:
                t = dir_y ** 0.5
                r = horizon[0] + (zenith[0] - horizon[0]) * t
                g = horizon[1] + (zenith[1] - horizon[1]) * t
                b = horizon[2] + (zenith[2] - horizon[2]) * t
                for mu, colour in markers:
                    # the u distance folded around the panorama's seam
                    du = abs((u - mu + 0.5) % 1.0 - 0.5)
                    if du <= (20.0 / 360.0) * 0.5 and 0.0 <= elev <= 10.0:
                        r, g, b = colour
                du = abs((u - sun_u + 0.5) % 1.0 - 0.5)
                if math.hypot(du * 360.0 * math.cos(math.radians(elev)), elev - sun_elev) <= 2.0:
                    r, g, b = (300.0, 280.0, 250.0)
            else:
                r, g, b = ground
            m = max(r, g, b)
            if m < 1e-32:
                row += bytes((0, 0, 0, 0))
                continue
            mant, e = math.frexp(m)
            s = mant * 256.0 / m
            row += bytes((min(255, int(r * s)), min(255, int(g * s)), min(255, int(b * s)), e + 128))
        rows.append(bytes(row))

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n")
        f.write(f"-Y {height} +X {width}\n".encode("ascii"))
        for row in rows:
            f.write(row)


def build_sky_map_test(out_dir):
    """S9b-3 fixture: the sky_test floor + pillars under one Sky light whose IMAGE is sky/test_sky.hdr, no fog and no
    sun (skySunIntensity 0: the panorama is the only sky and its markers show the mapping).

    Geometry is in metres (the importer scales by 12.5 to MMD units); `lights` / `cameras` are in MMD units like every
    other stage.
    """
    stage_id = "sky_map_test"
    gltf = Gltf(stage_id)
    floor = gltf.material("floor_open", base_color=(0.58, 0.58, 0.60, 1.0), roughness=0.62, metallic=0.0)
    pillar = gltf.material("pillar_stone", base_color=(0.72, 0.71, 0.68, 1.0), roughness=0.55, metallic=0.0)

    w, d = 26.0, 26.0
    gltf.add(grid_part("Floor", floor, ((-w / 2, 0.0, -d / 2), (-w / 2, 0.0, d / 2), (w / 2, 0.0, -d / 2)),
                       10, 10, uv_scale=(10.0, 10.0)))
    for i, (px, pz) in enumerate(((-9.0, -9.0), (9.0, -9.0), (-9.0, 9.0), (9.0, 9.0))):
        for part in box_parts(f"Pillar_{i}", pillar, (px, 2.5, pz), (0.45, 2.5, 0.45)):
            gltf.add(part)

    stage_dir = os.path.join(out_dir, stage_id)
    os.makedirs(stage_dir, exist_ok=True)
    with open(os.path.join(stage_dir, stage_id + ".glb"), "wb") as f:
        f.write(gltf.build())
    write_sky_map_hdr(os.path.join(stage_dir, "sky", "test_sky.hdr"))

    manifest = {
        "format": "mmdx-stage",
        "formatVersion": 1,
        "id": stage_id,
        "version": "1.0.0",
        "name": {"ko": "하늘 이미지 테스트", "en": "Image sky test", "ja": "イメージスカイテスト"},
        "description": {"ko": "S9b-3: 하늘 조명 하나가 HDRI 파노라마를 배경과 환경광으로 쓴다 (표식: +Z 빨강, +X 초록, -X 파랑).",
                        "en": "S9b-3: one Sky light uses an HDRI panorama as the background and the environment light "
                              "(markers: +Z red, +X green, -X blue)."},
        "authors": [{"name": "MMDX stagegen", "role": "procedural"}],
        "license": "CC0-1.0",
        "tags": ["sky", "light", "hdri", "test"],
        "models": [{"file": stage_id + ".glb", "name": "Room", "role": "floor"}],
        "materials": [
            {"match": "floor_open", "baseColor": [0.58, 0.58, 0.60, 1.0], "roughness": 0.62},
            {"match": "pillar_stone", "baseColor": [0.72, 0.71, 0.68, 1.0], "roughness": 0.55},
        ],
        "lights": [
            # one Sky light with an image: the panorama is the background and (through the sky environment) the ambient
            # light. Its own sun is turned off, so the image's markers are the only bright thing in the sky.
            {"kind": "sky", "name": "sky", "enabled": True,
             "skyMap": "sky/test_sky.hdr", "skyMapEncoding": "auto", "skyMapMirror": False,
             "values": {"skyTimeOfDay": 12.0, "skyTurbidity": 3.2, "skyHaze": 0.35, "skyNorthDeg": 35.0,
                        "skyGround": [0.34, 0.33, 0.31], "skyIntensity": 1.0, "skySunIntensity": 0.0,
                        "skyMaxElevationDeg": 60.0, "skyMapIntensity": 1.0}},
            {"kind": "ambient", "name": "ambient", "enabled": True, "values": {"intensity": 0.18}},
        ],
        "sockets": [
            {"name": "world", "position": [0.0, 30.0, -160.0]},
            {"name": "stage_center", "position": [0.0, 15.0, 0.0]},
        ],
        "cameras": [
            {"name": "wide", "position": [0.0, 26.0, 180.0], "target": [0.0, 52.0, -70.0], "fov": 50.0},
        ],
        "environment": {
            # no fog: the image's own colours must read on the floor and the pillars
            "fog": {"density": 0.0, "color": [0.6, 0.65, 0.75], "anisotropy": 0.5, "heightFalloff": 0.008},
        },
        "groups": [{"id": "sky", "label": {"ko": "하늘", "en": "Sky"}}],
        "params": [
            {"key": "rotation", "type": "float", "label": {"ko": "회전", "en": "Rotation"},
             "group": "sky", "default": 0.0, "min": -180.0, "max": 180.0},
        ],
        "variants": [{"id": "default", "label": {"ko": "기본", "en": "Default"}, "values": {"rotation": 0.0}}],
        "defaultVariant": "default",
        "bindings": [{"param": "rotation", "target": "lights.sky.sky.mapRotation"}],
        # a slow turn of the panorama over the first 30 s: the QA capture at 5 s reads it turned by a third
        "cues": [
            {"param": "rotation", "keys": [{"frame": 0, "value": 0.0}, {"frame": 900, "value": 120.0}]},
        ],
    }
    with open(os.path.join(stage_dir, "stage.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    return stage_dir


STAGES = {
    "cyclorama": build_cyclorama,
    "mirror_room": build_mirror_room,
    "led_stage": build_led_stage,
    "pack_test": build_pack_test,
    "gobo_test": build_gobo_test,
    "screen_light_test": build_screen_light_test,
    "sky_test": build_sky_test,
    "sky_cloud_test": build_sky_cloud_test,
    "sky_map_test": build_sky_map_test,
    "glass_room": build_glass_room,
}


def main():
    parser = argparse.ArgumentParser(description="write the procedural MMDX test stages")
    parser.add_argument("--out", default=os.path.join("captures", "stages"), help="output folder")
    parser.add_argument("--only", default="", help="comma separated stage names (default: all)")
    args = parser.parse_args()
    root = os.path.abspath(args.out)
    os.makedirs(root, exist_ok=True)
    names = [n.strip() for n in args.only.split(",") if n.strip()] or list(STAGES)
    for name in names:
        if name not in STAGES:
            raise SystemExit(f"unknown stage: {name} (known: {', '.join(STAGES)})")
        os.makedirs(os.path.join(root, name), exist_ok=True)
        written = STAGES[name](root)
        size = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(written) for f in fs)
        print(f"{name}: {os.path.relpath(written, os.getcwd())} ({size / 1024.0:.1f} KiB)")


if __name__ == "__main__":
    main()
