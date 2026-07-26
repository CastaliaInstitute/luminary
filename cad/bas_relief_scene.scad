// Generic Luminary source-faithful bas-relief compiler.
// Supply a compiled scene directory created by scripts/compile-bas-relief.sh.

$fn = 48;
scene = "nubble-aligned";
part = "assembly"; // island, architecture (reserved), breaker, foreground, assembly
scene_w = 139.7; // 5.5 in visible opening
scene_h = 88.9;  // 3.5 in visible opening
// OpenSCAD imports SVG `pt` units as 0.3527778 mm. Preserve canvas positions
// by converting that unit explicitly rather than resizing each layer's bounds.
svg_pt_mm = 0.3527778;

module layer(name, depth) {
    translate([-scene_w / 2, -scene_h / 2, 0])
        scale([scene_w / (1024 * svg_pt_mm), scene_h / (600 * svg_pt_mm), 1])
            linear_extrude(height = depth)
                import(str("../scenes/", scene, "/compiled/", name, ".svg"));
}

// Image-space masks are mounted inside a 5.5 x 3.5 in visible field. These
// depths provide parallax without turning the scene into a diorama.
module island() { layer("island", 1.6); }
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
module breaker() { translate([0,0,1.6]) layer("breaker", 1.6); }
module foreground() { translate([0,0,3.2]) layer("foreground", 1.8); }

if (part == "island") island();
if (part == "architecture") architecture();
if (part == "breaker") breaker();
if (part == "foreground") foreground();
if (part == "assembly") { island(); breaker(); foreground(); }
