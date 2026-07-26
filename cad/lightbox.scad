// Luminary / Living Landscape shadow-box model
// Units: millimetres. Reference: docs/reference-notes.md
//
// Render one part at a time by changing `part` below:
//   "assembly", "silhouette", "structures", "glass_hardware", "backplate", "carrier", "display", "pcb"

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
panel_border = 5.0;
scene_art = true;
nozzle_d = 0.20;
scene_base_layers = 4;       // 0.80 mm: visible face
scene_mid_layers = 6;        // 1.20 mm: focal structures
scene_foreground_layers = 8; // 1.60 mm: rocks and surf
rib_t = 2.0;
rib_h = 6.0;
magnet_d = 3.0;
magnet_h = 1.0;
magnet_edge = 7.0;

// Glass-door attachment hardware
steel_square_w = 6.35; // 1/4 in
steel_square_t = 1.0;
steel_square_edge = 7.0;
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
    translate([x, y, silhouette_t - magnet_h - 0.01])
        cylinder(d = magnet_d + 0.20, h = magnet_h + 0.02);
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

module dark_silhouette_panel() {
    difference() {
        // Full 4x6 panel with a thin visible border.
        translate([-panel_w / 2, -panel_h / 2, 0])
            cube([panel_w, panel_h, silhouette_t]);

        // Recess behind the border. This leaves the visible face intact.
        translate([-panel_w / 2 + panel_border, -panel_h / 2 + panel_border, -0.01])
            cube([panel_w - 2 * panel_border, panel_h - 2 * panel_border, silhouette_t + 0.02]);

        for (x = [-panel_w / 2 + magnet_edge, panel_w / 2 - magnet_edge])
            for (y = [-panel_h / 2 + magnet_edge, panel_h / 2 - magnet_edge])
                magnet_pocket(x, y);
    }

    // Photo-derived dark coastal scene. White structures are a separate part.
    if (scene_art)
        translate([-panel_w / 2 + panel_border, -panel_h / 2 + panel_border, 0])
            linear_extrude(height = nozzle_d * scene_base_layers)
                import("../assets/living-landscape-silhouette.svg");

    // Raised foreground gives the rocks a stronger shadow line. Water and surf
    // remain open so the display can render motion and changing light.
    if (scene_art)
        translate([-panel_w / 2 + panel_border, -panel_h / 2 + panel_border, nozzle_d * scene_base_layers])
            linear_extrude(height = nozzle_d * scene_foreground_layers)
                import("../assets/living-landscape-foreground.svg");

    // Minimal hidden ribs to stiffen the panel. Replace with the SVG-derived
    // landscape silhouette as the scene geometry is designed.
    for (x = [-panel_w / 2 + panel_border, panel_w / 2 - panel_border - rib_t])
        translate([x, -panel_h / 2 + panel_border, silhouette_t])
            cube([rib_t, panel_h - 2 * panel_border, rib_h]);
    for (y = [-panel_h / 2 + panel_border, panel_h / 2 - panel_border - rib_t])
        translate([-panel_w / 2 + panel_border, y, silhouette_t])
            cube([panel_w - 2 * panel_border, rib_t, rib_h]);
}

module white_structures() {
    translate([-panel_w / 2 + panel_border, -panel_h / 2 + panel_border, silhouette_t])
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
    color("#161616") dark_silhouette_panel();
    if (show_glass_hardware) color("#777777") glass_hardware();
    color("#f1eee4") white_structures();
    translate([0, 0, silhouette_t + display_gap]) display_model();
    translate([0, 0, silhouette_t + display_gap + 4.0 + 0.5]) carrier();
    translate([0, 0, silhouette_t + display_gap + 4.0 + 0.5 + carrier_t + pcb_standoff_h]) pcb_model();
    // Rear plate terminates at the fixed 2 in outer depth.
    translate([0, 0, shadow_box_depth - backplate_t]) rear_backplate();
}

if (part == "assembly") assembly();
if (part == "silhouette") dark_silhouette_panel();
if (part == "structures") white_structures();
if (part == "glass_hardware") glass_hardware();
if (part == "backplate") rear_backplate();
if (part == "carrier") carrier();
if (part == "display") display_model();
if (part == "pcb") pcb_model();
