// Parametric one-piece LED diffuser for HUB75 panels (MatrixPortal M4 kits).
// A thin diffuser face with the cell grid standing on it, printed face-down.
// Open in OpenSCAD, set the values below, F6 to render, File > Export > STL.
//
// Measure pitch: calipers across 10 LED centres (e.g. LED 1 to LED 11), divide by 10.
// Common Adafruit 64x32 panels: P2.5 (160x80 mm), P3 (192x96), P4 (256x128),
// P5 (320x160), P6 (384x192). Only P2.5 and P3 fit the P1S bed in one piece.
//
// Two colours on one plate: in Bambu Studio, add a filament change (or AMS
// colour switch) at the first layer above the face: white face, black grid.

/* [What to print] */
mode = "sampler";     // ["sampler", "full"]
// sampler: small patches side by side, one per face thickness in sampler_layers
sampler_layers = [1, 2, 3, 4];
test_cols = 8;
test_rows = 8;

/* [Panel] */
pitch = 3.0;          // mm between LED centres
cols = 64;
rows = 32;

/* [Grid] */
wall = 0.45;          // ~one line with a 0.4 nozzle; use 0.8 for two lines
height = 5;           // wall height above the face; taller = crisper squares
outer_extra = 0;      // extra outer-wall thickness; 0 = same as inner walls
fit_clearance = 0.0;  // shrink pitch slightly if the grid drifts off the LEDs

/* [Diffuser face] */
layer_height = 0.2;   // match your slicer's layer height
face_layers = 2;      // face thickness in layers (full mode)

p = pitch - fit_clearance;
border = wall/2 + outer_extra;  // outer wall straddles the cell edge like inner walls

module diffuser(c, r, layers) {
    t = layers * layer_height;
    w = c * p; h = r * p;
    // face at z=0 (touches the bed), grid on top of it
    translate([-border, -border, 0]) cube([w + 2*border, h + 2*border, t]);
    translate([0, 0, t - 0.01]) difference() {
        translate([-border, -border, 0]) cube([w + 2*border, h + 2*border, height]);
        for (i = [0:c-1], j = [0:r-1])
            translate([i*p + wall/2, j*p + wall/2, -1])
                cube([p - wall, p - wall, height + 2]);
    }
    // thickness label notches on the frame edge: one notch per layer
    for (k = [0:layers-1])
        translate([k*1.5, -border - 0.6, 0]) cube([0.8, 0.7, t]);  // face only, so it clears the LEDs
}

if (mode == "sampler") {
    gap = 6;
    for (n = [0:len(sampler_layers)-1])
        translate([n * (test_cols*p + 2*border + gap), 0, 0])
            diffuser(test_cols, test_rows, sampler_layers[n]);
} else {
    diffuser(cols, rows, face_layers);
}
