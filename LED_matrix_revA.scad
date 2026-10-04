// 8x8 LED matrix mounting frame
// http://www.thingiverse.com/thing:1883325
// Printbus, December 2016
//
// This consists of a mounting frame for 32mm 8x8 LED matrix modules.  
// Revision A adds flexibility that simplifies support for a handful of LED driver
// board types.  Types known to be supported are the FriedCircuits LED Matrix Link v1.2, 
// the printbus/klunkerbus LED Matrix Link v2.0, the board type with a MAXIM MAX7219
// located outside the LED matrix component, and the FC-16 type of driver board.  
// Sample parameters are provided for these boards types.
// 
// The top-level geometry should be tailored for the desired number of modules
// in the x-axis and the number of modules in the y-axis. For reinforcement in long, 
// thin displays, the top level geometry provides for enabling reinforcement struts
// to the x-axis for the inner rows of modules.
//
// For OpenSCAD documentation, see 
// http://en.wikibooks.org/wiki/OpenSCAD_User_Manual/The_OpenSCAD_Language
//
// NOTE ON BOARD ORIENTATIONS: The ability to properly drive pixels throughout an array 
// of modules depends on the board design, board orientation, the software driving the
// display, and the orientation of the LED matrix on the board.  Experimenting with
// a couple of LED modules before finalizing, printing, and assembling/wiring a
// display panel is suggested. 
//
// NOTE ON BOARD DIMENSIONS: The LED matrix components are typically undersized
// slightly less than 32mm. This allows adjacent LED matrices to be spaced at exact
// 32mm intervals so that all LED rows/columns in a large display are equally spaced.
// All of the driver boards sampled, however, have been found to be slightly oversized.
// Although the oversize may be small, it can accumulate and create fit problems for 
// display panels that use a multiple number of adjacent driver boards.  
// The sample dimensions provided here reflect the oversize dimensions measured on
// a small sample of board.  Check the dimensions of your boards for compatibility.
// Note that dimensions may vary across identical boards - don't assume all boards
// will have the same exact dimensions. Adjust the board dimension parameters here
// to accomodate the largest board that will be used.  If there is a concern that
// the oversize boards may lead to undesirable gaps between LED matrices, file the
// board edges as required to allow 32mm intervals and adjust the board dimension
// parameter(s) here accordingly.  
//
// NOTE ON MOUNTING SCREW SIZING: There are multiple factors to consider in choosing the 
// size of screws for mounting driver boards to the printed frame.  1) The amount
// of corner post material provided at each mounting screw location is limited by 
// the space between the board edges and the mounting holes. Using a smaller 
// screw size will increase the corner post wall thickness and provide more strength.  
// 2) Going with a smaller diameter screw provides tolerance in hole placements
// and board alignment.  3) Most boards provide only limited clearance for the
// heads of mounting screws.  A smaller diameter screw will have a smaller
// diameter head.  

// NOTE ON LED MATRIX ALIGNMENT: Sloppy alignment of the LED matrices on the boards can lead to 
// a sloppy looking display panel.   Soldering the LED SIP sockets to the board while 
// the LED matrix is plugged into the SIP sockets is suggested.  
//
// NOTES ON PANEL ASSEMBLY: Especially for larger display panels, measure received 
// driver boards for compatibility with the dimension parameters for the frame design. 
// File or sand material from board edges if boards are excessively oversized.
// Burrs or any other protrusions should be cleaned from the driver boards before 
// mounting them onto the frame.    Install all modules with hardware left initially
// loose, and tighten the hardware after adjusting boards for any alignment issues.
// If multiple smaller frames are used to form a larger display, clean any printing
// burrs from the frame sides that mate.  Zip ties might be adequate in holding partial 
// frames together. 
//
////////// REVISION HISTORY ///////////////////////////////////////////////////
// YYMMDD date code
// 161110 - initial publish
// 161203 - Revision A, improved support for additional board types

////////// INCLUDE AND USE  ///////////////////////////////////////////////////
// NOTE: openSCAD version 2014.03 or newer recommended

