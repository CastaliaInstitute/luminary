// Generic Luminary source-faithful bas-relief compiler.
// Supply a compiled scene directory created by scripts/compile-bas-relief.sh.

$fn = 48;
scene = "nubble-aligned";
part = "assembly"; // island, island_texture, architecture, breaker, breaker_texture, foreground, foreground_texture, assembly
scene_w = 139.7; // 5.5 in visible opening
scene_h = 88.9;  // 3.5 in visible opening
// OpenSCAD imports SVG `pt` units as 0.3527778 mm. Preserve canvas positions
// by converting that unit explicitly rather than resizing each layer's bounds.
svg_pt_mm = 0.3527778;
texture_depth = 0.75; // mm, source-derived rock detail above each base layer
texture_enabled = true; // set false for fast silhouette-only validation exports

module layer(name, depth) {
    translate([-scene_w / 2, -scene_h / 2, 0])
        scale([scene_w / (1024 * svg_pt_mm), scene_h / (600 * svg_pt_mm), 1])
            linear_extrude(height = depth)
                import(str("../scenes/", scene, "/compiled/", name, ".svg"));
}

// The compiler turns the masked photograph into three nested vector texture
// bands. This retains printable rock character without the huge, fragile STL
// created by a per-pixel terrain mesh.
module rock_texture(name, z, depth = texture_depth) {
    for (band = [[45, depth * 0.34], [65, depth * 0.67], [82, depth]])
        translate([-scene_w / 2, -scene_h / 2, z])
            scale([scene_w / (700 * svg_pt_mm), scene_h / (444 * svg_pt_mm), 1])
                linear_extrude(height = band[1])
                    import(str("../scenes/", scene, "/compiled/", name,
                               "-texture-", band[0], ".svg"));
}

module textured_layer(name, z, depth) {
    translate([0, 0, z]) layer(name, depth);
    if (texture_enabled) rock_texture(name, z + depth);
}

// Image-space masks are mounted inside a 5.5 x 3.5 in visible field. These
// depths provide parallax without turning the scene into a diorama.
module island() { textured_layer("island", 0, 1.6); }
// White architectural insert derived from the Nubble lighthouse tracing. The
// lantern centre remains open so the LCD supplies the transparent glazing.
module architecture() {
    // Kept deliberately smaller than the island mass: it overlays the traced
    // building footprints rather than becoming a freestanding 3D lighthouse.
    translate([-39.9, -17.5, 1.65])
        scale([0.45 * scene_w / 142.4, 0.45 * scene_h / 91.6, 1])
            linear_extrude(height = 0.8)
                import("../assets/living-landscape-structures.svg");
}
module breaker() { textured_layer("breaker", 1.6, 1.6); }
module foreground() { textured_layer("foreground", 3.2, 1.8); }
module island_texture() { rock_texture("island", 1.6); }
module breaker_texture() { rock_texture("breaker", 3.2); }
module foreground_texture() { rock_texture("foreground", 5.0); }

if (part == "island") island();
if (part == "island_texture") island_texture();
if (part == "architecture") architecture();
if (part == "breaker") breaker();
if (part == "breaker_texture") breaker_texture();
if (part == "foreground") foreground();
if (part == "foreground_texture") foreground_texture();
if (part == "assembly") { island(); breaker(); foreground(); }
