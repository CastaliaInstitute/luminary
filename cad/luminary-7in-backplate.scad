// Luminary 7-inch P4 rear mounting plate — production export.
// Matte-black PLA; dimensions in millimetres. Export directly to STL.
//
// Confirmed P4 envelope: PCB 164 x 97 mm; LCD/bezel 164.28 x 99.17 mm.
// The mounting pattern uses 6 x 5.2 mm tolerance slots for M2.5 fasteners
// because the vendor image does not certify exact hole-center coordinates.

$fn = 20;

plate_w = 177.8;  // 7 in
plate_h = 127.0;  // 5 in
plate_t = 3.0;

p4_land_w = 166.0;
p4_land_h = 101.6;
p4_land_t = 1.2;

slot_dx = 78.0;
slot_dy = 44.0;
slot_len = 6.0;
slot_d = 5.2;
counterbore_d = 5.8;
counterbore_t = 1.4;

magnet_d = 6.2;       // 6 mm nominal disc magnet clearance
magnet_t = 2.1;       // 2 mm nominal disc magnet clearance
magnet_dx = plate_w / 2 - 9.0;
magnet_dy = plate_h / 2 - 9.0;

cable_d = 8.0;
// Keep each exit fully circular with a 1 mm structural wall at the plate edge.
// The USB-C extension is connected internally; these holes only pass the cable.
cable_edge_wall = 1.0;
cable_edge_offset = cable_d / 2 + cable_edge_wall;

module rounded_plate() {
    // The wood shadow box hides the plate perimeter, so a plain rectangular
    // envelope is both stronger and much faster to compile than cosmetic
    // corner fillets.
    translate([-plate_w / 2, -plate_h / 2, 0]) cube([plate_w, plate_h, plate_t]);
}

module pill_slot(length, diameter) {
    hull() {
        translate([-(length - diameter) / 2, 0]) circle(d = diameter);
        translate([(length - diameter) / 2, 0]) circle(d = diameter);
    }
}

module p4_fastener(x, y) {
    translate([x, y, -0.01]) linear_extrude(height = plate_t + 0.02)
        pill_slot(slot_len, slot_d);
    // Recess the screw head on the rear face so it does not stand off from the
    // wooden frame or its steel attachment squares.
    translate([x, y, -0.01]) cylinder(d = counterbore_d, h = counterbore_t + 0.02);
}

module backplate() {
    difference() {
        union() {
            rounded_plate();
            translate([-p4_land_w / 2, -p4_land_h / 2, plate_t - 0.01])
                cube([p4_land_w, p4_land_h, p4_land_t]);
        }
        for (x = [-slot_dx, slot_dx])
            for (y = [-slot_dy, slot_dy]) p4_fastener(x, y);

        // Four rear-facing pockets accept magnets that couple to steel squares
        // fixed inside the shadow-box frame.
        for (x = [-magnet_dx, magnet_dx])
            for (y = [-magnet_dy, magnet_dy])
                translate([x, y, -0.01]) cylinder(d = magnet_d, h = magnet_t + 0.02);

        // Either cable route can be used: side for a wall install, bottom for
        // a tabletop orientation. The unused opening remains behind the box.
        translate([-plate_w / 2 + cable_edge_offset, 0, -0.01])
            cylinder(d = cable_d, h = plate_t + 0.02);
        translate([0, -plate_h / 2 + cable_edge_offset, -0.01])
            cylinder(d = cable_d, h = plate_t + 0.02);
    }
}

backplate();