////////// TOP LEVEL GEOMETRY  ////////////////////////////////////////////////
// Array size and options are set in PARAMETERS below (x_count, y_count, strut)
frame( x_count=x_count, y_count=y_count, strut=strut );

////////// PARAMETERS  ////////////////////////////////////////////////////////
// All dimensions are millimeter
// Open Window > Customizer to edit these as UI controls

/* [Frame] */
// Number of LED modules in the x-axis
x_count = 8;
// Number of LED modules in the y-axis
y_count = 2;
// X-axis reinforcement struts between y-axis rows
strut = false;

/* [LED Posts] */
// Fastener type in corner posts
post_fastener = "heatset"; // [heatset:M3 heat-set insert, thread:M3 direct thread (self-tap)]
// Which corners get screw posts
post_mode = "skip"; // [diagonal:Diagonal (FL+RR), every_other:Every other column, skip:Skip X (outer of group)]
// Skip mode: physical board width in modules (e.g. 4 for 4-wide FC-16 style)
post_group_x = 4; // [1:16]
// Skip mode: physical board height in modules
post_group_y = 1; // [1:8]
// Skip mode: which side to keep at board-board seams (holes are too close for both)
post_seam = "keep_right"; // [keep_right:Keep right (skip next board left), keep_left:Keep left (skip prev board right)]
// Heat-set: recommended hole ID for typical M3 brass inserts (mm)
post_insert_id = 4.0;
// Heat-set: insert length / blind hole depth (mm)
post_insert_depth = 5.0;
// Heat-set: minimum post OD for wall strength around the insert (mm)
post_od_min = 8.0;
// Direct thread: hole ID for M3 self-tap into plastic (nominal 2.7 + print fit)
post_thread_id = 2.9;
// Fragment count for round posts and holes
fn_circle = 64; // [16:128]

/* [Panel Mount] */
// Enable frame-to-board mounting holes between modules
panel_mount = true;
// Clearance hole ID — M3 standard fit (mm)
mnt_hid = 3.3;
// Countersink OD at frame front (mm)
mnt_cs_od = 6.5;
// Countersink depth (mm)
mnt_cs_depth = 2.0;
// Circular pad OD at each mount point (mm)
mnt_pad_od = 10;

/* [Hidden] */
// Board Reference Dimensions (as measured)
// Sample measurements are provided for various module types
// Only one block of measurements should be uncommented - all others need to be commented out
// Tailor the measurements for your board type if your measurements differ
// -------- dimensions for Friedcircuits LED Matrix Link v1.2 and LED Matrix Master v1.2
// For Friedcircuits v1.2, see https://friedcircuits.us/
// Note that Friedcircuits hole spacing leaves limited room for screw heads
// If using right angle headers to interconnect boards, consider soldering them after mounting boards
//   This will help minimize header placement from affecting board spacing
/*  ( a /* here comments out this block, a //* will uncomment it )
bd_sx = 32.2;       // board size in x-axis for the LED module
bd_sy = 32.25;      // board size in y-axis for the LED module
bd_hspx = 27.8;     // board hole spacing in x-axis, center to center
bd_hspy = 27.8;     // board hole spacing in y-axis, center to center
bd_hid = 3.3;       // board hole inner diameter (not used, but included for reference)
bd_pin_len = 0;     // pcb pin length in area of I-frame strut (n/a for low if_strut_w values)
//*/

// -------- dimensions for LED Matrix Link, klunkerbus v2.0
// For klunkerbus v2.0, see https://oshpark.com/shared_projects/J1PUJCGP
// If using right angle headers to interconnect boards, consider soldering them after mounting boards
//   This will help minimize header placement from affecting board spacing
//*  ( a /* here comments out this block, a //* will uncomment it )
bd_sx = 32.15;      // board size in x-axis for the LED module
bd_sy = 32.15;      // board size in y-axis for the LED module
bd_hspx = 26.8;     // board hole spacing in x-axis, center to center
bd_hspy = 26.8;     // board hole spacing in y-axis, center to center
bd_hid = 3.3;       // board hole inner diameter (not used, but included for reference)
bd_pin_len = 0;     // pcb pin length in area of I-frame strut (n/a for low if_strut_w values)
//*/

