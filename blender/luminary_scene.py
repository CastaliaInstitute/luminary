"""Build and render the Luminary shadow-box concept.

Run from the repository root:
  Blender.app/Contents/MacOS/Blender --background --python blender/luminary_scene.py

The scene is an illustrative product render, not a measured optical simulation.
"""

import bpy
import math
import os
from mathutils import Vector

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "renders")
os.makedirs(OUT, exist_ok=True)

# Palette
INK = (0.015, 0.025, 0.028, 1)
ROCK = (0.035, 0.055, 0.060, 1)
WHITE = (0.88, 0.86, 0.77, 1)
FRAME = (0.06, 0.045, 0.035, 1)
SKY = (0.12, 0.29, 0.42, 1)
OCEAN = (0.04, 0.22, 0.25, 1)
GLOW = (0.94, 0.54, 0.17, 1)


def mat(name, color, metallic=0.0, roughness=0.55, emission=None, strength=0.0):
    m = bpy.data.materials.new(name)
    m.diffuse_color = color
    m.use_nodes = True
    bsdf = m.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = color
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Metallic"].default_value = metallic
    if emission:
        bsdf.inputs["Emission Color"].default_value = emission
        bsdf.inputs["Emission Strength"].default_value = strength
    return m


def cube(name, loc, scale, material, bevel=0.0):
    bpy.ops.mesh.primitive_cube_add(location=loc)
    ob = bpy.context.object
    ob.name = name
    ob.scale = (scale[0] / 2, scale[1] / 2, scale[2] / 2)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if material:
        ob.data.materials.append(material)
    if bevel:
        mod = ob.modifiers.new("soft edges", "BEVEL")
        mod.width = bevel
        mod.segments = 3
    return ob


def prism(name, points, y, depth, material, bevel=0.0):
    # Polygon points are x,z pairs; y is the center of the printed layer.
    verts = [(x, y - depth / 2, z) for x, z in points]
    verts += [(x, y + depth / 2, z) for x, z in points]
    n = len(points)
    faces = [tuple(range(n - 1, -1, -1)), tuple(range(n, 2 * n))]
    for i in range(n):
        j = (i + 1) % n
        faces.append((i, j, n + j, n + i))
    mesh = bpy.data.meshes.new(name + " mesh")
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    ob = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(ob)
    ob.data.materials.append(material)
    if bevel:
        mod = ob.modifiers.new("printed edge softness", "BEVEL")
        mod.width = bevel
        mod.segments = 2
    return ob


def disk(name, loc, radius, material, depth=0.8):
    bpy.ops.mesh.primitive_cylinder_add(vertices=64, radius=radius, depth=depth,
                                        location=loc, rotation=(math.pi / 2, 0, 0))
    ob = bpy.context.object
    ob.name = name
    ob.data.materials.append(material)
    return ob


def look_at(ob, target):
    ob.rotation_euler = (Vector(target) - ob.location).to_track_quat("-Z", "Y").to_euler()


