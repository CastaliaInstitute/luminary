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
DISPLAY_BACKGROUND = os.path.join(ROOT, "assets", "display-sky-sea-1280x720.png")
STL_DIR = os.path.join(ROOT, "renders", "stl")
SILHOUETTE_CARRIER_STL = os.path.join(STL_DIR, "luminary-silhouette-carrier.stl")
SILHOUETTE_STL = os.path.join(STL_DIR, "luminary-silhouette.stl")
STRUCTURES_STL = os.path.join(STL_DIR, "luminary-structures.stl")
SCENE_VARIANT = os.environ.get("LUMINARY_SCENE", "luminary")

# Nubble uses the three user-approved AI-separated masks as distinct opaque
# parts.  The generated LCD image remains the sky-and-sea background.
if SCENE_VARIANT == "nubble":
    DISPLAY_BACKGROUND = os.path.join(ROOT, "assets", "display-nubble-1280x720.png")
    PRINTED_PARTS = [
        ("hidden magnetic frame", os.path.join(STL_DIR, "nubble-magnetic-frame.stl"), "carrier"),
        ("Nubble island layer", os.path.join(STL_DIR, "nubble-island-layer.stl"), "dark"),
        ("Nubble breaker-rock layer", os.path.join(STL_DIR, "nubble-breaker-layer.stl"), "rock"),
        ("Nubble foreground-rock layer", os.path.join(STL_DIR, "nubble-foreground-layer.stl"), "dark"),
    ]
else:
    PRINTED_PARTS = [
        ("clear PETG silhouette carrier", SILHOUETTE_CARRIER_STL, "carrier"),
        ("black printed silhouette relief", SILHOUETTE_STL, "dark"),
        ("white printed lighthouse insert", STRUCTURES_STL, "white"),
    ]
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