// -------- dimensions for DIP MAX7219 located outside the 8x8 LED matrix
/*  ( a /* here comments out this block, a //* will uncomment it )
bd_sx = 32.15;      // board size in x-axis for the LED module
bd_sy = 50.0;       // board size in y-axis for the LED module
bd_hspx = 26.75;    // board hole spacing in x-axis, center to center
bd_hspy = 44.65;    // board hole spacing in y-axis, center to center
bd_hid = 3.3;       // board hole inner diameter (not used, but included for reference)
bd_pin_len = 2.0;   // pcb pin length in area of I-frame strut (required for this type)
//*/

// -------- dimensions for FC-16 type board
// bd_hspx and bd_hspy assume input/output at forward/rearward edges; swap as necessary
/*  ( a /* here comments out this block, a //* will uncomment it )
bd_sx = 32.25;      // board size in x-axis for the LED module
bd_sy = 32.25;      // board size in y-axis for the LED module
bd_hspx = 26.2;     // board hole spacing in x-axis, center to center
bd_hspy = 20.0;     // board hole spacing in y-axis, center to center
bd_hid = 3.55;      // board hole inner diameter (not used, but included for reference)
bd_pin_len = 0;     // pcb pin length in area of I-frame strut (n/a for low if_strut_w values)
//*/

// I-frame parameters for tailoring. An I-frame is provided for each 8x8 LED module in the array.
if_leg_w = 7;       // I-frame center leg or brace width
if_base_sz = 3.2;   // I-frame base size in z-axis(should be multiple of layer height)
if_sz = 12.2;       // I-frame size in z-axis (should be multiple of layer height)
if_sx = bd_sx + 0;  // I-frame size in x-axis, with any additional fit clearance
if_sy = bd_sy + 0;  // I-frame size in y-axis, with any additional fit clearance
if_strut_w = 4;     // I-frame x-axis strut width, if support strut is used
// Note: for Matrix Link and DIP style boards, increasing if_strut_w may require setting bd_pin_len
// Note: for klunkerbus LED Matrix Link v2.0, if_sz-if_base_sz must accomodate resistor used

