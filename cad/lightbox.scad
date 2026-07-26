// Luminary / Living Landscape shadow-box model
// Units: millimetres. Reference: docs/reference-notes.md
//
// Render one part at a time by changing `part` below:
//   "assembly", "preview", "silhouette", "silhouette_carrier", "structures", "glass_hardware", "backplate", "carrier", "display", "pcb"

$fn = 64;

part = "assembly";

// Shadow-box envelope
frame_outer_w = 177.8; // 7 in
frame_outer_h = 127.0; // 5 in
shadow_box_depth = 50.8; // 2 in overall depth
panel_w = 152.4; // 6 in nominal interior
panel_h = 101.6; // 4 in nominal interior

// Waveshare ESP32-P4-WiFi6-Touch-LCD-5 reference dimensions
display_w = 126.90;
display_h = 70.70;
active_w = 110.32;
active_h = 62.28;
pcb_w = 118.50;
pcb_h = 64.50;

// Printed rear plate / mounting architecture
backplate_t = 3.0;
insert_t = 1.0; // shallow registration land; keeps the display close to the frame back
insert_border = 5.0;
rear_wall = 2.5;
rear_magnet_d = 3.2;
rear_magnet_h = 1.2;
rear_magnet_edge = 9.0;
usb_side = "left"; // "left" or "right"
usb_cable_cutout_d = 8.0; // small circular cable pass-through
usb_cutout_y = 0.0;
usb_edge_clearance = 3.0;
usb_bottom_edge_clearance = 3.0;

// Printed silhouette panel
silhouette_t = 0.80;
silhouette_carrier_t = 0.40; // two 0.20 mm clear-filament layers behind all art
glass_rabbet_margin = 8.0; // hidden by the door's glass-mounting margin
panel_border = glass_rabbet_margin;
tapered_edge_w = 2.0; // final portion of mount frame feathered toward clear window
scene_art = true;
nozzle_d = 0.20;
scene_base_layers = 4;       // 0.80 mm: visible face
scene_mid_layers = 6;        // 1.20 mm: white architecture and middle shoreline
scene_foreground_layers = 8; // 1.60 mm: dark foreground rocks
rib_t = 2.0;
rib_h = 6.0;
magnet_d = 3.0;
magnet_h = 1.0;
magnet_edge = 4.0; // centers each 8 mm pod within the hidden perimeter
magnet_pod_w = 8.0;
magnet_pod_t = 2.0; // hidden depth; gives the 1 mm magnet a real pocket
svg_field_w = 142.4;
svg_field_h = 91.6;

// Glass-door attachment hardware
steel_square_w = 6.35; // 1/4 in
steel_square_t = 1.0;
steel_square_edge = 4.0;
show_glass_hardware = true;

// Carrier and stack-up assumptions; verify against the actual frame.
carrier_t = 3.0;
carrier_clearance = 1.5;
// 35 mm leaves an ~15 mm rear electronics envelope inside a 2 in box.
display_gap = 35.0;
pcb_standoff_h = 2.5;

module rounded_box(size, radius = 3) {
    hull() {
        for (x = [radius, size[0] - radius])
            for (y = [radius, size[1] - radius])
                translate([x, y, 0]) cylinder(r = radius, h = size[2]);
    }
}

module magnet_pocket(x, y) {
    // Pocket opens on the rear of the hidden corner pod. A 0.8 mm face remains
    // between magnet and glass, while the magnet stays invisible from the front.
    translate([x, y, magnet_pod_t - magnet_h - 0.01])
        cylinder(d = magnet_d + 0.20, h = magnet_h + 0.02);
}

module corner_magnet_pod(x, y) {
    difference() {
        // Overlap the carrier by 0.10 mm; coplanar contact is not a printable union.
        translate([x - magnet_pod_w / 2, y - magnet_pod_w / 2, silhouette_carrier_t - 0.10])
            cube([magnet_pod_w, magnet_pod_w, magnet_pod_t - silhouette_carrier_t + 0.10]);
        magnet_pocket(x, y);
    }
}

module frame_ring_2d(outer_w, outer_h, inner_w, inner_h) {
    difference() {
        square([outer_w, outer_h], center = true);
        square([inner_w, inner_h], center = true);
    }
}

module tapered_mount_frame(height = silhouette_t) {
    // Rigid outer portion: hidden beneath the door rabbet and strong enough to
    // carry the corner pods.
    linear_extrude(height = height)
        frame_ring_2d(panel_w, panel_h,
                      panel_w - 2 * (panel_border - tapered_edge_w),
                      panel_h - 2 * (panel_border - tapered_edge_w));

