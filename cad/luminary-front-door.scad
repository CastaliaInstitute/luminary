// Luminary hinged front door reference model.
// Wood components; dimensions in millimetres. This is a woodworking assembly
// drawing, not a recommended one-piece FDM print.

$fn = 32;
part = "assembly"; // door, hinges, glass, assembly

door_w = 177.8;     // 7 in outer width
door_h = 127.0;     // 5 in outer height
door_d = 25.4;      // 1 in depth
window_w = 139.7;   // 5.5 in clear opening
window_h = 88.9;    // 3.5 in clear opening
rail = (door_w - window_w) / 2; // 19.05 mm / 0.75 in
glass_t = 2.0;

module door() {
    // Boolean ring guarantees one watertight reference solid while preserving
    // the four 0.75 in wood rail dimensions for the cut list.
    difference() {
        translate([-door_w/2, -door_h/2, 0]) cube([door_w, door_h, door_d]);
        translate([-window_w/2, -window_h/2, -0.01]) cube([window_w, window_h, door_d + 0.02]);
    }
}

module glass() {
    // Glass is seated at the front surface of the closed door.
    translate([-window_w/2, -window_h/2, -glass_t]) cube([window_w, window_h, glass_t]);
}

module hinge(x) {
    // Two-leaf butt hinge: one leaf on the door and one on the case rail.
    translate([x - 9, -door_h/2 - 1, 3]) cube([18, 12, 1.2]);
    translate([x - 9, -door_h/2 - 13, 3]) cube([18, 12, 1.2]);
    translate([x, -door_h/2 - 1, 3]) rotate([0, 90, 0]) cylinder(d = 4, h = 18, center = true);
}

if (part == "door") door();
if (part == "glass") glass();
if (part == "hinges") { hinge(-52); hinge(52); }
if (part == "assembly") { door(); glass(); hinge(-52); hinge(52); }
