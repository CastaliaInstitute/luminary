// Luminary / Living Landscape shadow-box model
// Units: millimetres. Reference: docs/reference-notes.md
//
// Render one part at a time by changing `part` below:
//   "assembly", "preview", "silhouette", "silhouette_carrier", "structures", "glass_hardware", "backplate", "carrier", "display", "pcb", "p4", "p4_mask"

$fn = 64;

part = "assembly";

// Shadow-box envelope
frame_outer_w = 177.8; // 7 in landscape door
frame_outer_h = 127.0; // 5 in landscape door
shadow_box_depth = 50.8; // 2 in overall depth
scene_frame_w = 152.4; // 6 in hidden magnetic scene insert
scene_frame_h = 101.6; // 4 in hidden magnetic scene insert
p4_mount_w = 166.0;    // board is 164 mm wide; 1 mm relief each side
p4_mount_h = 101.6;
glass_visible_w = 139.7; // 5.5 in clear door window
glass_visible_h = 88.9;  // 3.5 in clear door window

// Waveshare ESP32-P4-WiFi6-Touch-LCD-7 reference dimensions (supplied 7 in outline).
display_w = 164.28;
display_h = 99.17;
active_w = 154.58;
active_h = 86.42;
pcb_w = 164.00;
pcb_h = 97.00;

// Printed rear plate / mounting architecture. Print in matte black PLA; this
// is the structural P4 mounting plate, not a separate wood-frame backing.
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
// The P4 sits directly against the rear mounting architecture. The full box
// depth is available for the layered relief, as it should be in a shadow box.
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
        frame_ring_2d(scene_frame_w, scene_frame_h,
                      scene_frame_w - 2 * (panel_border - tapered_edge_w),
                      scene_frame_h - 2 * (panel_border - tapered_edge_w));

    // The final 2 mm toward the clear window feathers away instead of creating
    // a hard visible frame edge.
    linear_extrude(height = height, scale = 0.94)
        frame_ring_2d(scene_frame_w - 2 * (panel_border - tapered_edge_w),
                      scene_frame_h - 2 * (panel_border - tapered_edge_w),
                      scene_frame_w - 2 * panel_border,
                      scene_frame_h - 2 * panel_border);
}

module glass_steel_square(x, y) {
    // Square is bonded to the inside face of the glass; the magnet is retained
    // in the silhouette panel directly behind it.
    translate([x, y, -steel_square_t])
        cube([steel_square_w, steel_square_w, steel_square_t], center = true);
}

module glass_hardware() {
    for (x = [-scene_frame_w / 2 + steel_square_edge, scene_frame_w / 2 - steel_square_edge])
        for (y = [-scene_frame_h / 2 + steel_square_edge, scene_frame_h / 2 - steel_square_edge])
            glass_steel_square(x, y);
}

module transparent_silhouette_carrier() {
    // Clear carrier is deliberately only a hidden rabbet ring—not a full 4x6
    // sheet. The photograph-derived island reaches the side margins itself.
    union() {
        // A simple closed ring is more reliable than the former coplanar taper.
        linear_extrude(height = silhouette_carrier_t)
            frame_ring_2d(scene_frame_w, scene_frame_h,
                          scene_frame_w - 2 * panel_border,
                          scene_frame_h - 2 * panel_border);

        // Two hairline supports retain otherwise isolated foreground boulders.
        // They terminate under dark rock and remain visually absent over water.
        for (x = [-10, 34])
            linear_extrude(height = silhouette_carrier_t)
                hull() {
                    translate([x, -scene_frame_h / 2 + panel_border + 0.2]) circle(d = 0.6);
                translate([x, -23]) circle(d = 0.6);
            }

        // Island no longer stretches across the opening. These two short clear
        // ties terminate behind its left/right rock shelf and disappear into
        // the rabbet ring.
        for (side = [-1, 1])
            linear_extrude(height = silhouette_carrier_t)
                hull() {
                    translate([side * (scene_frame_w / 2 - panel_border + 1.0), 7]) circle(d = 0.6);
                    translate([side * 47.5, 7]) circle(d = 0.6);
                }

