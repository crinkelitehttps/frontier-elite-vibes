# Unnamed functions in fe2.s

Call targets (jsr/bsr) that have a bare `Lxxxxx` label. Guesses come from reading the first lines of each routine and its callees, not from tracing it in-game.
Confidence: H = fairly sure, M = plausible, L = weak guess, U = unknown.

| Addr | Callers | Guess | Conf |
|---|---|---|---|
| L34c10 | 1 | Low-level line/span rasteriser pieces | L |
| L34eea | 1 | Low-level line/span rasteriser pieces | L |
| L35028 | 1 | Draw a clipped primitive: allocates temp frame, calls 37e22 then the clipper | M |
| L3506e | 2 | 3D/2D line clipping and rasterising against the 320x169 view | M |
| L350e2 | 3 | 3D/2D line clipping and rasterising against the 320x169 view | M |
| L358aa | 1 | XOR a word in a table (toggle a pixel/flag) | L |
| L358b6 | 1 | Entry with a 21912-byte stack frame (probably a heavy 3D render path) | L |
| L3591e | 2 | 3D/2D line clipping and rasterising against the 320x169 view | M |
| L35c3a | 1 | Draw a clipped primitive: allocates temp frame, calls 37e22 then the clipper | M |
| L35d20 | 2 | Scanner: draw a vertical stalk of d2 pixels at (d0,d1) into logscreen2 (clips at row $c6, finds the word and bit mask), loops L35d64 | M |
| L35d64 | 1 | Scanner stalk row loop: BackHLine (via 642(a0)) one row with mask d7, then next row (160 bytes), d2 times | M |
| L36792 | 1 | Square root or log iteration on d0 | L |
| L368a6 | 3 | Integer square root wrappers with exponent normalisation (32/16-bit) | M |
| L368c4 | 2 | Integer square root wrappers with exponent normalisation (32/16-bit) | M |
| L37b9a | 2 | Convert two angles to a unit vector using the sine table | H |
| L37c34 | 5 | 3D/2D line clipping and rasterising against the 320x169 view | M |
| L37d3e | 2 | 3D/2D line clipping and rasterising against the 320x169 view | M |
| L37d96 | 11 | 3D clipping helper: bisect a line segment (halve deltas until it meets the clip edge) | M |
| L37e22 | 5 | Average/smooth 4 points (add + asr #2): curve/Bezier subdivision step | M |
| L3810e | 6 | Project a point and offset it to screen centre (round objects) | M |
| L386c6 | 1 | Epilogue piece (pops a5/a6 and returns) | M |
| L386ce | 2 | Iterate over a list of Z-tree nodes calling a routine from a table | L |
| L388d2 | 1 | Draw the twinkly distant objects (stars) | M |
| L38fb4 | 8 | Walk an object's parent chain (68(a0)) accumulating position/distance relative to a frame | M |
| L38fba | 1 | Walk an object's parent chain (68(a0)) accumulating position/distance relative to a frame | M |
| L3901a | 1 | Position of object a3 relative to a0 via the parent chain | M |
| L3901c | 3 | Position of object a3 relative to a0 via the parent chain | M |
| L390f2 | 6 | Big relative-position/orientation computation between two objects (builds a 220-byte frame) | L |
| L39682 | 1 | Start of a model setup (adds $12 to a4) | L |
| L3a68c | 2 | 2D clip test for a primitive | L |
| L3ae88 | 3 | Near-plane clip + project one line endpoint (z at 12(a0) >= $40; else bisects with L37d96); drops the whole line (pops the return address) if both ends are behind or off the 320x168 view | M |
| L3b2cc | 6 | Outcode: classify d6 against the 0..320 x range, sets bits in d7 | M |
| L3b2e8 | 6 | Outcode: classify d6 against the 0..168 y range | M |
| L3b5c6 | 1 | Complex-object bytecode interpreter (dispatch table of segment types) | M |
| L3b5ee | 2 | Complex-object bytecode interpreter (dispatch table of segment types) | M |
| L3b7f6 | 2 | Complex-object step via host call Nu_ComplexSNext | M |
| L3be48 | 4 | Forward-differencing step (accumulating adds) for drawing a curve, projects each point | M |
| L3c3be | 2 | Draw scenery/map features | L |
| L3c82a | 1 | Draw scenery/map features | L |
| L3dc5e | 2 | Planet feature visibility test | L |
| L3dcbc | 3 | Planet-feature rendering (projection and visibility tests) | M |
| L3dd6e | 1 | Planet-feature rendering (projection and visibility tests) | M |
| L3ddc0 | 5 | Planet-feature rendering (projection and visibility tests) | M |
| L3ddc4 | 2 | Planet-feature rendering (projection and visibility tests) | M |
| L3e036 | 2 | Planet-feature rendering (projection and visibility tests) | M |
| L3e5c0 | 1 | Jump table for number formatting types | M |
| L3e630 | 1 | Number formatting | L |
| L3e6aa | 1 | Format number with 1 / 2 decimal places (insert "." ) | H |
| L3e6c6 | 1 | Format number with 1 / 2 decimal places (insert "." ) | H |
| L3e7ba | 1 | Measure/wrap text (counts characters per line) | L |
| L3f566 | 2 | 64-bit square root / proximity distance (the one fixed for sqrt overflow) | H |
| L3fa8e | 1 | Collision hit test: point d0-d2 (object-relative) against the hull bytecode of object a1's model; returns d0 = 1 clear, 0 or -1 hit, with flags in 16016(a6) (collision-and-raycast.md) | M |
| L3fc9e | 1 | Hull test of a nested sub-part named by the hull bytecode (offset d6, orientation code d3, base d5); recursion of L3fa8e | M |
| L3fd6a | 3 | Test the point against one hull plane (d0 = plane index): d4 = 0 unless it is beyond the margin d7 | M |
| L3fe0e | 2 | Rotate the 3x3 matrix at a0 a quarter turn: swap two columns and negate one | M |
| L3fe46 | 2 | Transpose the 3x3 matrix at a0 | H |
| L3fe56 | 1 | Scale the 48-bit vector 44(a0) to 16 bits, rotate it by the transpose of matrix a1 and store it at 98(a0) (world to parent-local frame in L3f460) | M |
| L4011c | 1 | Same as L3fe56 with the matrix not transposed (parent-local back to world) in L3f460 | M |
| L402ce | 2 | Apply a packed axis code in a2 (bits 0-2 pick a permutation from L3c47e, bits 3-5 negate) to the words d3-d5 | M |
| L40314 | 2 | Ray cast: insert/merge an interval into the list of ray parameter intervals at a4 (nodes: +30 t, +32 next, +34 flags) | M |
| L4048e | 2 | Ray cast: interval list merge, variant for the exit side | M |
| L40504 | 1 | Ray cast: interval list intersection of the part being clipped with the list so far | M |
| L405dc | 1 | Ray cast: rotate the ray start 16100(a6) by the object matrix 16140(a6) | M |
| L40726 | 1 | Ray cast: rotate the ray start 16070(a6) by the transposed object matrix 16140(a6) | M |
| L40888 | 4 | Same scale-and-rotate as L3fe56 for the position 44(a1), used per object by the ray cast | M |
| L409fc | 2 | Ray cast entry (822(a5), used for laser beams): ray from a position/direction against every object of the frame | H |
| L40e4c | 2 | Ray cast result: scale a vector by 16236(a6) to the hit point | M |
| L4110e | 1 | Ray cast: bounding-sphere quick reject of object a1 (radius 266(a1)) against the ray | M |
| L413e0 | 2 | Ray cast: walk the hull bytecode of a model (same format as L3fba6), clip the ray against each convex part and merge the intervals | M |
| L41628 | 2 | Clip the ray against the plane list of one convex hull part (entering side); gives t range in a0/a1 | M |
| L41668 | 1 | Same as L41628 with the plane sense inverted | M |
| L416a0 | 2 | Fetch plane d0 from the model's plane table (packed normal and offset) and the ray's dot product with it | M |
| L41ddc | 1 | Empty stub (just rts) | H |
| L4221c | 1 | Clear the icon-position list (A6_icon_positions = -1) | H |
| L42a24 | 1 | Format a number into a temporary buffer then continue to 42a32 | M |
| L42a32 | 3 | Format number d0 as decimal text into a0 (d1 = decimal places) | H |
| L42a34 | 4 | Format number d0 as decimal text into a0 (d1 = decimal places) | H |
| L42bb0 | 1 | Format d0 as 8 hex digits | H |
| L42c72 | 4 | DrawStrToPhys wrapper preserving registers | H |
| L42c92 | 1 | Parse a decimal number from the string at a0 | M |
| L42cb4 | 5 | Normalise a vector (d0..d2) to unit length | M |
| L42cba | 2 | Normalise a vector (d0..d2) to unit length | M |
| L42cde | 7 | Orthonormalise a rotation matrix (normalise 3 vectors with 42cb4) | M |
| L42dd6 | 6 | Bucket a distance (d1) into one of 6 levels | M |
| L4350e | 5 | Relative-vector helpers between objects (used for "relative to" frames / camera) | L |
| L4351e | 1 | Relative-vector helpers between objects (used for "relative to" frames / camera) | L |
| L435aa | 2 | Relative-vector helpers between objects (used for "relative to" frames / camera) | L |
| L43626 | 2 | Relative-vector helpers between objects (used for "relative to" frames / camera) | L |
| L43676 | 3 | Relative-vector helpers between objects (used for "relative to" frames / camera) | L |
| L436a4 | 3 | Relative-vector helpers between objects (used for "relative to" frames / camera) | L |
| L43b7e | 1 | Matrix/dot-product style arithmetic | L |
| L43bc6 | 1 | Matrix/dot-product style arithmetic | L |
| L440fe | 1 | Iterate objects and process each (parents, relative transform) | L |
| L441ce | 1 | Draw the cockpit sky (Lcockpit_sky) | M |
| L44252 | 2 | Per-frame object scene setup (time of day, matrices) | L |
| L44340 | 1 | Iterate objects and process each (parents, relative transform) | L |
| L445e8 | 1 | Copy a 3D object to a temp buffer and compute with it | L |
| L44ace | 2 | Object index to pointer: table L44b02[idx*2] + base (a4, game-objs, or a6 depending on entry) | H |
| L44adc | 1 | Object index to pointer: table L44b02[idx*2] + base (a4, game-objs, or a6 depending on entry) | H |
| L44ae6 | 2 | Object index to pointer: table L44b02[idx*2] + base (a4, game-objs, or a6 depending on entry) | H |
| L44aea | 5 | Object index to pointer: table L44b02[idx*2] + base (a4, game-objs, or a6 depending on entry) | H |
| L44af4 | 8 | Object index to pointer: table L44b02[idx*2] + base (a4, game-objs, or a6 depending on entry) | H |
| L44af8 | 6 | Object index to pointer: table L44b02[idx*2] + base (a4, game-objs, or a6 depending on entry) | H |
| L44be8 | 1 | Clear the 114-byte object table | H |
| L44c4a | 2 | Test a bit in the object-flag table and call a method | L |
| L44f1c | 3 | Spawn random object(s) (44f1c type 15, with name) | L |
| L44fd6 | 2 | Spawn random object(s) (44f1c type 15, with name) | L |
| L45770 | 1 | Reset the RNG seeds to fixed values when d0 < $400 | H |
| L457d8 | 1 | Per-object update dispatch (calls three methods of each object class) | M |
| L458b4 | 4 | Map an object type (d0) to a category 3..5 | L |
| L45960 | 1 | Matrix/dot-product style arithmetic | L |
| L45af8 | 1 | Matrix/dot-product style arithmetic | L |
| L45f1e | 1 | Play a sound and call routine 816 with a computed value | L |
| L461fc | 1 | Convert 16.16 coordinates to 32-bit integers | L |
| L46666 | 4 | Planet loop of the system generator: draws orbits, masses (via L4754a) and calls L46bd0 for each planet of a star; see system-generation.md | M |
| L46ac8 | 3 | Make a double star: two components named 'Name A,B' with the mass split, second at phase $7fff | M |
| L46bc8 | 1 | Fill the derived fields of a body record without the class lookup (enters L46cb4) | M |
| L46bd0 | 6 | Create one body record for mass d0: class from table L46418/L463ea, name (system name + letter/digit counters, or randnames), then L46cb4 | M |
| L46ea4 | 1 | Base of a computed dispatch (`jsr L46ea4(pc,a3.w)`, a3 = per-class offset set in L46bd0): offset 0 is this rts, the other offsets are class post-processing routines that follow | M |
| L47164 | 5 | Create up to d1 ground-base records on the current body: template L47b62, model from L47210, orbit from L47270, random name; max 18 stations/bases | M |
| L472b0 | 1 | Create one orbital-station record of type d0 (0..2 selects template L477e2/L47ba2/L47a62); max 18 | M |
| L47370 | 8 | Give an NPC/ship a random name via the randnames module | M |
| L4752e | 14 | PRNG | H |
| L4754a | 2 | Saturating curve lookup: d0 < $4000 gives table L47562[(d0 >> 5) & $1fe] (0..$ffff), else $ffff; used by the planet loop L46666 | M |
| L4b268 | 2 | Hyperspace destination screen | L |
| L4b2e0 | 1 | Hyperspace map helper | L |
| L4b31e | 1 | Screen helpers that copy the 17348(a6) table and draw (hyperspace/map) | L |
| L4b35e | 1 | Screen helpers that copy the 17348(a6) table and draw (hyperspace/map) | L |
| L4b8f2 | 3 | Look up a game-data object and compute its width+margin | L |
| L4b914 | 1 | Copy a formatted string to a0 / draw it | M |
| L4b920 | 1 | Copy a formatted string to a0 / draw it | M |
| L4ff38 | 10 | Draw one face-part bitmap (offset in a1+a0); used by the face generator | M |
| L5f138 | 1 | Table lookup in an object record | L |
| L5f290 | 1 | Table lookup in an object record | L |
| L5f326 | 4 | Start station/docking music depending on the option and location | M |
| L5f51a | 2 | Rebuild the station list: clear 1248(a6), walk the objects with callback L5f4f4 (copies the model byte of each station into a 474-byte record, max $12) | H |
| L5f546 | 1 | Generate the economy of every station of the system on arrival: 31 commodity price/quantity pairs, shop items and news entries (see starport-economy.md) | M |
| L5f67e | 2 | Create one system-wide offer entry in the 24-byte list at 9768(a6) through the handler table L5f6a0 | M |
| L5f6a0 | 1 | Not a function: word-offset jump table of 16 handlers (L5f80c, L5f814, L5f828, L5f704, L5f75c, L5f7b4) used by L5f67e | H |
| L5f6cc | 3 | Pick a random neighbouring system (starsystemview fn 1) and fetch its rating byte (60(a6) fn 1); d1 = system, d0 = rating | M |
| L5f8ca | 1 | Periodic market refresh (event $f): expire list entries, blend all prices 3:1 with a fresh roll, drop/add station news and shop items | M |
| L5fa52 | 6 | Add one random item (kind d5) to the station's shop list at 416(a0), max 14, 4 bytes each | M |
| L5fa70 | 2 | Choose an item for L5fa52: index from table L5fac2 by kind and system class, returns d1 = item id, d0 = random word | M |
| L5fbc0 | 2 | Commodity price: base from the L5fcd2 table through interpolation curves, +-3% jitter (d4 = commodity flags) | M |
| L5fc52 | 2 | Commodity quantity: counterpart of L5fbc0, base word 4(a2)/6(a2), jitter up to +100% | M |
| L5fdca | 2 | Generate one station news entry (16 bytes at 126(a0)) via the 16-way table L5fde4 | M |
| L5fde4 | 1 | Not a function: word-offset jump table for L5fdca (ban, shortage, system-state and neighbour-system notices) | H |
| L601ae | 1 | Copy commodity name d0/2 (31 names, Water ... ) to a0 | H |
| L601c0 | 2 | Format one commodity line (name + amount) from the 1204(a6) table | M |
| L60438 | 1 | Generate a random registration/ID string | M |
| L60444 | 1 | Generate a random registration/ID string | M |
| L6047c | 1 | Generate a random registration/ID string | M |
| L60530 | 3 | Format a ship/system registration string (letters + digits) | M |
| L60552 | 5 | Append a random letter A-Z to the string at a0 | H |
| L60560 | 7 | Append a random digit 0-9 to the string at a0 | H |
| L615bc | 3 | Create the player ship object and reset the system history | L |
| L615c0 | 1 | Create the player ship object and reset the system history | L |
| L61666 | 1 | Create the player ship object and reset the system history | L |
| L6186e | 2 | Leave-station/launch setup | L |
| L61914 | 1 | HUD panel / icon toggle and redraw helpers | L |
| L61a56 | 1 | Apply a speed limit from the frame step to a ship (14446(a6)) | L |
| L61c26 | 1 | Atmosphere/planet-approach processing incl. the 20 km drag cutoff | M |
| L6203c | 1 | Main player flight frame: control update, physics, player update | M |
| L622c4 | 1 | Arrival/landing handling after a long approach (checks distance $b71b00) | L |
| L622de | 5 | Arrival/landing handling after a long approach (checks distance $b71b00) | L |
| L62400 | 2 | Dust/scenery gate before the scenery spawner at L61cf4 | M |
| L625d6 | 2 | Overflow-checking shift helper | L |
| L6275c | 3 | Return unless the 3D view is mode 2 or 3 | H |
| L62772 | 2 | Read mouse control into the flight controls | M |
| L627ac | 4 | Physics sub-step: gravity/orbit advance (hooks Lgrav_step, Lorbit_align) | M |
| L6281a | 5 | Kill handler: zero player speed vectors (hooks Lkill_log) | M |
| L6287c | 1 | Per-frame player update (hook Lpos_player; music, docking checks) | M |
| L62886 | 6 | Per-frame player update (hook Lpos_player; music, docking checks) | M |
| L62966 | 3 | Build debris or dynamic-colour copy of the player ship (copies 96 bytes) | L |
| L62bdc | 1 | Cockpit key handler (view switch, targeting, etc.) | M |
| L62be6 | 4 | Cockpit key handler (view switch, targeting, etc.) | M |
| L62f02 | 1 | Landing or launch sequence step | L |
| L63002 | 7 | Count down a timer at 722(a6) by the frame step; reset when it underflows | M |
| L63008 | 3 | Count down a timer at 722(a6) by the frame step; reset when it underflows | M |
| L6304a | 1 | Draw/animate a ship fly-in (docking/launch view) | L |
| L632a0 | 2 | Ship destruction/escape sequence handler | L |
| L63362 | 1 | End of the hyperspace effect: plays effect 20, clears mode 704, restores the background colour, stamps the player's time (126(a0)) and creates the arrival ship (model 10) | M |
| L6345a | 1 | Launch/fire a missile at the current target (checks flags, plays sound) | L |
| L63482 | 3 | Launch/fire a missile at the current target (checks flags, plays sound) | L |
| L6348e | 2 | Launch/fire a missile at the current target (checks flags, plays sound) | L |
| L63990 | 2 | Draw the target indicator (tunnel squares, speed readout) | M |
| L63b82 | 1 | Scale 9 matrix words by about 0.99 (re-damp/renormalise a rotation matrix) | L |
| L63cca | 1 | Draw the HUD cross-hair (hook Lhud_cross) | M |
| L63d3a | 4 | Draw one cross-hair arm: add the cross position (4(a7), 6(a7)) to the endpoints and call A5_DrawLine (data L63d76..L63d94) | M |
| L63dc6 | 1 | Draw a square outline, centre d0/d1, half-size d2, colour 716(a6), with 4 clipped lines (target tunnel squares) | H |
| L63e30 | 1 | Cycle the 3D view mode (front/rear/side) | M |
| L63e6e | 19 | Redraw the cockpit panel / HUD button area (view-mode icon + panel bitmaps) | M |
| L63fd4 | 4 | Draw the equipment list only if the flag bit is set | H |
| L63fde | 4 | Draw the equipment/missile list (8 slots at 172(a0)) | M |
| L6414e | 8 | Blit a panel bitmap to both buffers at y=$bc, preserving registers | H |
| L64160 | 1 | HUD panel / icon toggle and redraw helpers | L |
| L64184 | 1 | HUD panel / icon toggle and redraw helpers | L |
| L6f052 | 1 | Draw the time-of-day readout | M |
| L6f174 | 1 | Draw the date readout on the clock/comms screen | M |
| L6f178 | 1 | Draw the date readout on the clock/comms screen | M |
| L6f18e | 1 | Draw the full clock/date panel | L |
| L6f210 | 2 | Draw a gauge or rotating indicator on that panel (lines + matrix rotate) | L |
| L6f264 | 2 | Rotating indicator drawing | L |
| L7244a | 3 | Options screen redraw: reset saved state and blit the screen | L |
| L72460 | 1 | Options screen redraw: reset saved state and blit the screen | L |
| L7246c | 1 | Options screen redraw: reset saved state and blit the screen | L |
| L7257c | 4 | Open the load/save confirmation sub-screen | L |
| L725ba | 2 | Copy a filename string (a0) to the save-name buffer and store d0 | H |
| L728cc | 3 | Save/load screen key handler | M |
| L72b7e | 1 | File selector screen | M |
| L72be2 | 2 | Save/load slot picker: increment a save counter and draw it | L |
| L72cb8 | 1 | Build the file list (calls 450/516 from the jump table) | L |
| L72d1a | 1 | Add the ".." parent dir entry to the file selector | M |
| L72d56 | 1 | Set current directory from a0 and read it | M |
| L72d76 | 1 | File selector: go up one directory (finds last path separator) | M |
| L72dae | 1 | File selector: enter a subdirectory | M |
| L746ba | 2 | NPC group spawners (pirates/police/traders by type; call 74a38 and 74d7a) | M |
| L746fe | 1 | NPC group spawners (pirates/police/traders by type; call 74a38 and 74d7a) | M |
| L74732 | 2 | NPC group spawners (pirates/police/traders by type; call 74a38 and 74d7a) | M |
| L747e2 | 1 | NPC group spawners (pirates/police/traders by type; call 74a38 and 74d7a) | M |
| L7483e | 1 | NPC group spawners (pirates/police/traders by type; call 74a38 and 74d7a) | M |
| L7490a | 1 | Empty stub (just rts) | H |
| L7490c | 1 | Pirate spawn roll: RandInt(7); 0 = lone pirate, 1-2 = group (issue 6) | H |
| L749aa | 4 | Set the bounty (238) of a fresh pirate from the word table L749c6, indexed d2 + (rand & $1e); also sets class byte 236 = $fb | M |
| L749f0 | 1 | NPC group spawners (pirates/police/traders by type; call 74a38 and 74d7a) | M |
| L74a34 | 3 | Spawn an NPC ship (object type 79) of class d0 near the player | M |
| L74a38 | 7 | Spawn an NPC ship (object type 79) of class d0 near the player | M |
| L74c76 | 1 | Pick an AI weapon byte: tier from a random 0..50 plus class bias (L74c4a) times the budget d4, looked up in L74c62/L74c57 | M |
| L74ca2 | 3 | Create four escorts/wingmen (calls 74cb6 x4) | L |
| L74cb6 | 4 | Create one wingman/escort via the starsystemview module (d2=side) | L |
| L74d10 | 4 | Pick a random station of the system (1254(a6) records, count 1248(a6)) and place it at its object (516(a5)); used as a leaving ship's destination | H |
| L74d36 | 1 | Pick a random station record with bit 4 of 125 set (the d0-th such record; same 474-byte table as 1254(a6)); used by the orbital class 11 spawner | M |
| L74d58 | 1 | Init a new NPC: random seed and name | M |
| L74d7a | 7 | Finish spawn: randomise seeds/orientation and zero velocity of new NPC | M |
| L74fb0 | 1 | NPC ship interaction/AI routines (docking, speed limits) | L |
| L75006 | 1 | NPC ship interaction/AI routines (docking, speed limits) | L |
| L75020 | 1 | NPC ship interaction/AI routines (docking, speed limits) | L |
| L7508a | 1 | NPC ship interaction/AI routines (docking, speed limits) | L |
| L75248 | 12 | Ship timer/damage counter: subtract d1 from a byte, destroy/trigger when it underflows | L |
| L75852 | 1 | Spawn a ship (type 9) with random orientation and a sound effect | L |
| L75968 | 1 | Spawn a ship tied to object a0 (copies its id into 212) | L |
| L75c92 | 3 | Refresh the current station screen: dispatch on 11732(a6) to the builder of that screen (station-screens.md section 2) | M |
| L75d10 | 2 | Draw the station screen: title, face box text and the visible page of items (3 or 13 per page) via L75d72 | M |
| L75d72 | 1 | Draw one list item with its icon, plus the up/down scroll arrows when the list continues | M |
| L75eae | 1 | Show message d0 (L76130) then rebuild the whole station screen (L75eb2) | M |
| L75f1c | 4 | Set flag, default dialogue pointer, then fall into 75f2a reset | M |
| L75f2a | 29 | Reset station dialogue list state (clear counters, key scancode) | M |
| L76018 | 85 | Append an entry to the station-screen item/dialogue list (24-byte records at A6+10578, 45 max); d0 = text id | M |
| L76058 | 1 | Post a message to the station screen: close the panel, play effect 29, queue it (max 4) if a message is already on screen | M |
| L760ac | 1 | Start showing a message: store it at 11690(a6), arm the message timer | M |
| L760ba | 1 | Draw the pending message on the face box | M |
| L76130 | 32 | Run a station dialogue/menu screen: draw face + text, poll keys, return the choice | M |
| L763e8 | 1 | End the station screens: restore the panel unless a flag is set | M |
| L76412 | 2 | Clear station-screen flag 115 bit 0 and release scan keys 49 to 53 | M |
| L76436 | 2 | Run the picked item: play a sound, load its 24-byte record and jump through the L76476 id table | H |
| L76a76 | 1 | Shipyard upgrades screen: list the upgrades available for the player's ship (rows $986e, price scaled by the ship class) | M |
| L76f1e | 1 | Shipyard ship viewer: show one ship for sale with its stats and a rotating 3D model | M |
| L77686 | 2 | Check the job's target is reachable (distance rating vs the rank score) and find a station via L776f8 | M |
| L77f88 | 3 | Remove the picked offer from the system-wide offers list at 9792(a6) (shift later records down) | M |
| L78144 | 2 | Map an id constant (d1) to a small category 12..14 | L |
| L78222 | 11 | Show a station reply dialogue: d0 = $9916, the message text id in a3, then run L76130 (not a classifier) | M |
| L782dc | 4 | Clear the message id of picked board entry d5 so it cannot be taken twice | H |
| L78396 | 1 | Face box / comms screen drawing and "connecting" message | M |
| L783ca | 1 | Face box / comms screen drawing and "connecting" message | M |
| L783f2 | 1 | Face box / comms screen drawing and "connecting" message | M |
| L786c2 | 2 | Station screen: draw header text with position data | L |
| L787b0 | 1 | Draw the 3D rotating face/ship box with lines | L |
| L78998 | 1 | Draw the NPC face (faces module) | H |
| L789c8 | 1 | Draw the face-box frame bitmap | M |
| L7e142 | 2 | Status-screen pages (ship / list pages, titles $9c02..$9c08), one routine per tab | M |
| L7e27a | 1 | Wipe the 3D area (wrapper around N_Wipe3DArea) | H |
| L7e30a | 2 | Status-screen main loop/redraw pieces (page switching, rotating 3D ship preview) | M |
| L7e330 | 1 | Status-screen main loop/redraw pieces (page switching, rotating 3D ship preview) | M |
| L7e684 | 1 | Status-screen main loop/redraw pieces (page switching, rotating 3D ship preview) | M |
| L7e6fc | 1 | Status-screen main loop/redraw pieces (page switching, rotating 3D ship preview) | M |
| L7e788 | 2 | Status page | L |
| L7e806 | 2 | Status-screen pages (ship / list pages, titles $9c02..$9c08), one routine per tab | M |
| L7e8ca | 3 | Look up a rating level from 4 thresholds, returns its text id | M |
| L7e8dc | 2 | Status-screen pages (ship / list pages, titles $9c02..$9c08), one routine per tab | M |
| L7e9ce | 2 | Status-screen pages (ship / list pages, titles $9c02..$9c08), one routine per tab | M |
| L7ea70 | 1 | Status-screen pages (ship / list pages, titles $9c02..$9c08), one routine per tab | M |
| L7eb9c | 2 | Status-screen pages (ship / list pages, titles $9c02..$9c08), one routine per tab | M |
| L7ec04 | 7 | Status-screen header: clear to blue and draw title (text id d0) | M |
| L7ec9e | 3 | Notify module 100(a6) fn 32 with d7 = event kind and d1 = 670(a6) (used by the table-driven event handler at L7ec5c) | M |
| L7ef98 | 4 | Change rating track 828(a6) by d0 (54-entry list), announce tier change | M |
| L7efbc | 4 | Change rating track 830(a6) by d0 (38-entry list), announce tier change | M |
| L7efe0 | 2 | Find tier of score d2 in table L7e8b0, add delta d0 (clamped), return tier change | M |
| L7f084 | 2 | Set achievement bit d4 in 13306(a6), print its message the first time (music 7) | M |
| L7f334 | 1 | Docking resolution of accepted jobs: pay, penalise or drop each job whose origin is this system (starport-economy.md 7.2) | M |
| L7f4f0 | 7 | Advance to the next valid status-screen page (skips empty pages: jobs, contacts) | M |
| L7f54a | 1 | Blit the status-screen tab bitmap | M |
| L7f55c | 5 | Draw a status-screen text row at x/y/width from fields near 734(a6) | L |
| L7f576 | 12 | Draw shadowed formatted text, colour 15 (status-screen helper) | M |
| L80d02 | 1 | Initialise the mission/world slot data from a table | L |
| L80df8 | 2 | Enter or leave a location: sound/music transitions and slot setup | L |
| L80f5c | 1 | Spawn police ship (Lpol_slot / Lpol_spawn) | M |
| L81236 | 1 | Event handler dispatch by code d0 | L |
| L8145e | 2 | Arrive in a new system: shift the 3-entry visited-system history | M |
| L816b6 | 2 | Adjust three coordinates by a table value indexed by 166(a0) | L |
| L816e8 | 2 | Initialise a new NPC record (random, zero velocity) | M |
| L81b9a | 1 | Search the 52-byte record table at 13306 for a free or matching entry | M |
| L81bd2 | 2 | Search the 52-byte record table at 13306 for a free or matching entry | M |
| L81bf2 | 3 | Search the 52-byte record table at 13306 for a free or matching entry | M |
| L83c32 | 3 | Draw the galaxy map star field for current zoom/position | M |
| L83d3c | 2 | Unpack a 3-byte star position into map coordinates relative to map centre | M |
| L83fb2 | 1 | Galaxy-map screen UI: blit panel bitmap and set up/refresh buttons | L |
| L83fb6 | 1 | Galaxy-map screen UI: blit panel bitmap and set up/refresh buttons | L |
| L83fc4 | 2 | Galaxy-map screen UI: blit panel bitmap and set up/refresh buttons | L |
| L83fcc | 3 | Galaxy-map screen UI: blit panel bitmap and set up/refresh buttons | L |
| L83fd0 | 3 | Galaxy-map screen UI: blit panel bitmap and set up/refresh buttons | L |
| L84700 | 7 | Generate a star-system name from its packed coordinates (searches core-systems sector table) | H |
| L84706 | 1 | Entry inside 84700 (name generation after masking d0) | H |
| L84820 | 2 | Copy a string from a small table chosen by 3 bits of d3 (a0 = output) | M |
| L848b0 | 2 | Transform a map coordinate through the inverse view matrix (galaxy map screen position) | M |
| L8490e | 1 | Draw the selected-system detail panel on the galaxy map | M |
| L8497a | 2 | Blit the galaxy-map panel bitmap L8a5a6 at (240,186) to both buffers | H |
| L8497e | 2 | Blit the bitmap in a0 at (240,186) to both buffers (galaxy-map panel) | H |
| L8498a | 10 | Draw a star-system name at screen position (d0..d2) using 84700 for the text | H |
| L849a2 | 3 | Draw the system name/coordinates readout on the galaxy map | M |
| L84ae6 | 2 | Distance between two systems (VectorLen of coordinate deltas) for the map readout | M |
| L84b1a | 2 | Sample the galaxy density map by bilinear interpolation (thunk to L8ad0e, table L8adc0_galaxy_bmp) at map coords d0,d1 | M |
| L84f6e | 4 | Draw the system-info screen (list of the system's bodies) | M |
| L84fa0 | 2 | Draw one body row on the system-info screen, recurses over children | M |
| L84fc8 | 4 | Draw a star/planet bitmap for the system-info screen | H |
| L8a8f0 | 3 | Draw galaxy-map overlay (circles and lines for ranges) | M |
| L8a984 | 2 | DrawLine wrapper that preserves registers | H |
| L8aa48 | 1 | Plot a star dot on the galaxy map (size/colour variants; uses galaxy_color_table) | M |
| L8aabc | 1 | Plot a star dot on the galaxy map (size/colour variants; uses galaxy_color_table) | M |
| L8ab40 | 1 | Plot a star dot on the galaxy map (size/colour variants; uses galaxy_color_table) | M |
| L8abe2 | 4 | PRNG step, same algorithm, separate seed pair (galaxy-map side) | H |
| L8ad08 | 3 | Plot a star dot on the galaxy map (size/colour variants; uses galaxy_color_table) | M |