// Fastener and hardware component hole sizing (reference table)
/**************************************************************************************
Standard hardware dimension reference data follows
These are a reference point only; adjustment may be necessary due to nozzle size, etc.
Also note that MCAD polyhole does better at actual diameters than circle/cylinder

SIZE         THREAD  CLEAR   NUT AFD  NUT OD   NUT     HEAD     HEAD    WASHER  FLAT    
             HOLE    HOLE    WRENCH   @fn=6    HEIGHT  OD       HEIGHT  HEIGHT  OD      
-----------  ------  ------  -------  -------  ------  ------   ------  ------  ----- 
M2 x 0.4     1.75mm  2.20mm  4.0mm    4.62mm   1.6mm   4.0mm    2.0mm   0.3mm   5.5mm
M2.5 x 0.45  2.20mm  2.75mm  5.0mm    5.77mm   2.0mm   5.0mm    2.5mm   0.3mm   6.0mm
M3 x 0.5     2.70mm  3.30mm  5.5mm    6.35mm   2.4mm   6.0mm    3.0mm   0.5mm   7.0mm 
M4 x 0.7     3.50mm  4.40mm  7.0mm    8.08mm   3.2mm   8.0mm    4.0mm   0.8mm   9.0mm
M5 x 0.8     4.50mm  5.50mm  8.0mm    9.24mm   4.7mm   10.0mm   5.0mm   1.0mm   10.0mm
M6 x 1.0     5.50mm  6.60mm  10.0mm   11.55mm  5.2mm   12.0mm   6.0mm   1.6mm   12.0mm
M8 x 1.25    7.20mm  8.80mm  13.0mm   15.01mm  6.8mm   16.0mm   8.0mm   2.0mm   17.0mm
#2-56        1.85mm  2.44mm  4.76mm   5.50mm   1.59mm  4.60mm   2.18mm  0.91mm  6.35mm
#3-56        2.26mm  2.79mm  4.76mm   5.50mm   1.59mm  5.28mm   2.51mm  0.91mm  7.94mm
#4-40        2.44mm  3.26mm  6.35mm   7.33mm   2.38mm  5.97mm   2.85mm  1.14mm  9.53mm
#6-32        2.95mm  3.80mm  7.94mm   9.17mm   2.78mm  7.37mm   3.51mm  1.14mm  11.11mm
#8-32        3.66mm  4.50mm  8.73mm   10.08mm  3.18mm  8.74mm   4.17mm  1.14mm  12.7mm
#10-24       4.09mm  5.11mm  9.53mm   11.00mm  3.18mm  10.13mm  4.83mm  1.14mm  14.29mm
#10-32       4.31mm  5.11mm  9.53mm   11.00mm  3.18mm  10.13mm  4.83mm  1.14mm  14.29mm
1/4-20       5.56MM  6.76MM  11.11mm  12.83mm  4.76mm  13.03mm  6.35mm  1.80mm  18.65
Notes:       1,2     2,3     4        5        6       7,8      8,9     8,10    8,11
----------------------------------------------------------------------------------------
Note  1: Thread hole is for tap or self thread of machine screw in soft material
Note  2: Hole dimensions are from littlemachineshop.com Tap Drill - 50% Thread column data
Note  3: Clearance hole data is littlemachineshop.com Clearance Drill - Standard Fit column data
Note  4: Nut Across Flat Diameter (AFD) metric data is ISO, inch is boltdepot.com US Nut Size table
Note  5: Nut round OD is the openSCAD circle diameter required to achieve a nut size at $fn=6 
         Nut OD is calculated as = (NUT AFD)/cos(30)
Note  6: Nut height is for standard hex nut; jam nuts are less, lock nuts are more
         Metric data is ISO 4032, SAE data from boltdepot.com US Nut Size tables
Note  7: Head diameter varies with the head style; value shown is max across all except truss head
Note  8: Data from http://www.numberfactory.com/nf_metric.html or http://www.numberfactory.com/nf_inch.html 
Note  9: Head height varies with head style; value shown is max across all styles
Note 10: Standard washer height or thickness
Note 11: Flat washer outer diameter
****************************************************************************************/

////////// NON-USER PARAMETERS AND CALCULATIONS ////////////////////////////////
// Parameters beyond this point are normally not altered in basic tailoring
// Modify at your own risk
// --- Mesh geometry ---
MF = 0.01;    // Mesh overlap factor is the amount of overlap on geometries for proper mesh
MSA = MF;     // Mesh Single Adjustment factor (translate ends; size adjustment on single ended mesh)
MDA = 2*MF;   // Mesh Double Adjustment (size adjustment on double ended mesh like boring holes)

// calculated parameters
post_hox = (if_sx - bd_hspx)/2;                       // post hole offset from i-frame edge in x axis
post_hoy = (if_sy - bd_hspy)/2;                       // post hole offset from i-frame edge in y axis
post_od_edge = post_hox <= post_hoy?post_hox*2:post_hoy*2; // od that just meets the module edge
// Heat-set needs thicker walls; direct thread can use the edge-fit post like original M2.5
post_od = (post_fastener == "heatset" && post_od_edge < post_od_min) ? post_od_min : post_od_edge;
if_strut_sz = if_sz - bd_pin_len;                     // i-frame strut height adjusted for any board pins

//----------------------------------------------------------
module post_screw_hole() {
// Hole in a corner post: blind heat-set pocket, or full-depth direct-thread bore
  if ( post_fastener == "heatset" )
    translate([ 0, 0, if_sz - post_insert_depth ])
      cylinder( h=post_insert_depth + MSA, d=post_insert_id, center=false, $fn=fn_circle );
  else
    translate([ 0, 0, -MSA ])
      cylinder( h=if_sz + MDA, d=post_thread_id, center=false, $fn=fn_circle );
}