        // Four hidden rear pods carry the magnets that couple to the steel squares.
        for (x = [-scene_frame_w / 2 + magnet_edge, scene_frame_w / 2 - magnet_edge])
            for (y = [-scene_frame_h / 2 + magnet_edge, scene_frame_h / 2 - magnet_edge])
                corner_magnet_pod(x, y);
    }
}

module dark_silhouette_panel() {
    // Source-traced island and foreground boulders, bonded to the transparent
    // carrier rather than forced to grow visible side bridges.
    if (scene_art)
        translate([-glass_visible_w / 2, -glass_visible_h / 2, silhouette_carrier_t])
            scale([glass_visible_w / svg_field_w,
                   glass_visible_h / svg_field_h, 1])
                linear_extrude(height = nozzle_d * scene_base_layers)
                    import("../assets/living-landscape-photo-trace.svg");

}

module white_structures() {
    // Print separately in white. The two narrow lantern mullions in the SVG
    // leave its centre open; that transparent window is lit by the display.
    translate([-glass_visible_w / 2, -glass_visible_h / 2, silhouette_carrier_t])
        scale([glass_visible_w / svg_field_w,
               glass_visible_h / svg_field_h, 1])
            linear_extrude(height = nozzle_d * scene_mid_layers)
                import("../assets/living-landscape-structures.svg");
}

module rear_magnet_pocket(x, y) {
    // Pockets open on the rear face toward the steel plates in the frame.
    translate([x, y, -0.01])
        cylinder(d = rear_magnet_d + 0.25, h = rear_magnet_h + 0.02);
}

module p4_mount_slot(x, y) {
    // 5 x 3.2 mm tolerance slot for M2.5 hardware. It tolerates small
    // differences between the reference drawing and the actual board holes.
    translate([x - 2.5, y - 1.6, -0.01]) cube([5.0, 3.2, backplate_t + 0.02]);
}

module rear_backplate() {
    usb_side_cutout_x = usb_side == "left"
        ? -frame_outer_w / 2 + usb_cable_cutout_d / 2 + usb_edge_clearance
        : frame_outer_w / 2 - usb_cable_cutout_d / 2 - usb_edge_clearance;
    usb_bottom_cutout_y = -frame_outer_h / 2 + usb_cable_cutout_d / 2 + usb_bottom_edge_clearance;
    difference() {
    // Full 5x7 matte-black PLA plate, aligned with the shadow-box exterior.
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

        // P4 mounting hardware: use M2.5 screws and washers from the rear.
        for (x = [-78.0, 78.0])
            for (y = [-44.0, 44.0])
                p4_mount_slot(x, y);
    }

    // Wide rear mounting land: accommodates the 164 mm P4 PCB with 1 mm per-side relief.
    translate([-p4_mount_w / 2, -p4_mount_h / 2, backplate_t - 0.10])
        cube([p4_mount_w, p4_mount_h, insert_t]);

}

module display_model() {
    color("#202124")
        translate([-display_w / 2, -display_h / 2, 0])
            rounded_box([display_w, display_h, 4.0], 3);
    color("#101820")
        translate([-active_w / 2, -active_h / 2, 4.0])
            cube([active_w, active_h, 0.2]);
}

module pcb_model() {
    color("#1267a8")
        translate([-pcb_w / 2, -pcb_h / 2, -1.6])
            cube([pcb_w, pcb_h, 1.6]);
}

module p4_reference_model() {
    // Simplified fit-check model based on the supplied Waveshare outline.
    // It captures the display shell, active area, and the PCB envelope.
    union() {
        display_model();
        pcb_model();
    }
}

module p4_aspect_mask() {
    // A pair of 0.6 mm matte-black PLA rails on the active LCD. They reduce
    // the visible 16:9 artwork to the 5.5:3.5 (1.571:1) scene aspect without
    // altering the actual P4 screen or its naturally thin/wider bezels.
    mask_w = active_h * glass_visible_w / glass_visible_h; // 97.83 mm
    side_w = (active_w - mask_w) / 2;
    for (side = [-1, 1])
        translate([side * (mask_w / 2 + side_w / 2), 0, 4.2 + 0.3])
            cube([side_w, active_h, 0.6], center = true);
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
if (part == "p4") p4_reference_model();
if (part == "p4_mask") p4_aspect_mask();