    // The final 2 mm toward the clear window feathers away instead of creating
    // a hard visible frame edge.
    linear_extrude(height = height, scale = 0.94)
        frame_ring_2d(panel_w - 2 * (panel_border - tapered_edge_w),
                      panel_h - 2 * (panel_border - tapered_edge_w),
                      panel_w - 2 * panel_border,
                      panel_h - 2 * panel_border);
}

module glass_steel_square(x, y) {
    // Square is bonded to the inside face of the glass; the magnet is retained
    // in the silhouette panel directly behind it.
    translate([x, y, -steel_square_t])
        cube([steel_square_w, steel_square_w, steel_square_t], center = true);
}

module glass_hardware() {
    for (x = [-panel_w / 2 + steel_square_edge, panel_w / 2 - steel_square_edge])
        for (y = [-panel_h / 2 + steel_square_edge, panel_h / 2 - steel_square_edge])
            glass_steel_square(x, y);
}

module transparent_silhouette_carrier() {
    // Clear carrier is deliberately only a hidden rabbet ring—not a full 4x6
    // sheet. The photograph-derived island reaches the side margins itself.
    union() {
        // A simple closed ring is more reliable than the former coplanar taper.
        linear_extrude(height = silhouette_carrier_t)
            frame_ring_2d(panel_w, panel_h,
                          panel_w - 2 * panel_border,
                          panel_h - 2 * panel_border);

        // Two hairline supports retain otherwise isolated foreground boulders.
        // They terminate under dark rock and remain visually absent over water.
        for (x = [-10, 34])
            linear_extrude(height = silhouette_carrier_t)
                hull() {
                    translate([x, -panel_h / 2 + panel_border + 0.2]) circle(d = 0.6);
                translate([x, -23]) circle(d = 0.6);
            }

        // Island no longer stretches across the opening. These two short clear
        // ties terminate behind its left/right rock shelf and disappear into
        // the rabbet ring.
        for (side = [-1, 1])
            linear_extrude(height = silhouette_carrier_t)
                hull() {
                    translate([side * (panel_w / 2 - panel_border - 0.2), 7]) circle(d = 0.6);
                    translate([side * 59.7, 7]) circle(d = 0.6);
                }

        // Four hidden rear pods carry the magnets that couple to the steel squares.
        for (x = [-panel_w / 2 + magnet_edge, panel_w / 2 - magnet_edge])
            for (y = [-panel_h / 2 + magnet_edge, panel_h / 2 - magnet_edge])
                corner_magnet_pod(x, y);
    }
}

module dark_silhouette_panel() {
    // Source-traced island and foreground boulders, bonded to the transparent
    // carrier rather than forced to grow visible side bridges.
    if (scene_art)
        translate([-panel_w / 2 + panel_border, -panel_h / 2 + panel_border, silhouette_carrier_t])
            scale([(panel_w - 2 * panel_border) / svg_field_w,
                   (panel_h - 2 * panel_border) / svg_field_h, 1])
                linear_extrude(height = nozzle_d * scene_base_layers)
                    import("../assets/living-landscape-photo-trace.svg");

}

module white_structures() {
    // Print separately in white. The two narrow lantern mullions in the SVG
    // leave its centre open; that transparent window is lit by the display.
    translate([-panel_w / 2 + panel_border, -panel_h / 2 + panel_border, silhouette_carrier_t])
        scale([(panel_w - 2 * panel_border) / svg_field_w,
               (panel_h - 2 * panel_border) / svg_field_h, 1])
            linear_extrude(height = nozzle_d * scene_mid_layers)
                import("../assets/living-landscape-structures.svg");
}

module rear_magnet_pocket(x, y) {
    // Pockets open on the rear face toward the steel plates in the frame.
    translate([x, y, -0.01])
        cylinder(d = rear_magnet_d + 0.25, h = rear_magnet_h + 0.02);
}

module p4_mount_standoff(x, y) {
    translate([x, y, backplate_t + insert_t]) {
        difference() {
            cylinder(d = 7.0, h = 6.0);
            translate([0, 0, -0.01]) cylinder(d = 3.2, h = 6.02);
        }
    }
}

