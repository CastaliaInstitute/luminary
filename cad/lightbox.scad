// Lightbox / Living Landscape shadow-box starter model
// Units: millimetres. Reference: docs/reference-notes.md
//
// Render one part at a time by changing `part` below:
//   "assembly", "silhouette", "carrier", "display", "pcb"

$fn = 64;

part = "assembly";

// Shadow-box envelope
frame_outer_w = 177.8; // 7 in
frame_outer_h = 127.0; // 5 in
panel_w = 152.4; // 6 in nominal interior
panel_h = 101.6; // 4 in nominal interior

// Waveshare ESP32-P4-WiFi6-Touch-LCD-5 reference dimensions
display_w = 126.90;
display_h = 70.70;
active_w = 110.32;
active_h = 62.28;
pcb_w = 118.50;
pcb_h = 64.50;

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

// Carrier and stack-up assumptions; verify against the actual frame.
carrier_t = 3.0;
carrier_clearance = 1.5;
display_gap = 2.5;
pcb_standoff_h = 4.0;

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

module silhouette_panel() {
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

    // Photo-derived coastal scene. Keep the SVG separate so the silhouette
    // can be revised without changing the mechanical envelope.
    if (scene_art)
        translate([-panel_w / 2 + panel_border, -panel_h / 2 + panel_border, 0])
            linear_extrude(height = nozzle_d * scene_base_layers)
                import("../assets/living-landscape-silhouette.svg");

    // Raised foreground gives the rocks and surf a stronger shadow line.
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
    // Front-to-back stack: silhouette, air gap, display, carrier, PCB.
    color("#161616") silhouette_panel();
    translate([0, 0, silhouette_t + display_gap]) display_model();
    translate([0, 0, silhouette_t + display_gap + 4.0 + 0.5]) carrier();
    translate([0, 0, silhouette_t + display_gap + 4.0 + 0.5 + carrier_t + pcb_standoff_h]) pcb_model();
}

if (part == "assembly") assembly();
if (part == "silhouette") silhouette_panel();
if (part == "carrier") carrier();
if (part == "display") display_model();
if (part == "pcb") pcb_model();
