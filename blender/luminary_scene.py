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


def distressed_white_frame():
    """Warm white paint rubbed through to brown wood grain."""
    m = mat("distressed white painted wood", (0.78, 0.75, 0.67, 1), roughness=0.72,
            emission=(0.035, 0.03, 0.022, 1), strength=0.12)
    nodes = m.node_tree.nodes
    links = m.node_tree.links
    bsdf = nodes.get("Principled BSDF")
    noise = nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 5.5
    noise.inputs["Detail"].default_value = 8.0
    noise.inputs["Roughness"].default_value = 0.78
    ramp = nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.24
    ramp.color_ramp.elements[0].color = (0.16, 0.065, 0.025, 1)
    ramp.color_ramp.elements[1].position = 0.70
    ramp.color_ramp.elements[1].color = (0.98, 0.93, 0.81, 1)
    worn = ramp.color_ramp.elements.new(0.39)
    worn.color = (0.30, 0.13, 0.055, 1)
    paint = ramp.color_ramp.elements.new(0.52)
    paint.color = (0.88, 0.82, 0.69, 1)
    links.new(noise.outputs["Fac"], ramp.inputs["Fac"])
    links.new(ramp.outputs["Color"], bsdf.inputs["Base Color"])
    bump = nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.30
    bump.inputs["Distance"].default_value = 0.22
    links.new(noise.outputs["Fac"], bump.inputs["Height"])
    links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    bsdf.inputs["Emission Color"].default_value = (0.42, 0.39, 0.32, 1)
    bsdf.inputs["Emission Strength"].default_value = 0.45
    return m


def textured_rock_material():
    m = mat("charcoal rock with grain", (0.055, 0.07, 0.068, 1), roughness=0.92,
            emission=(0.012, 0.016, 0.015, 1), strength=0.16)
    nodes = m.node_tree.nodes
    links = m.node_tree.links
    bsdf = nodes.get("Principled BSDF")
    noise = nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 13.0
    noise.inputs["Detail"].default_value = 7.0
    noise.inputs["Roughness"].default_value = 0.82
    bump = nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.35
    bump.inputs["Distance"].default_value = 0.32
    links.new(noise.outputs["Fac"], bump.inputs["Height"])
    links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
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
    rock = textured_rock_material()
    white = mat("matte white lighthouse", WHITE, roughness=0.75,
                emission=(0.12, 0.11, 0.09, 1), strength=0.35)
    frame = distressed_white_frame()
    sky = mat("animated sky display", SKY, roughness=0.35, emission=SKY, strength=0.35)
    ocean = mat("animated ocean display", OCEAN, roughness=0.28, emission=OCEAN, strength=0.5)
    warm = mat("lighthouse beacon", GLOW, roughness=0.2, emission=GLOW, strength=5.0)
    cloud = mat("cloud glow", (0.76, 0.83, 0.86, 1), roughness=0.7, emission=(0.45, 0.58, 0.65, 1), strength=0.35)

    # The physical display: sky and water are intentionally behind all relief.
    # The physical silhouette is 44.45 mm in front of the display.
    display_y = 46.5
    cube("display glass / sky", (0, display_y, 62), (126.9, 3.5, 70.7), sky, bevel=2.5)
    cube("display ocean", (0, display_y - 2.0, 42), (122, 1.0, 27), ocean)
    # Rear mechanical architecture: a full 5x7 printed plate with a shallow
    # 4x6 registration land behind the P4/display stack.
    cube("printed 5x7 rear plate", (0, display_y + 22, 62), (177.8, 5.0, 127.0), dark, bevel=3.0)
    cube("shallow 4x6 rear registration land", (0, display_y + 18.5, 62), (152.4, 1.0, 101.6), rock, bevel=1.0)
    disk("display moon", (-37, display_y - 3.3, 78), 7, warm, depth=0.4)
    for x, z, sx in [(-22, 86, 15), (4, 88, 11), (28, 81, 17)]:
        bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, location=(x, display_y - 3.7, z))
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

    # White insert: lighthouse and cottages are deliberately separate from black relief.
    tower = [(-2.5, 51), (-2.0, 73), (2.0, 73), (2.5, 51)]
    prism("white lighthouse tower", tower, -5.0, 3.0, white, bevel=0.35)
    prism("white lighthouse keeper house", [(-8, 51), (-8, 59), (0, 66), (8, 59), (8, 51)], -5.0, 3.0, white, bevel=0.45)
    cube("lighthouse lantern room", (0, -6.0, 74), (7, 3.2, 2.8), white, bevel=0.5)
    cube("lighthouse beacon opening", (0, -7.8, 74), (3.0, 0.4, 1.3), warm, bevel=0.25)
    for x, z, s in [(30, 53, 8), (42, 51, 6)]:
        prism("white island cottage", [(x - s / 2, z), (x - s / 2, z + 7), (x, z + 12), (x + s / 2, z + 7), (x + s / 2, z)], -4.5, 2.5, white, bevel=0.35)

    # Frame and glass. The frame is intentionally oversized around the nominal 6x4 opening.
    cube("frame top", (0, -10, 116), (178, 28, 11), frame, bevel=2.5)
    cube("frame bottom", (0, -10, 8), (178, 28, 11), frame, bevel=2.5)
    cube("frame left", (-84, -10, 62), (11, 28, 105), frame, bevel=2.5)
    cube("frame right", (84, -10, 62), (11, 28, 105), frame, bevel=2.5)
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
    key.data.energy = 1050
    key.data.shape = 'DISK'
    key.data.size = 180
    look_at(key, (0, 0, 55))
    bpy.ops.object.light_add(type="AREA", location=(100, -80, 100))
    fill = bpy.context.object
    fill.data.energy = 420
    fill.data.size = 100
    look_at(fill, (0, 0, 55))

    # Blue screen spill across the relief, plus a warm local lighthouse glow.
    bpy.ops.object.light_add(type="AREA", location=(0, -45, 72))
    screen_spill = bpy.context.object
    screen_spill.name = "cool display spill"
    screen_spill.data.energy = 240
    screen_spill.data.color = (0.12, 0.38, 0.55)
    screen_spill.data.size = 110
    look_at(screen_spill, (0, 0, 52))
    bpy.ops.object.light_add(type="POINT", location=(0, -24, 74))
    beacon = bpy.context.object
    beacon.name = "warm lighthouse beacon spill"
    beacon.data.energy = 55
    beacon.data.color = (1.0, 0.34, 0.08)
    beacon.data.shadow_soft_size = 12

    # Camera.
    # Slight three-quarter angle to reveal frame depth and layered relief.
    # Strong enough perspective to expose the 37 mm front-to-back stack.
    bpy.ops.object.camera_add(location=(260, -270, 125))
    cam = bpy.context.object
    cam.data.lens = 62
    cam.data.sensor_width = 36
    look_at(cam, (0, 0, 60))
    bpy.context.scene.camera = cam

    world = bpy.context.scene.world
    world.color = (0.008, 0.012, 0.014)
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.022, 0.028, 0.03, 1)
    world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.35

    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 16
    scene.cycles.use_denoising = True
    scene.render.resolution_x = 800
    scene.render.resolution_y = 600
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