module rear_backplate() {
    usb_side_cutout_x = usb_side == "left"
        ? -frame_outer_w / 2 + usb_cable_cutout_d / 2 + usb_edge_clearance
        : frame_outer_w / 2 - usb_cable_cutout_d / 2 - usb_edge_clearance;
    usb_bottom_cutout_y = -frame_outer_h / 2 + usb_cable_cutout_d / 2 + usb_bottom_edge_clearance;
    difference() {
        // Full 5x7 plate, aligned with the shadow-box exterior.
        translate([-frame_outer_w / 2, -frame_outer_h / 2, 0])
            cube([frame_outer_w, frame_outer_h, backplate_t]);

        for (x = [-frame_outer_w / 2 + rear_magnet_edge, frame_outer_w / 2 - rear_magnet_edge])
            for (y = [-frame_outer_h / 2 + rear_magnet_edge, frame_outer_h / 2 - rear_magnet_edge])
                rear_magnet_pocket(x, y);

        // Small circular cable access opening. The USB-C connector remains
        // internal; only the cable exits through the printed back.
        translate([usb_side_cutout_x, usb_cutout_y, backplate_t / 2])
            cylinder(d = usb_cable_cutout_d, h = backplate_t + 0.2, center = true);

        // Bottom-center access for tabletop placement and downward cable routing.
        translate([0, usb_bottom_cutout_y, backplate_t / 2])
            cylinder(d = usb_cable_cutout_d, h = backplate_t + 0.2, center = true);
    }

    // Raised 4x6 insert: the P4/display carrier mounts above this boss.
    translate([-panel_w / 2, -panel_h / 2, backplate_t])
        rounded_box([panel_w, panel_h, insert_t], 3);

    // Four PCB mounting points. Coordinates are based on the photo's corner-hole
    // callouts and remain adjustable until the actual board is measured.
    for (x = [-pcb_w / 2 + 3.75, pcb_w / 2 - 3.75])
        for (y = [-pcb_h / 2 + 3.75, pcb_h / 2 - 3.75])
            p4_mount_standoff(x, y);

}

module display_model() {
    color("#202124")
        rounded_box([display_w, display_h, 4.0], 3);
    color("#101820")
        translate([(display_w - active_w) / 2, (display_h - active_h) / 2, 4.0])
            cube([active_w, active_h, 0.2]);
}

module pcb_model() {
    color("#1267a8")
        translate([(display_w - pcb_w) / 2, (display_h - pcb_h) / 2, 0])
            cube([pcb_w, pcb_h, 1.6]);
}

module carrier() {
    // Carrier is centered on the display and leaves a small perimeter margin.
    carrier_w = display_w + 2 * carrier_clearance;
    carrier_h = display_h + 2 * carrier_clearance;
    difference() {
        translate([-carrier_w / 2, -carrier_h / 2, 0])
            rounded_box([carrier_w, carrier_h, carrier_t], 3);
        translate([-display_w / 2, -display_h / 2, -0.01])
            cube([display_w, display_h, carrier_t + 0.02]);
    }

    // Four rear standoffs for the PCB mounting plane.
    for (x = [-pcb_w / 2 + 3.75, pcb_w / 2 - 3.75])
        for (y = [-pcb_h / 2 + 3.75, pcb_h / 2 - 3.75])
            translate([x, y, carrier_t])
                cylinder(d = 6, h = pcb_standoff_h);
}

module assembly() {
    // Front-to-back stack: dark silhouette, white structures, display, carrier, PCB.
    color([0.82, 0.94, 1.00, 0.22]) transparent_silhouette_carrier();
    color("#161616") dark_silhouette_panel();
    if (show_glass_hardware) color("#777777") glass_hardware();
    color("#f1eee4") white_structures();
    translate([0, 0, silhouette_t + display_gap]) display_model();
    translate([0, 0, silhouette_t + display_gap + 4.0 + 0.5]) carrier();
    translate([0, 0, silhouette_t + display_gap + 4.0 + 0.5 + carrier_t + pcb_standoff_h]) pcb_model();
    // Rear plate terminates at the fixed 2 in outer depth.
    translate([0, 0, shadow_box_depth - backplate_t]) rear_backplate();
}

// Front-on engineering preview of the actual two-colour printable pieces.
// This deliberately excludes the display and frame so the landform can be judged.
module silhouette_preview() {
    color([0.82, 0.94, 1.00, 0.22]) transparent_silhouette_carrier();
    color("#202020") dark_silhouette_panel();
    color("#f5f3ea") white_structures();
}

if (part == "assembly") assembly();
if (part == "preview") silhouette_preview();
if (part == "silhouette") dark_silhouette_panel();
if (part == "silhouette_carrier") transparent_silhouette_carrier();
if (part == "structures") white_structures();
if (part == "glass_hardware") glass_hardware();
if (part == "backplate") rear_backplate();
if (part == "carrier") carrier();
if (part == "display") display_model();
if (part == "pcb") pcb_model();
