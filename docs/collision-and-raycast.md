# Collision tests and ray casting against ship hulls

The block `L3fa8e` to `L416a0` in `fe2/fe2.s`, which `unnamed-functions.md` listed as "3D model face drawing / lighting steps",
is not the renderer. It is the **collision and ray-cast code** that works on a model's *hull* data. Tags: **[code]** read from the
disassembly, not run; **[inferred]** a conclusion that could be wrong.

## 1. Entry points

[code] `L3f460` (786(a5), 'collision test of this ship against everything'; `L3f45a` is the same without damage) first handles the parent frame, then
checks the ship against each object. Two helpers copy a ship between the world frame and its parent's local frame: `L3fe56` and `L4011c`, with `L3f566` as the
proximity distance. The hit test of one object against a point is `L3f8fe`, which calls `L3fa8e`. The result flags live in `16014..16020(a6)`, the hit object in
`16024(a6)`; damage is done through the hooks `Lpol_dmg1` and `Lpol_dmg2`.

`L409fc` is `822(a5)`, the **ray cast** used by laser beams (`L45dc8`) and by a caller at line 25239 of `fe2.s` (not identified); `L4097e` is `826(a5)`.
The ray state lives in `16032..16312(a6)`.

## 2. Collision hit test: `L3fa8e`, `L3fba6`, `L3fc9e`, `L3fd6a`

[code] The model's hull data sits in the same game-data block as the drawing data (`A6_game_data[90(a1)]`): `6(a5)` points at the vertex-like table, `2(a5)` at a
second table, `10(a5)` is a scale exponent, `26(a5)` the start of the hull bytecode. `L3fa8e` scales the test point to the model's size
(shifting by a common exponent, rotating by the object matrix unless the ship is docked, `69(a0)`), then `L3fba6` interprets the bytecode:

- A record starts with a flag byte `d0` and a second byte. Bit 5 makes it conditional on a bit of the object state words (`114(a1, 2 * n)`), as in the drawing
  bytecode. Bit 6 and bit 7 select the record shapes (list of plane indices, or a reference to a nested part).
- Plane lists run through `L3fd6a`, which dots the point (relative to one of the model's points) with a packed normal and compares it with the margin `d7`.
- A nested part (`L3fc9e`) carries a 16-bit offset to another hull, a packed axis code (low three bits pick a permutation from `L3c47e`, three more bits negate;
  `L402ce` applies the same code to three words) and a base point index.

The return is `d0 = 1` clear of all parts, `-1` or `0` hit, with the part's bits OR-ed into `16016(a6)` (the docking code in npc-ship-ai reads bits 6 and 5 of this result).

## 3. Ray cast: `L409fc` and friends

[code] `L409fc` stores the ray (start `d0..d2`, direction `d3..d5`, flags in `a2`, range `d7/a3`, an object index in `d6` that is skipped) and walks the object table (bit 2 of each flag byte,
object indices from `$72` down), skipping the shooter. For each candidate it moves the ray into the object's frame (`L405dc`/`L40726`, `L40888`), rejects by bounding
sphere (`L4110e`) and then clips against the hull (`L413e0`). The clipping is a ray/convex-solid intersection: `L41628` and `L41668` clip against the plane list of one part
(`L416a0` fetches each plane and the ray's dot product), keeping the largest entering and smallest leaving parameters, and `L40314`/`L4048e`/`L40504` keep and merge the
resulting intervals in a list at `a4` (nodes: `+30` parameter, `+32` next, `+34` flags). `L41374` picks the nearest interval start and the plane normal that gave it.
`L40e02`, `L40e4c`, `L40f24`, `L40fbe` and `L4102a` convert the hit to a point and normal (scaling by `16236(a6)`).

[inferred] The names 'interval list' and 'Cyrus-Beck' are mine: the code shows min/max parameter updates per plane and a linked list of parameter values, which is that algorithm.

## 4. What this means for the host code

[inferred] The native scene renderer does not use this data. The hull records share the layout conventions of the drawing bytecode (conditional records on the state words, 0-terminated lists), but I did not check
whether `tools/fe2ShipBuilder/fe2core.cpp` can read them. The hull is a union of convex parts with its own planes, so it can differ from the drawn shape.

## 5. Not traced

- The tail of the post-processing (`L40f24`, `L40fbe`, `L4102a`, `L411ea`, `L41374`) beyond what is listed above.
- The exact meaning of the `16016(a6)` bits and the `L4097e` continuation.