//----------------------------------------------------------
module panel_mount_hole() {
// Clearance bore through the I-frame base plus countersink opening at the front face
// so a flat-head screw sits flush and can fasten the frame to a backing board.
  union() {
    translate([ 0, 0, -MSA ])
      cylinder( h=if_base_sz + MDA, d=mnt_hid, center=false, $fn=fn_circle );
    translate([ 0, 0, if_base_sz - mnt_cs_depth ])
      cylinder( h=mnt_cs_depth + MSA, d1=mnt_hid, d2=mnt_cs_od, center=false, $fn=fn_circle );
  }
}

//----------------------------------------------------------
module corner_post( px, py ) {
// Round post at (px,py) — cylinder only, no squared-up flats
  translate([ px, py, 0 ])
    cylinder( h=if_sz, d=post_od, center=false, $fn=fn_circle );
}

//----------------------------------------------------------
function post_active_column( mx ) =
  (post_mode == "every_other") ? (mx % 2 == 0) : true;

function post_group_left( mx ) =
  (mx % post_group_x == 0);

function post_group_right( mx ) =
  (mx % post_group_x == post_group_x - 1);

function post_group_front( my ) =
  (my % post_group_y == 0);

function post_group_rear( my ) =
  (my % post_group_y == post_group_y - 1);

//----------------------------------------------------------
module matrix_mount( mx, my, x_count, y_count ) {
// I-frame mount for one LED module. Posts only where heat-set inserts go (see post_mode).
  active = post_active_column( mx );

  // --- diagonal / every_other: FL+RR, plus free-edge pair on panel ends ---
  diag_fl = active;
  diag_rr = active;
  diag_fr = active && (mx == x_count - 1) && (my == 0);
  diag_rl = active && (mx == 0) && (my == y_count - 1);

  // --- skip: outer corners of each post_group_x × post_group_y physical board ---
  g_left = post_group_left( mx );
  g_right = post_group_right( mx );
  g_front = post_group_front( my );
  g_rear = post_group_rear( my );
  // Seam between boards: adjacent group_right / group_left holes are ~5.4mm apart
  suppress_left = g_left && (mx > 0) && (post_seam == "keep_right");
  suppress_right = g_right && (mx < x_count - 1) && post_group_left( mx + 1 ) && (post_seam == "keep_left");

  skip_fl = g_left && g_front && !suppress_left;
  skip_rl = g_left && g_rear && !suppress_left;
  skip_fr = g_right && g_front && !suppress_right;
  skip_rr = g_right && g_rear && !suppress_right;

  // post_group_y == 1: each row is its own strip — diagonal FL+RR on group edge
  // columns, plus both posts on the overall panel left/right ends.
  skip1_fl = g_left && !suppress_left;
  skip1_rr = g_right && !suppress_right;
  skip1_fr = g_right && !suppress_right && (mx == x_count - 1) && (my == 0);
  skip1_rl = g_left && !suppress_left && (mx == 0) && (my == y_count - 1);

  use_skip = (post_mode == "skip");
  use_skip1 = use_skip && (post_group_y == 1);

  has_fl = use_skip1 ? skip1_fl : (use_skip ? skip_fl : diag_fl);
  has_fr = use_skip1 ? skip1_fr : (use_skip ? skip_fr : diag_fr);
  has_rr = use_skip1 ? skip1_rr : (use_skip ? skip_rr : diag_rr);
  has_rl = use_skip1 ? skip1_rl : (use_skip ? skip_rl : diag_rl);