def display_image_material(image_path):
    """LCD-like emissive material using the generated land-free backdrop."""
    m = bpy.data.materials.new("generated sky and sea LCD image")
    m.use_nodes = True
    nodes = m.node_tree.nodes
    links = m.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Strength"].default_value = 1.15
    image = nodes.new("ShaderNodeTexImage")
    image.image = bpy.data.images.load(image_path, check_existing=True)
    image.interpolation = "Linear"
    links.new(image.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return m


def clear_petg_material():
    """Nearly invisible clear-filament carrier; keep only a trace of reflection."""
    m = bpy.data.materials.new("clear PETG carrier")
    m.use_nodes = True
    nodes = m.node_tree.nodes
    links = m.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    transparent = nodes.new("ShaderNodeBsdfTransparent")
    links.new(transparent.outputs["BSDF"], output.inputs["Surface"])
    return m


def clear_glass_material():
    """Render-only glass for the hinged shadow-box door."""
    m = bpy.data.materials.new("clear glass door pane")
    m.use_nodes = True
    nodes = m.node_tree.nodes
    links = m.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    transparent = nodes.new("ShaderNodeBsdfTransparent")
    links.new(transparent.outputs["BSDF"], output.inputs["Surface"])
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


def display_image_plane(name, loc, width, height, material):
    # Plane is oriented toward the camera on the front (-Y) face of the LCD.
    bpy.ops.mesh.primitive_plane_add(size=2, location=loc, rotation=(math.pi / 2, 0, 0))
    ob = bpy.context.object
    ob.name = name
    ob.scale = (width / 2, height / 2, 1)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    ob.data.materials.append(material)
    return ob


def import_print_stl(name, filepath, material, location):
    """Import the actual OpenSCAD printable part, not a render proxy."""
    bpy.ops.object.select_all(action="DESELECT")
    bpy.ops.wm.stl_import(filepath=filepath)
    imported = list(bpy.context.selected_objects)
    for ob in imported:
        ob.name = name
        # OpenSCAD's XY scene plane becomes Blender's XZ front plane. Its Z
        # extrusion becomes depth, matching the physical front-to-back stack.
        ob.rotation_euler = (math.pi / 2, 0, 0)
        ob.location = location
        ob.data.materials.clear()
        ob.data.materials.append(material)
    return imported


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
    white = mat("matte white lighthouse", (1.0, 0.99, 0.94, 1), roughness=0.52,
                emission=(0.42, 0.40, 0.34, 1), strength=1.0)
    frame = distressed_white_frame()
    wood_mat = mat("warm wood mat", (0.48, 0.26, 0.12, 1), roughness=0.62,
                   emission=(0.10, 0.045, 0.015, 1), strength=0.32)
    glass = clear_glass_material()
    hinge = mat("aged hinge metal", (0.14, 0.105, 0.065, 1), metallic=0.65, roughness=0.38)
    lcd_body = mat("LCD black bezel", (0.008, 0.012, 0.014, 1), roughness=0.30)
    display_image = display_image_material(DISPLAY_BACKGROUND)
    clear_carrier = clear_petg_material()

    # The physical display: sky and water are intentionally behind all relief.
    # Fixed 2 in box depth, with a 20 mm silhouette-to-screen allocation.
    # Keep the LCD close to the relief: at the intended three-quarter viewing
    # angle, a larger gap makes the source-registered masks visibly drift from
    # their sky, horizon, and breaking-wave landmarks.
    display_y = 14.5
    cube("LCD body and bezel", (0, display_y, 62), (126.9, 3.5, 70.7), lcd_body, bevel=2.5)
    display_image_plane("generated sky and sea on display", (0, display_y - 1.78, 62),
                        110.32, 62.28, display_image)
    # Rear mechanical architecture: a full 5x7 printed plate with a shallow
    # 4x6 registration land behind the P4/display stack.
    cube("printed 5x7 rear plate", (0, 49.3, 62), (177.8, 3.0, 127.0), dark, bevel=3.0)
    cube("shallow 4x6 rear registration land", (0, 46.8, 62), (152.4, 1.0, 101.6), rock, bevel=1.0)
    # Import the exact printable meshes, never render-only silhouette proxies.
    materials = {"carrier": clear_carrier, "dark": dark, "rock": rock, "white": white}
    for name, filepath, material in PRINTED_PARTS:
        import_print_stl(name, filepath, materials[material], (0, 5.0, 62))

    # Frame and glass. The frame is intentionally oversized around the nominal 6x4 opening.
    cube("frame top", (0, 25.4, 116), (178, 50.8, 11), frame, bevel=2.5)
    cube("frame bottom", (0, 25.4, 8), (178, 50.8, 11), frame, bevel=2.5)
    cube("frame left", (-84, 25.4, 62), (11, 50.8, 105), frame, bevel=2.5)
    cube("frame right", (84, 25.4, 62), (11, 50.8, 105), frame, bevel=2.5)

    # Hinged front door: shallow distressed-wood frame and clear glass. It sits
    # proud of the 2 in case and is intentionally separate from printed parts.
    door_y = -3.0
    cube("door top rail", (0, door_y, 119), (170, 5.0, 8), frame, bevel=1.6)
    cube("door bottom rail", (0, door_y, 5), (170, 5.0, 8), frame, bevel=1.6)
    cube("door left rail", (-81, door_y, 62), (8, 5.0, 114), frame, bevel=1.6)
    cube("door right rail", (81, door_y, 62), (8, 5.0, 114), frame, bevel=1.6)
    # The actual glass is 4.5 × 3.5 in. A warm wood mat hides the larger 4 × 6
    # silhouette carrier and defines the true visible aperture.
    mat_y = door_y - 2.7
    cube("wood mat top", (0, mat_y, 109.45), (152.4, 1.6, 6.35), wood_mat)
    cube("wood mat bottom", (0, mat_y, 14.55), (152.4, 1.6, 6.35), wood_mat)
    cube("wood mat left", (-66.675, mat_y, 62), (19.05, 1.6, 88.9), wood_mat)
    cube("wood mat right", (66.675, mat_y, 62), (19.05, 1.6, 88.9), wood_mat)
    cube("door glass", (0, door_y - 3.6, 62), (114.3, 0.6, 88.9), glass)
    for z in (31, 93):
        bpy.ops.mesh.primitive_cylinder_add(vertices=24, radius=2.2, depth=11,
                                            location=(-85, door_y - 2.6, z))
        h = bpy.context.object
        h.name = "door hinge"
        h.rotation_euler = (math.pi / 2, 0, 0)
        h.data.materials.append(hinge)

    # Ground plane and lighting.
    ground = mat("ground", (0.055, 0.06, 0.055, 1), roughness=0.85)
    cube("ground", (0, 45, -3), (500, 500, 4), ground)
    bpy.ops.object.light_add(type="AREA", location=(-100, -260, 220))
    key = bpy.context.object
    key.name = "softbox key"
    key.data.energy = 1450
    key.data.shape = 'DISK'
    key.data.size = 180
    look_at(key, (0, 0, 55))
    bpy.ops.object.light_add(type="AREA", location=(110, -180, 120))
    fill = bpy.context.object
    fill.data.energy = 700
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
    # Strong enough perspective to expose the 35 mm relief-to-display stack.
    bpy.ops.object.camera_add(location=(120, -440, 102))
    cam = bpy.context.object
    cam.data.lens = 68
    cam.data.sensor_width = 36
    look_at(cam, (0, 0, 60))
    bpy.context.scene.camera = cam

    world = bpy.context.scene.world
    world.color = (0.008, 0.012, 0.014)
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.022, 0.028, 0.03, 1)
    world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.55

    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    # A fast preview can be requested with LUMINARY_SAMPLES=4; final renders
    # retain the 16-sample Cycles setting by default.
    scene.cycles.samples = int(os.environ.get("LUMINARY_SAMPLES", "16"))
    scene.cycles.use_denoising = True
    scene.render.resolution_x = 800
    scene.render.resolution_y = 600
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    output_name = "nubble-concept" if SCENE_VARIANT == "nubble" else "luminary-concept"
    scene.render.filepath = os.path.join(OUT, output_name + ".png")
    scene.render.film_transparent = False
    scene.view_settings.look = "AgX - Medium High Contrast"
    scene.render.image_settings.color_mode = "RGBA"
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, output_name + ".blend"))
    bpy.ops.render.render(write_still=True)


if __name__ == "__main__":
    build()
