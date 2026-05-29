# Affinity Node Reference

Every Visual Script node available in the editor's **Nodes** tab, grouped by category, with what it does and its pins. Auto-generated from `src/editor/frame_loop.cpp` (the `VsNodeType` enum, `sVsNodeDefs` table, and the description tooltips) — if a node is added or its tooltip changes, regenerate this doc so it stays in sync.

**249 nodes total.**

Pin colors in the editor: **green = events**, **blue = actions**, **orange = gates/conditions**, **purple = data/math**. Exec pins are the white triangles that wire the order of operations; data pins are the round colored dots that carry values.

> Many nodes behave differently in **Mode 0** (top-down tile RPG) vs **Mode 4** (first-person 3D). Where a node only applies to one mode, the description says so. See `docs/NODE_CONVENTION.md` for how nodes map to emitted runtime code.

## Categories

- [Events](#events) — 15 nodes
- [Actions](#actions) — 131 nodes
- [Gates / Conditions](#gates--conditions) — 26 nodes
- [Data / Math](#data--math) — 68 nodes
- [Other](#other) — 9 nodes

---

## Events

Green nodes. They have **no exec input** — they are entry points the runtime fires automatically (every frame, on a key, on a collision, etc.) and start a chain.

### On Key Pressed

Fires once when a key is pressed down.

*Pins:* 1 exec out; inputs: `Key`

### On Key Released

Fires once when a key is released.

*Pins:* 1 exec out; inputs: `Key`

### On Key Held

Fires every frame while a key is held.

*Pins:* 1 exec out; inputs: `Key`

### On Collision

Fires when the player collides with an object.

*Pins:* 1 exec out; inputs: `Radius (int)`, `Object`

### On Start

Fires once when the scene starts.

*Pins:* 1 exec out

### On Update

Fires every frame.

*Pins:* 1 exec out

### On Timer

Fires every N frames. Connect an Integer for the interval.

*Pins:* 1 exec out; inputs: `Interval (int)`

### On Death

Event: fires when the specified sprite's HP reaches 0.

*Pins:* 1 exec out; inputs: `Object (int)`

### On Hit

Event: fires when the specified sprite takes damage.

*Pins:* 1 exec out; inputs: `Object (int)`

### On State Enter

Event: fires when the sprite enters the specified state.

*Pins:* 1 exec out; inputs: `Object (int)`, `State (int)`

### On State Exit

Event: fires when the sprite exits the specified state.

*Pins:* 1 exec out; inputs: `Object (int)`, `State (int)`

### On Trigger Enter

Event: fires when a sprite enters a trigger zone.

*Pins:* 1 exec out; inputs: `Obj A (int)`, `Obj B (int)`

### On Trigger Exit

Event: fires when a sprite exits a trigger zone.

*Pins:* 1 exec out; inputs: `Obj A (int)`, `Obj B (int)`

### On Any Key

Event: fires when any button is pressed.

*Pins:* 1 exec out

### On Collision 2D

Event: fires when the player collides with this object in Mode 0 (tilemap).

*Pins:* 1 exec out

---

## Actions

Blue nodes. They take an exec input, **do** something (move the player, set a variable, play a sound), and pass exec on.

### Move Player

Moves the player in a direction while its key is held.

*Pins:* exec in; 1 exec out; inputs: `Direction`

### Look Direction

Sets the player's facing direction.

*Pins:* exec in; 1 exec out; inputs: `Direction`

### Change Scene

Loads a different scene by index.

*Pins:* exec in; 1 exec out; inputs: `Scene (int)`

### Set Variable

Sets a variable slot to a value.

*Pins:* exec in; 1 exec out; inputs: `Var Slot (int)`, `Value`

### Add Variable

Adds an amount to a variable slot.

*Pins:* exec in; 1 exec out; inputs: `Var Slot (int)`, `Amount`

### Play Sound

Plays a sound effect by ID.

*Pins:* exec in; 1 exec out; inputs: `Sound Instance`

### Wait

Pauses execution for a number of frames.

*Pins:* exec in; 1 exec out; inputs: `Frames (int)`

### Jump

Makes the player jump with the given force. Only works when grounded.

*Pins:* exec in; 1 exec out; inputs: `Force (float)`

### Walk

Sets the player's movement speed (walk).

*Pins:* exec in; 1 exec out; inputs: `Speed (int)`

### Sprint

Sets the player's movement speed (sprint).

*Pins:* exec in; 1 exec out; inputs: `Speed (int)`

### Orbit Camera

Rotates the orbit camera in a direction at a speed.

*Pins:* exec in; 1 exec out; inputs: `Direction`, `Speed (int)`

### Play Animation

Plays an animation on the player sprite.

*Pins:* exec in; 1 exec out; inputs: `Anim`

### Set Gravity

Sets gravity strength (pixels per frame^2).

*Pins:* exec in; 1 exec out; inputs: `Value (float)`

### Set Max Fall

Sets the maximum fall speed (terminal velocity).

*Pins:* exec in; 1 exec out; inputs: `Value (float)`

### Destroy Object

Removes a sprite/object from the scene.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`

### Auto Orbit

Enables auto-orbit camera when strafing. 0 = disabled.

*Pins:* exec in; 1 exec out; inputs: `Speed (int)`

### Dampen

Multiplies upward velocity by factor when fired. Use with On Key Released for variable jump height.

*Pins:* exec in; 1 exec out; inputs: `Factor (float)`

### Set Flag

Sets a flag bit (0-31) to a value (0 or 1).

*Pins:* exec in; 1 exec out; inputs: `Flag (int)`, `Value (int)`

### Toggle Flag

Toggles a flag bit (0-31).

*Pins:* exec in; 1 exec out; inputs: `Flag (int)`

### Freeze Player

Disables all player input until UnfreezePlayer is called.

*Pins:* exec in; 1 exec out; inputs: `Blueprint`

### Unfreeze Player

Re-enables player input after FreezePlayer.

*Pins:* exec in; 1 exec out; inputs: `Blueprint`

### Set Cam Height

Sets the camera height above the floor.

*Pins:* exec in; 1 exec out; inputs: `Height (int)`

### Set Horizon

Sets the horizon scanline (0-159). Higher = camera looks down.

*Pins:* exec in; 1 exec out; inputs: `Scanline (int)`

### Teleport

Teleports the player to an absolute X, Y, Z position.

*Pins:* exec in; 1 exec out; inputs: `X (int)`, `Y (int)`, `Z (int)`

### Set Visible

Shows or hides a sprite. 0 = hidden, 1 = visible.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Visible (int)`

### Set Position

Sets a sprite's world position (X, Z).

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `X (int)`, `Z (int)`

### Stop Anim

Stops the current animation playback.

*Pins:* exec in; 1 exec out

### Set Anim Speed

Sets animation playback speed multiplier.

*Pins:* exec in; 1 exec out; inputs: `Speed (int)`

### Set Velocity Y

Sets the player's vertical velocity directly. Works airborne.

*Pins:* exec in; 1 exec out; inputs: `Velocity (float)`

### Stop Sound

Stops all sound channels.

*Pins:* exec in; 1 exec out

### Set Scale

Sets a sprite's scale. 256 = normal size.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Scale (float)`

### Screen Shake

Shakes the camera for N frames at given intensity.

*Pins:* exec in; 1 exec out; inputs: `Intensity (int)`, `Frames (int)`

### Fade Out

Fades the screen to black over N frames.

*Pins:* exec in; 1 exec out; inputs: `Frames (int)`

### Fade In

Fades the screen in from black over N frames.

*Pins:* exec in; 1 exec out; inputs: `Frames (int)`

### Move Toward

Moves a sprite toward another sprite at a speed each frame.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Target (int)`, `Speed (int)`

### Look At

Rotates a sprite to face toward a target sprite.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Target (int)`

### Set Sprite Anim

Sets the animation index on a specific sprite (not just the player).

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Anim (int)`

### Spawn Effect

Triggers a visual effect at a position (effect ID, X, Z).

*Pins:* exec in; 1 exec out; inputs: `Effect (int)`, `X (int)`, `Z (int)`

### Set HP

Sets a sprite's health points to a value.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `HP (int)`

### Damage HP

Subtracts an amount from a sprite's HP. Clamps to 0.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Amount (int)`

### Add Score

Adds an amount to the global score counter.

*Pins:* exec in; 1 exec out; inputs: `Amount (int)`

### Respawn

Resets the player to their starting position and resets velocity.

*Pins:* exec in; 1 exec out

### Save Data

Saves all flags and score to SRAM (persistent across power cycles).

*Pins:* exec in; 1 exec out

### Load Data

Loads flags and score from SRAM.

*Pins:* exec in; 1 exec out

### Flip Sprite

Flips a sprite horizontally. 1 = flipped, 0 = normal.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Flip (int)`

### Set Draw Dist

Changes the mesh draw distance at runtime.

*Pins:* exec in; 1 exec out; inputs: `Distance (int)`

### Enable Collision

Enables or disables collision on a sprite. 0 = off, 1 = on.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Enable (int)`

### Countdown

Decrements an internal counter each trigger. Passes execution when it hits 0, then resets.

*Pins:* exec in; 1 exec out; inputs: `Frames (int)`

### Reset Timer

Resets all countdown/timer counters on this object.

*Pins:* exec in; 1 exec out

### Increment

Adds 1 to a variable slot.

*Pins:* exec in; 1 exec out; inputs: `Var Slot (int)`

### Decrement

Subtracts 1 from a variable slot.

*Pins:* exec in; 1 exec out; inputs: `Var Slot (int)`

### Set FOV

Sets the camera field of view.

*Pins:* exec in; 1 exec out; inputs: `FOV (int)`

### Shake Stop

Immediately stops any screen shake.

*Pins:* exec in; 1 exec out

### Lock Camera

Locks the camera angle (stops orbit/rotation).

*Pins:* exec in; 1 exec out

### Unlock Camera

Unlocks camera rotation.

*Pins:* exec in; 1 exec out

### Set Cam Speed

Sets how fast the camera follows the player.

*Pins:* exec in; 1 exec out; inputs: `Speed (int)`

### Apply Force

Adds a force to the player's velocity (X, Z components).

*Pins:* exec in; 1 exec out; inputs: `X (int)`, `Z (int)`

### Bounce

Reverses the player's Y velocity with damping. Use on landing for bounce pads.

*Pins:* exec in; 1 exec out; inputs: `Damping (float)`

### Set Friction

Sets ground friction (how fast player decelerates). 256 = instant stop.

*Pins:* exec in; 1 exec out; inputs: `Friction (float)`

### Clone Sprite

Duplicates a sprite at its current position.

*Pins:* exec in; 1 exec out; inputs: `Source (int)`

### Hide All

Hides all sprites.

*Pins:* exec in; 1 exec out

### Show All

Shows all sprites.

*Pins:* exec in; 1 exec out

### Set Color

Changes a sprite's palette color index at runtime.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Color (int)`

### Swap Sprite

Swaps a sprite's asset/tileset to a different one at runtime.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Asset (int)`

### Set Sprite Y

Sets a sprite's Y position (height).

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Y (int)`

### Stop All

Stops all running script chains immediately.

*Pins:* exec in

### Set Layer

Sets a sprite's draw priority layer (0=back, 3=front).

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Layer (int)`

### Set Alpha

Sets a sprite's alpha blend level (0=transparent, 16=opaque).

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Alpha (int)`

### Flash

Flashes a sprite white for the specified number of frames.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Frames (int)`

### Set Sprite Scale

Sets the scale of a specific sprite.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Scale (float)`

### Rotate Sprite

Rotates a sprite by the given angle (brads).

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Angle (int)`

### Set HP (Clamped)

Sets HP clamped to max HP (won't exceed maximum).

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `HP (int)`

### Heal HP

Adds HP to a sprite, clamped to max HP.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Amount (int)`

### Set Max HP

Sets a sprite's maximum HP value.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `MaxHP (int)`

### Set BG Color

Sets the background clear color (RGB15).

*Pins:* exec in; 1 exec out; inputs: `Color (int)`

### Face Player

Rotates a sprite to face toward the player.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`

### Move Forward

Moves a sprite forward in its facing direction at the given speed.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Speed (int)`

### Patrol

Moves a sprite back and forth between its start and (X1,Z1).

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `X1 (int)`, `Z1 (int)`, `Speed (int)`

### Chase Player

Moves a sprite toward the player at the given speed.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Speed (int)`

### Flee Player

Moves a sprite away from the player at the given speed.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Speed (int)`

### Set AI

Sets the AI behavior mode for a sprite (0=None, 1=Patrol, 2=Chase, 3=Flee).

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Mode (int)`

### Emit Particle

Spawns a particle effect at the given world position.

*Pins:* exec in; 1 exec out; inputs: `Type (int)`, `X (int)`, `Z (int)`

### Set Tint

Sets a color tint on a sprite (RGB15).

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Color (int)`

### Shake Sprite

Shakes a specific sprite for N frames.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Frames (int)`

### Set Text

Sets a HUD text slot to display a numeric value.

*Pins:* exec in; 1 exec out; inputs: `Slot (int)`, `Value (int)`

### Show HUD

Makes a HUD element slot visible and freezes player movement.

*Pins:* exec in; 1 exec out; inputs: `Slot (int)`

### Hide HUD

Hides a HUD element slot and unfreezes player movement.

*Pins:* exec in; 1 exec out; inputs: `Slot (int)`

### Array Set

Writes a value to the variable array at the given index.

*Pins:* exec in; 1 exec out; inputs: `Index (int)`, `Value (int)`

### Draw Number

Draws an integer value at screen position (X, Y) using tile font.

*Pins:* exec in; 1 exec out; inputs: `Value (int)`, `X (int)`, `Y (int)`

### Draw Text

Draws a predefined text string (by ID) at screen position (X, Y).

*Pins:* exec in; 1 exec out; inputs: `Text ID (int)`, `X (int)`, `Y (int)`

### Clear Text

Clears all rendered text from the screen.

*Pins:* exec in; 1 exec out

### Set Text Color

Sets the color used for subsequent text rendering (RGB15).

*Pins:* exec in; 1 exec out; inputs: `Color (int)`

### Add Item

Adds the given amount to an inventory slot (0-15).

*Pins:* exec in; 1 exec out; inputs: `Slot (int)`, `Amount (int)`

### Remove Item

Removes the given amount from an inventory slot. Won't go below 0.

*Pins:* exec in; 1 exec out; inputs: `Slot (int)`, `Amount (int)`

### Set Item Count

Sets the exact count of items in an inventory slot.

*Pins:* exec in; 1 exec out; inputs: `Slot (int)`, `Count (int)`

### Use Item

Consumes one item from the given inventory slot if available.

*Pins:* exec in; 1 exec out; inputs: `Slot (int)`

### Show Dialogue

Opens a dialogue box displaying the text with the given ID.

*Pins:* exec in; 1 exec out; inputs: `Text ID (int)`

### Hide Dialogue

Closes the currently open dialogue box.

*Pins:* exec in; 1 exec out

### Next Line

Advances the dialogue to the next line of text.

*Pins:* exec in; 1 exec out

### Set Speaker

Sets the current speaker index (controls portrait/name display).

*Pins:* exec in; 1 exec out; inputs: `Speaker (int)`

### Set State

Sets a sprite's state machine to the given state ID.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `State (int)`

### Transition

Changes a sprite's state after an optional delay (in frames).

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `State (int)`, `Delay (int)`

### Set Collision Size

Sets the collision radius of a sprite.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Radius (int)`

### Ignore Collision

Disables collision between two specific sprites.

*Pins:* exec in; 1 exec out; inputs: `Obj A (int)`, `Obj B (int)`

### Spawn At

Spawns a new sprite at the given world position.

*Pins:* exec in; 1 exec out; inputs: `Asset (int)`, `X (int)`, `Z (int)`

### Destroy After

Destroys a sprite after a delay (in frames).

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Frames (int)`

### Spawn Projectile

Spawns a projectile from a sprite in its facing direction.

*Pins:* exec in; 1 exec out; inputs: `Asset (int)`, `X (int)`, `Z (int)`, `Speed (int)`

### Set Lifetime

Sets the remaining lifetime (in frames) for a sprite.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Frames (int)`

### Draw Bar

Draws a horizontal bar at screen position (X, Y) with fill amount.

*Pins:* exec in; 1 exec out; inputs: `X (int)`, `Y (int)`, `Value (int)`, `Max (int)`

### Draw Sprite Icon

Draws a sprite icon at screen position (X, Y).

*Pins:* exec in; 1 exec out; inputs: `Asset (int)`, `X (int)`, `Y (int)`

### Show Timer

Shows the on-screen countdown timer.

*Pins:* exec in; 1 exec out; inputs: `Slot (int)`, `Seconds (int)`

### Hide Timer

Hides the on-screen countdown timer.

*Pins:* exec in; 1 exec out; inputs: `Slot (int)`

### Set Bar Color

Sets the color of a HUD bar (RGB15).

*Pins:* exec in; 1 exec out; inputs: `Color (int)`

### Set Bar Max

Sets the maximum value for a HUD bar.

*Pins:* exec in; 1 exec out; inputs: `Max (int)`

### Reload Scene

Reloads the current scene from scratch.

*Pins:* exec in

### Set Checkpoint

Saves the current player position as a checkpoint.

*Pins:* exec in; 1 exec out

### Load Checkpoint

Teleports the player back to the last saved checkpoint.

*Pins:* exec in; 1 exec out

### Follow Player

Moves a sprite toward the player, stopping at the given distance (default = collision bounds).

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `Distance (int)`, `Speed (int)`

### Set Follow Facing

Sets the follow object's facing direction from its movement direction (Mode 0).

*Pins:* exec in; 1 exec out

### Play Hud Anim

Starts a HUD animation layer (resets frame to 0).

*Pins:* exec in; 1 exec out

### Stop Hud Anim

Stops a HUD animation layer.

*Pins:* exec in; 1 exec out

### Set Anim Speed

Sets the tick speed of a HUD animation layer (1=fastest, higher=slower).

*Pins:* exec in; 1 exec out

### Reset Scene

Reloads the current scene. Player respawns at start position.

*Pins:* exec in; 1 exec out

### Set Player Height

Sets the player collision height (pixels). Controls wall Y-overlap and floor snap-up distance. Default 12.

*Pins:* exec in; 1 exec out; inputs: `Value (float)`

### Set HUD Value

Action: sets afn_hud_value[slot] so a HUD counter element displays the given number.

*Pins:* exec in; 1 exec out; inputs: `Value`, `Slot`

### Update Respawn Pos

Updates the Respawn start position to the given object's world position. Use on checkpoint trigger.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`

### Set Velocity X

Adds a world-X velocity to the player every frame, independent of input. Pair with Velocity Falloff to decay it (boost pads, knockback, wind).

*Pins:* exec in; 1 exec out; inputs: `Velocity (float)`

### Set Velocity Z

Adds a world-Z velocity to the player every frame, independent of input. Pair with Velocity Falloff to decay it.

*Pins:* exec in; 1 exec out; inputs: `Velocity (float)`

### Velocity Falloff

Linearly decays current vx/vz to 0 over N frames. Set after Set Velocity X/Z to define how quickly the boost/push fades.

*Pins:* exec in; 1 exec out; inputs: `Frames (int)`

### Boost Forward

Pushes the player along their current view direction at the given speed. Runtime decomposes into world vx/vz using sin/cos(viewAngle). Pair with Velocity Falloff to decay.

*Pins:* exec in; 1 exec out; inputs: `Speed (float)`

### Halt Momentum

Zeros all player velocity (vx, vz, vy, pending boost, falloff). Use after respawn so leftover boost-pad momentum doesn't carry over.

*Pins:* exec in; 1 exec out

---

## Gates / Conditions

Orange nodes. They take exec in and only pass it on if a condition holds — e.g. `IsOnGround` only continues the chain when the player is grounded.

### Branch

If condition is true, execute True path; otherwise False.

*Pins:* exec in; exec out: `True`, `False`; inputs: `Condition`

### Compare Var

Compares a variable slot against a value. Outputs 1 or 0.

*Pins:* inputs: `Var Slot`, `Value`; outputs: `Result`

### Is Moving

Gate: only passes execution through if the player is currently moving (d-pad held).

*Pins:* exec in; 1 exec out

### Is On Ground

Gate: only passes execution through if the player is on the ground.

*Pins:* exec in; 1 exec out

### Is Jumping

Gate: only passes execution through if the player is airborne and rising.

*Pins:* exec in; 1 exec out

### Is Falling

Gate: only passes execution through if the player is airborne and falling.

*Pins:* exec in; 1 exec out

### Check Flag

Branches on whether a flag (0-31) is set or clear.

*Pins:* exec in; exec out: `Set`, `Clear`; inputs: `Flag (int)`

### Do Once

Only passes execution through once. Subsequent triggers are ignored.

*Pins:* exec in; 1 exec out

### Flip Flop

Alternates between exec output A and B each time triggered.

*Pins:* exec in; exec out: `A`, `B`

### Gate

Passes execution only if the Open input is nonzero. 0 = blocked.

*Pins:* exec in; 1 exec out; inputs: `Open (int)`

### For Loop

Executes the downstream chain Count times in a row.

*Pins:* exec in; 1 exec out; inputs: `Count (int)`

### Sequence

Fires Then 0, then Then 1 in order each trigger.

*Pins:* exec in; exec out: `Then 0`, `Then 1`

### Is Flag Set

Gate: only passes execution if the specified flag bit is set.

*Pins:* exec in; 1 exec out; inputs: `Flag (int)`

### Is HP Zero

Gate: only passes execution if the sprite's HP is zero.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`

### Is Near

Gate: only passes execution if two sprites are within the given radius.

*Pins:* exec in; 1 exec out; inputs: `Obj A (int)`, `Obj B (int)`, `Radius (int)`

### Wait Until

Blocks execution until the connected condition becomes true.

*Pins:* exec in; 1 exec out; inputs: `Condition`

### Repeat While

Keeps firing downstream every frame while condition is true.

*Pins:* exec in; 1 exec out; inputs: `Condition`

### Delay

Delays downstream execution by N frames (non-blocking).

*Pins:* exec in; 1 exec out; inputs: `Frames (int)`

### Is Alive

Gate: only passes execution if the sprite's HP is greater than 0.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`

### Has Item

Gate: only passes execution if the inventory slot has count > 0.

*Pins:* exec in; 1 exec out; inputs: `Slot (int)`

### Is Dialogue Open

Gate: only passes execution if a dialogue box is currently open.

*Pins:* exec in; 1 exec out

### Dialogue Choice

Presents two choices to the player. Branches to A or B based on selection.

*Pins:* exec in; exec out: `A`, `B`; inputs: `Choice A (int)`, `Choice B (int)`

### Is In State

Gate: only passes execution if the sprite is in the given state.

*Pins:* exec in; 1 exec out; inputs: `Object (int)`, `State (int)`

### Is Colliding

Gate: only passes execution if two sprites are overlapping.

*Pins:* exec in; 1 exec out; inputs: `Obj A (int)`, `Obj B (int)`

### Is True

Gate: only passes execution through if the data input is non-zero (true).

*Pins:* exec in; 1 exec out; inputs: `Value (int)`

### On Rise

Rising-edge gate: only passes execution on the first frame the upstream condition becomes true. Blocks while it keeps firing. Resets when it stops.

*Pins:* exec in; 1 exec out

---

## Data / Math

Purple nodes. They have **no exec pins**; they output a value that's evaluated inline wherever it's wired (into another node's data input).

### Integer

Outputs a constant integer value.

*Pins:* outputs: `Out`

### Key

Outputs a key constant (A, B, L, R, etc).

*Pins:* outputs: `Out`

### Direction

Outputs a direction (Left, Right, Up, Down).

*Pins:* outputs: `Out`

### Animation

Outputs an animation index.

*Pins:* outputs: `Out`

### Float

Outputs a constant float value.

*Pins:* outputs: `Out`

### Object

Data node: outputs a constant object/sprite index, chosen from a dropdown of placed sprites.

*Pins:* outputs: `Out`

### Add

Outputs A + B.

*Pins:* inputs: `A`, `B`; outputs: `Result`

### Subtract

Outputs A - B.

*Pins:* inputs: `A`, `B`; outputs: `Result`

### Multiply

Outputs (A * B) >> 8 (fixed-point multiply).

*Pins:* inputs: `A`, `B`; outputs: `Result`

### Negate

Outputs -Value.

*Pins:* inputs: `Value`; outputs: `Result`

### Random Int

Outputs a random integer between Min and Max (inclusive).

*Pins:* inputs: `Min`, `Max`; outputs: `Result`

### Get Flag

Reads a flag bit (0-31). Outputs 1 if set, 0 if clear.

*Pins:* inputs: `Flag (int)`; outputs: `Value`

### Abs

Outputs the absolute value of the input.

*Pins:* inputs: `Value`; outputs: `Result`

### Min

Outputs the smaller of A and B.

*Pins:* inputs: `A`, `B`; outputs: `Result`

### Max

Outputs the larger of A and B.

*Pins:* inputs: `A`, `B`; outputs: `Result`

### Modulo

Outputs A modulo B (remainder).

*Pins:* inputs: `A`, `B`; outputs: `Result`

### Clamp

Clamps Value between Min and Max.

*Pins:* inputs: `Value`, `Min`, `Max`; outputs: `Result`

### Sign

Outputs -1 if negative, 0 if zero, 1 if positive.

*Pins:* inputs: `Value`; outputs: `Result`

### Compare

Compares A and B. paramInt[0] selects operator: 0=Equal, 1=NotEqual, 2=Less, 3=Greater, 4=LessEq, 5=GreaterEq.

*Pins:* inputs: `A`, `B`; outputs: `Result`

### AND

Outputs 1 if both A and B are nonzero.

*Pins:* inputs: `A`, `B`; outputs: `Result`

### OR

Outputs 1 if either A or B is nonzero.

*Pins:* inputs: `A`, `B`; outputs: `Result`

### NOT

Outputs 1 if Value is zero, 0 otherwise.

*Pins:* inputs: `Value`; outputs: `Result`

### Get Variable

Reads a variable slot and outputs its value.

*Pins:* inputs: `Var Slot (int)`; outputs: `Value`

### Get Player X

Outputs the player's current X position.

*Pins:* outputs: `X`

### Get Player Z

Outputs the player's current Z position.

*Pins:* outputs: `Z`

### Select

If Cond is nonzero, outputs A. Otherwise outputs B.

*Pins:* inputs: `Cond`, `A`, `B`; outputs: `Result`

### Lerp

Linearly interpolates from A to B by T (0-256 = 0.0-1.0 fixed-point).

*Pins:* inputs: `A`, `B`, `T`; outputs: `Result`

### Distance

Outputs the approximate distance between two sprites.

*Pins:* inputs: `Obj A (int)`, `Obj B (int)`; outputs: `Dist`

### Get Sprite X

Reads a sprite's X world position.

*Pins:* inputs: `Object (int)`; outputs: `X`

### Get Sprite Z

Reads a sprite's Z world position.

*Pins:* inputs: `Object (int)`; outputs: `Z`

### Is Key Down

Outputs 1 if the connected key is currently held, 0 otherwise.

*Pins:* inputs: `Key`; outputs: `Held`

### Sin Wave

Outputs an oscillating value: amplitude * sin(frame * 2pi / period).

*Pins:* inputs: `Amplitude`, `Period`; outputs: `Value`

### Get Time

Outputs the current frame counter (increments every frame).

*Pins:* outputs: `Frames`

### Get HP

Reads a sprite's current health points.

*Pins:* inputs: `Object (int)`; outputs: `HP`

### Get Score

Outputs the current score.

*Pins:* outputs: `Score`

### Divide

Outputs A / B (integer division). Returns 0 if B is 0.

*Pins:* inputs: `A`, `B`; outputs: `Result`

### Power

Outputs Base raised to the Exp power (integer).

*Pins:* inputs: `Base`, `Exp`; outputs: `Result`

### Remap

Remaps Value from [0, InMax] to [0, OutMax].

*Pins:* inputs: `Value`, `In Max`, `Out Max`; outputs: `Result`

### Average

Outputs (A + B) / 2.

*Pins:* inputs: `A`, `B`; outputs: `Result`

### Is Alive

Outputs 1 if sprite's HP > 0 (alive), 0 if dead.

*Pins:* inputs: `Object (int)`; outputs: `Alive`

### Ping Pong

Outputs a value that bounces between 0 and Range based on time and Speed.

*Pins:* inputs: `Range`, `Speed`; outputs: `Value`

### Get Angle

Outputs the angle (in brads) between two sprites.

*Pins:* inputs: `Obj A (int)`, `Obj B (int)`; outputs: `Angle`

### Get Player Y

Outputs the player's Y position (height).

*Pins:* outputs: `Y`

### Get Sprite Y

Outputs a sprite's Y position (height).

*Pins:* inputs: `Object (int)`; outputs: `Y`

### Get Layer

Outputs a sprite's current draw priority layer.

*Pins:* inputs: `Object (int)`; outputs: `Layer`

### Get Velocity Y

Outputs the player's current vertical velocity.

*Pins:* outputs: `VelY`

### Get Rotation

Outputs a sprite's current rotation angle.

*Pins:* inputs: `Object (int)`; outputs: `Angle`

### Get Max HP

Outputs a sprite's maximum HP value.

*Pins:* inputs: `Object (int)`; outputs: `MaxHP`

### Get Delta Time

Outputs frame delta time (always 1 at fixed 60fps).

*Pins:* outputs: `DT`

### Map Value

Maps Value from [InMin, InMax] to [OutMin, OutMax].

*Pins:* inputs: `Value`, `In Min`, `In Max`, `Out Min`; outputs: `Result`

### Wrap

Wraps Value to stay within [Min, Max] range.

*Pins:* inputs: `Value`, `Min`, `Max`; outputs: `Result`

### Smooth Step

Hermite smoothstep interpolation between A and B by T.

*Pins:* inputs: `A`, `B`, `T`; outputs: `Result`

### Ease In

Quadratic ease-in: T*T (0-256 range).

*Pins:* inputs: `T`; outputs: `Result`

### Ease Out

Quadratic ease-out: 1-(1-T)^2 (0-256 range).

*Pins:* inputs: `T`; outputs: `Result`

### Dist To Player

Outputs the distance from a sprite to the player.

*Pins:* inputs: `Object (int)`; outputs: `Dist`

### Get AI

Outputs the current AI behavior mode of a sprite.

*Pins:* inputs: `Object (int)`; outputs: `Mode`

### Get Random

Outputs a random value between 0 and 255 (fixed-point 0.0-1.0).

*Pins:* outputs: `Value`

### Array Get

Reads from the variable array at the given index.

*Pins:* inputs: `Index (int)`; outputs: `Value`

### Get Item Count

Outputs the quantity of items in the given inventory slot.

*Pins:* inputs: `Slot (int)`; outputs: `Count`

### Get State

Outputs a sprite's current state machine state ID.

*Pins:* inputs: `Object (int)`; outputs: `State`

### Prev State

Outputs the sprite's previous state (before last transition).

*Pins:* inputs: `Object (int)`; outputs: `State`

### State Timer

Outputs how many frames the sprite has been in its current state.

*Pins:* inputs: `Object (int)`; outputs: `Frames`

### Get Lifetime

Outputs the remaining lifetime of a sprite.

*Pins:* inputs: `Object (int)`; outputs: `Frames`

### Get Scene

Outputs the current scene index.

*Pins:* outputs: `Scene`

### Get Input Axis

Outputs the current D-pad axis value (-256 to 256).

*Pins:* inputs: `Axis (int)`; outputs: `Value`

### Get Last Key

Outputs the key code of the last button pressed.

*Pins:* outputs: `Key`

### Get Cursor Stop

Returns the current cursor stop index (0-based).

*Pins:* outputs: `Stop`

### Blueprint

Outputs a blueprint definition index. Use with Freeze/Unfreeze Player to disable a specific blueprint.

*Pins:* outputs: `Out`

---

## Other

Nodes that don't fall into the four main colors (structural / flow helpers).

### Group

Groups nodes into a reusable subgraph.

*Pins:* no pins

### Custom Code

Action: a user-written block of C code with custom input/output pins. The code is emitted verbatim into the runtime.

*Pins:* exec in; 1 exec out

### Print

Debug: prints an integer value to mGBA log output.

*Pins:* exec in; 1 exec out; inputs: `Value (int)`

### Cursor Up

Moves the element's cursor to the previous stop (wraps to last).

*Pins:* exec in; 1 exec out

### Cursor Down

Moves the element's cursor to the next stop (wraps to first).

*Pins:* exec in; 1 exec out

### Follow Link

Navigates to the linked element at the current cursor stop.

*Pins:* exec in; 1 exec out

### Is Near 2D

Gate: passes exec only if the player is currently adjacent to this blueprint's tilemap object (Mode 0).

*Pins:* exec in; 1 exec out

### Is Follow Moving

Gate: passes exec only if the follow object is currently moving between tiles (Mode 0).

*Pins:* exec in; 1 exec out

### Sound Instance

Outputs a sound instance index for PlaySound.

*Pins:* 1 output(s)

---
