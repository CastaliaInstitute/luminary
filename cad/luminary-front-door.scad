// Luminary hinged front door reference model.
// Wood components; dimensions in millimetres. This is a woodworking assembly
// drawing, not a recommended one-piece FDM print.

$fn = 32;
part = "assembly"; // door, sight_mat, hinges, glass, assembly

door_w = 177.8;     // 7 in outer width
door_h = 127.0;     // 5 in outer height
door_d = 25.4;      // 1 in depth
window_w = 139.7;   // 5.5 in clear opening
window_h = 88.9;    // 3.5 in clear opening
rail = (door_w - window_w) / 2; // 19.05 mm / 0.75 in
glass_t = 2.0;
// A thin painted-wood sight mat sits immediately behind the glass.  It keeps
// the documented 5.5 x 3.5 in glass pane but reduces the visible LCD aperture
// enough to hide the P4's bezel and active-area edge.
sight_mat_t = 3.2;
sight_window_w = 133.35; // 5.25 in visible aperture
sight_window_h = 82.55;  // 3.25 in visible aperture
contact_frame_w = 152.4; // 6 in, black display-contact frame
contact_frame_h = 101.6; // 4 in
contact_rebate_clearance = 0.30;
contact_rebate_t = 1.0;

module door() {
    // Boolean ring guarantees one watertight reference solid while preserving
    // the four 0.75 in wood rail dimensions for the cut list.
    difference() {
        translate([-door_w/2, -door_h/2, 0]) cube([door_w, door_h, door_d]);
        translate([-window_w/2, -window_h/2, -0.01]) cube([window_w, window_h, door_d + 0.02]);
        // Rear rabbet captures the 0.8 mm black contact frame flush with the
        // door inset. It is a mechanical seat, not a magnet feature.
        translate([-(contact_frame_w + contact_rebate_clearance)/2,
                   -(contact_frame_h + contact_rebate_clearance)/2,
                   door_d - contact_rebate_t])
            cube([contact_frame_w + contact_rebate_clearance,
                  contact_frame_h + contact_rebate_clearance,
                  contact_rebate_t + 0.01]);
    }
}

module glass() {
    // Glass is seated at the front surface of the closed door.
    translate([-window_w/2, -window_h/2, -glass_t]) cube([window_w, window_h, glass_t]);
}

module sight_mat() {
    difference() {
        // Flush to the inside face of the glass; made from the same
        // distressed-white wood as the front door.
        translate([-window_w/2, -window_h/2, 0])
            cube([window_w, window_h, sight_mat_t]);
        translate([-sight_window_w/2, -sight_window_h/2, -0.01])
            cube([sight_window_w, sight_window_h, sight_mat_t + 0.02]);
    }
}

module contact_rebate_gauge() {
    // Inspection-only volume for the 4 x 6 in contact-frame seat.
    translate([-contact_frame_w/2, -contact_frame_h/2, door_d - contact_rebate_t])
        cube([contact_frame_w, contact_frame_h, contact_rebate_t]);
}

module hinge(x) {
    // Two-leaf butt hinge: one leaf on the door and one on the case rail.
    translate([x - 9, -door_h/2 - 1, 3]) cube([18, 12, 1.2]);
    translate([x - 9, -door_h/2 - 13, 3]) cube([18, 12, 1.2]);
    translate([x, -door_h/2 - 1, 3]) rotate([0, 90, 0]) cylinder(d = 4, h = 18, center = true);
}

if (part == "door") door();
if (part == "sight_mat") sight_mat();
if (part == "glass") glass();
if (part == "contact_rebate_gauge") contact_rebate_gauge();
if (part == "hinges") { hinge(-52); hinge(52); }
if (part == "assembly") { door(); sight_mat(); glass(); hinge(-52); hinge(52); }