def build():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for datablocks in (bpy.data.meshes, bpy.data.curves, bpy.data.materials, bpy.data.cameras, bpy.data.lights):
        # Keep the script repeatable in an already-open Blender file.
        pass

    dark = mat("matte black PLA", (0.035, 0.055, 0.058, 1), roughness=0.78,
               emission=(0.015, 0.022, 0.024, 1), strength=0.2)
    rock = mat("charcoal rock", (0.075, 0.105, 0.105, 1), roughness=0.9,
               emission=(0.025, 0.035, 0.034, 1), strength=0.25)
    white = mat("matte white lighthouse", WHITE, roughness=0.75,
                emission=(0.12, 0.11, 0.09, 1), strength=0.35)
    frame = mat("walnut shadow-box frame", (0.11, 0.075, 0.05, 1), roughness=0.52,
                emission=(0.025, 0.014, 0.008, 1), strength=0.2)
    sky = mat("animated sky display", SKY, roughness=0.35, emission=SKY, strength=0.35)
    ocean = mat("animated ocean display", OCEAN, roughness=0.28, emission=OCEAN, strength=0.5)
    warm = mat("lighthouse beacon", GLOW, roughness=0.2, emission=GLOW, strength=5.0)
    cloud = mat("cloud glow", (0.76, 0.83, 0.86, 1), roughness=0.7, emission=(0.45, 0.58, 0.65, 1), strength=0.35)

    # The physical display: sky and water are intentionally behind all relief.
    cube("display glass / sky", (0, 10.5, 62), (126.9, 3.5, 70.7), sky, bevel=2.5)
    cube("display ocean", (0, 8.5, 42), (122, 1.0, 27), ocean)
    disk("display moon", (-37, 7.2, 78), 7, warm, depth=0.4)
    for x, z, sx in [(-22, 86, 15), (4, 88, 11), (28, 81, 17)]:
        bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, location=(x, 6.8, z))
        ob = bpy.context.object
        ob.name = "display cloud"
        ob.scale = (sx / 2, 0.7, 3.0)
        bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
        ob.data.materials.append(cloud)

    # Dark base relief, in front of the screen.
    island = [(-71, 52), (-55, 47), (-31, 48), (-8, 44), (18, 46), (41, 48), (71, 53), (71, 62), (-71, 62)]
    prism("dark island silhouette", island, 2.0, 3.2, dark, bevel=0.8)
    foreground = [(-76, 33), (-63, 39), (-48, 35), (-37, 20), (-25, 15), (-8, 28), (7, 22), (18, 14), (36, 21), (52, 27), (67, 21), (76, 26), (76, 10), (-76, 10)]
    prism("raised foreground rocks", foreground, -2.0, 5.0, rock, bevel=1.0)
    surf = [(-72, 40), (-46, 38), (-22, 41), (-2, 37), (18, 40), (42, 37), (72, 39), (72, 42), (42, 40), (19, 43), (-3, 40), (-23, 44), (-47, 41), (-72, 43)]
    prism("raised surf edge", surf, -3.0, 4.0, rock, bevel=0.6)

    # White insert: lighthouse and cottages are deliberately separate from black relief.
    tower = [(-2.5, 51), (-2.0, 73), (2.0, 73), (2.5, 51)]
    prism("white lighthouse tower", tower, -5.0, 3.0, white, bevel=0.35)
    prism("white lighthouse keeper house", [(-8, 51), (-8, 59), (0, 66), (8, 59), (8, 51)], -5.0, 3.0, white, bevel=0.45)
    cube("lighthouse lantern room", (0, -6.0, 74), (7, 3.2, 2.8), white, bevel=0.5)
    cube("lighthouse beacon opening", (0, -7.8, 74), (3.0, 0.4, 1.3), warm, bevel=0.25)
    for x, z, s in [(30, 53, 8), (42, 51, 6)]:
        prism("white island cottage", [(x - s / 2, z), (x - s / 2, z + 7), (x, z + 12), (x + s / 2, z + 7), (x + s / 2, z)], -4.5, 2.5, white, bevel=0.35)

    # Frame and glass. The frame is intentionally oversized around the nominal 6x4 opening.
    cube("frame top", (0, -10, 116), (178, 18, 11), frame, bevel=2.5)
    cube("frame bottom", (0, -10, 8), (178, 18, 11), frame, bevel=2.5)
    cube("frame left", (-84, -10, 62), (11, 18, 105), frame, bevel=2.5)
    cube("frame right", (84, -10, 62), (11, 18, 105), frame, bevel=2.5)
    # The real object has a glass door. It is omitted from this beauty render
    # so the camera can show the relief and display clearly without opaque-glass
    # color-management artifacts. Add a transparent glass shader for final
    # optical studies if required.

    # Ground plane and lighting.
    ground = mat("ground", (0.055, 0.06, 0.055, 1), roughness=0.85)
    cube("ground", (0, 45, -3), (500, 500, 4), ground)
    bpy.ops.object.light_add(type="AREA", location=(-110, -180, 220))
    key = bpy.context.object
    key.name = "softbox key"
    key.data.energy = 650
    key.data.shape = 'DISK'
    key.data.size = 180
    look_at(key, (0, 0, 55))
    bpy.ops.object.light_add(type="AREA", location=(100, -80, 100))
    fill = bpy.context.object
    fill.data.energy = 260
    fill.data.size = 100
    look_at(fill, (0, 0, 55))

    # Camera.
    bpy.ops.object.camera_add(location=(0, -330, 69))
    cam = bpy.context.object
    cam.data.lens = 58
    cam.data.sensor_width = 36
    look_at(cam, (0, 0, 60))
    bpy.context.scene.camera = cam

    world = bpy.context.scene.world
    world.color = (0.008, 0.012, 0.014)
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.022, 0.028, 0.03, 1)
    world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.35

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE_NEXT"
    scene.render.resolution_x = 1000
    scene.render.resolution_y = 760
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.filepath = os.path.join(OUT, "luminary-concept.png")
    scene.render.film_transparent = False
    scene.view_settings.look = "AgX - Medium High Contrast"
    scene.render.image_settings.color_mode = "RGBA"
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, "luminary-concept.blend"))
    bpy.ops.render.render(write_still=True)


if __name__ == "__main__":
    build()