  difference() {
    union() {
      translate([ -MSA, -MSA, 0 ])
        cube([ if_sx + MDA, if_leg_w/2 + MSA, if_base_sz ], center=false);
      translate([ -MSA, if_sy - if_leg_w/2, 0 ])
        cube([ if_sx + MDA, if_leg_w/2 + MSA, if_base_sz ], center=false);
      translate([ if_sx/2 - if_leg_w/2, 0, 0 ])
        cube([ if_leg_w, if_sy, if_base_sz ], center=false);

      if ( has_fl ) corner_post( post_hox, post_hoy );
      if ( has_fr ) corner_post( if_sx - post_hox, post_hoy );
      if ( has_rr ) corner_post( if_sx - post_hox, if_sy - post_hoy );
      if ( has_rl ) corner_post( post_hox, if_sy - post_hoy );
    }

    if ( has_fl ) translate([ post_hox, post_hoy, 0 ]) post_screw_hole();
    if ( has_fr ) translate([ if_sx - post_hox, post_hoy, 0 ]) post_screw_hole();
    if ( has_rr ) translate([ if_sx - post_hox, if_sy - post_hoy, 0 ]) post_screw_hole();
    if ( has_rl ) translate([ post_hox, if_sy - post_hoy, 0 ]) post_screw_hole();
  }
}

//----------------------------------------------------------
module frame( x_count, y_count, strut) {
  // Report key info to the console
  echo(str("Frame size is x: ", x_count*if_sx,"mm, y: ", y_count*if_sy, "mm"));
  echo(str("Clearance between i-frame base and LED board: ", if_sz-if_base_sz, "mm"));
  echo(str("Corner post od: ", post_od, "mm; fastener: ", post_fastener,
    post_fastener == "heatset"
      ? str(" insert ", post_insert_id, "mm x ", post_insert_depth, "mm deep")
      : str(" thread hole ", post_thread_id, "mm through")));
  echo(str("Post mode: ", post_mode,
    post_mode == "skip" ? str(" (group ", post_group_x, "x", post_group_y, ", seam ", post_seam, ")") :
    post_mode == "every_other" ? " (even columns only)" : " (FL+RR, ends get both)"));
  if ( panel_mount )
    echo(str("Panel mount: M3 clearance ", mnt_hid, "mm, countersink od ", mnt_cs_od, "mm at midpoints between modules"));
    
  difference() {
    union () {
      // start with the I-frame mounts for each LED matrix
      for (x=[0:x_count-1])
        for (y=[0:y_count-1])
          translate([ x*if_sx, y*if_sy, 0 ])
            matrix_mount( x, y, x_count, y_count );

      // add x-axis reinforcement struts 
      if ( strut == true ) {
        if ( y_count > 1 ) {
          for (x=[0:x_count-1]) {
            for (y=[1:y_count-1]) {
              translate([ x*if_sx + post_hox + post_od/2 - MSA, y*if_sy - if_strut_w/2, 0 ])
                cube([ if_sx - 2*post_hox - post_od + MDA, if_strut_w, if_strut_sz ]); 
            }
          }
        }
      }

      // Panel mounts between modules
      if ( panel_mount ) {
        if ( x_count > 1 )
          for (x=[1:x_count-1])
            for (y=[0:y_count-1]) {
              translate([ x*if_sx - if_leg_w/2, y*if_sy - MSA, 0 ])
                cube([ if_leg_w, if_sy + MDA, if_base_sz ], center=false);
              translate([ x*if_sx, y*if_sy + if_sy/2, 0 ])
                cylinder( h=if_base_sz, d=mnt_pad_od, center=false, $fn=fn_circle );
            }
        if ( y_count > 1 )
          for (y=[1:y_count-1])
            for (x=[0:x_count-1])
              translate([ x*if_sx + if_sx/2, y*if_sy, 0 ])
                cylinder( h=if_base_sz, d=mnt_pad_od, center=false, $fn=fn_circle );
      }
    }

    if ( panel_mount ) {
      if ( x_count > 1 )
        for (x=[1:x_count-1])
          for (y=[0:y_count-1])
            translate([ x*if_sx, y*if_sy + if_sy/2, 0 ])
              panel_mount_hole();
      if ( y_count > 1 )
        for (y=[1:y_count-1])
          for (x=[0:x_count-1])
            translate([ x*if_sx + if_sx/2, y*if_sy, 0 ])
              panel_mount_hole();
    }
  }
}




