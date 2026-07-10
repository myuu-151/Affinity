# Affinity Node Reference

Every Visual Script node available in the editor's **Nodes** tab, grouped by category, with what it does and its pins. Auto-generated from `src/editor/frame_loop.cpp` (the `VsNodeType` enum, `sVsNodeDefs` table, and the description tooltips) — regenerate with `python tools/regen_node_reference.py` whenever nodes or pins change.

**370 nodes total.**

Pin colors in the editor: **green = events**, **blue = actions**, **orange = gates/conditions**, **purple = data/math**. Exec pins are the white triangles that wire the order of operations; data pins are the round colored dots that carry values.

> Many nodes behave differently in **Mode 0** (top-down tile RPG) vs **Mode 4** (first-person 3D). Where a node only applies to one mode, the description says so. See `docs/NODE_CONVENTION.md` for how nodes map to emitted runtime code.

## Categories

- [Events](#events) — 15 nodes
- [Actions](#actions) — 221 nodes
- [Gates / Conditions](#gates--conditions) — 56 nodes
- [Data / Math](#data--math) — 78 nodes

---

## Events

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

Event: fires the frame an object takes damage. Object pin wired = that sprite; unwired = the player or anyone.

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

### Move Player

Moves the player in the given Direction whenever this runs. Wire it after On Key Held(key) to bind movement to that button — e.g. On Key Held(A) -> Move Player(Up) moves up while A is held (remappable). Left-click for the Facing switch: Direction Facing (rig turns toward movement) or Consistent Facing (rig keeps its yaw — strafe/moonwalk).

*Pins:* 1 exec in, 1 exec out; inputs: `Direction`

### Look Direction

Sets the player's facing direction.

*Pins:* 1 exec in, 1 exec out; inputs: `Direction`

### Change Scene

Loads a different scene by index. Delay (frames) holds the current scene before switching; Transition (frames) sets the fade/crossfade duration (default 15) — raise it (e.g. 45-60) to slow a piece Crossfade.

*Pins:* 1 exec in, 1 exec out; inputs: `Scene (int)`, `Delay (frames)`, `Transition (frames)`

### Set Variable

Sets a variable slot to a value.

*Pins:* 1 exec in, 1 exec out; inputs: `Var Slot (int)`, `Value`

### Add Variable

Adds an amount to a variable slot.

*Pins:* 1 exec in, 1 exec out; inputs: `Var Slot (int)`, `Amount`

### Play Sound

Plays a sound instance. Persist Link >0 keeps it alive across scene changes, sharing music only between scenes that play it with the same link; a different link swaps the held track.

*Pins:* 1 exec in, 1 exec out; inputs: `Sound Instance`, `Persist Link`

### Wait

Pauses execution for a number of frames.

*Pins:* 1 exec in, 1 exec out; inputs: `Frames (int)`

### Jump

Makes the player jump with the given Force. Only works when grounded. Fall Force (PSV, optional) adds EXTRA downward acceleration once you're past the apex, for a heavier fall than the rise (unlike Set Max Fall, which only caps fall speed; they combine). Two node-body sliders (PSV) shape an 'anime' arc: Rise Float % reduces gravity WHILE RISING so you float up and hang at the apex (0 = normal, higher = floatier/longer hang); Fall Smooth (frames) eases the Fall Force in over N descent frames instead of snapping it on at the apex (0 = instant). Float up + hang + heavy fall = anime jump. Energy Cost (int, optional): when > 0 the jump only fires if afn_energy >= cost and spends it on launch (0/unwired = free).

*Pins:* 1 exec in, 1 exec out; inputs: `Force (float)`, `Fall Force (float)`, `Energy Cost (int)`

### Walk

Sets the player's movement speed (walk).

*Pins:* 1 exec in, 1 exec out; inputs: `Speed (int)`

### Sprint

Sets the player's movement speed (sprint).

*Pins:* 1 exec in, 1 exec out; inputs: `Speed (int)`

### Orbit Camera

Rotates the orbit camera in a direction at a speed whenever this runs. Gate it with On Key Held(key) to bind orbiting to a button (any key, remappable).

*Pins:* 1 exec in, 1 exec out; inputs: `Direction`, `Speed (int)`

### Play Animation

Plays an animation on the player sprite.

*Pins:* 1 exec in, 1 exec out; inputs: `Anim`

### Set Gravity

Sets gravity strength (pixels per frame^2).

*Pins:* 1 exec in, 1 exec out; inputs: `Value (float)`

### Set Max Fall

Sets the maximum fall speed (terminal velocity).

*Pins:* 1 exec in, 1 exec out; inputs: `Value (float)`

### Destroy Object

Removes a sprite/object from the scene.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`

### Auto Orbit

Enables auto-orbit camera when strafing. 0 = disabled.

*Pins:* 1 exec in, 1 exec out; inputs: `Speed (int)`

### Dampen

Multiplies upward velocity by factor when fired. Use with On Key Released for variable jump height.

*Pins:* 1 exec in, 1 exec out; inputs: `Factor (float)`

### Custom Code

_(see in-editor tooltip)_

*Pins:* 1 exec in, 1 exec out

### Set Flag

Sets a flag bit (0-31) to a value (0 or 1).

*Pins:* 1 exec in, 1 exec out; inputs: `Flag (int)`, `Value (int)`

### Toggle Flag

Toggles a flag bit (0-31).

*Pins:* 1 exec in, 1 exec out; inputs: `Flag (int)`

### Freeze Player

Disables all player input until UnfreezePlayer is called.

*Pins:* 1 exec in, 1 exec out; inputs: `Blueprint`

### Unfreeze Player

Re-enables player input after FreezePlayer.

*Pins:* 1 exec in, 1 exec out; inputs: `Blueprint`

### Set Cam Height

Sets the camera height above the floor.

*Pins:* 1 exec in, 1 exec out; inputs: `Height (int)`

### Set Horizon

Sets the horizon scanline (0-159). Higher = camera looks down.

*Pins:* 1 exec in, 1 exec out; inputs: `Scanline (int)`

### Teleport

Teleports the player to an absolute X, Y, Z position.

*Pins:* 1 exec in, 1 exec out; inputs: `X (int)`, `Y (int)`, `Z (int)`

### Set Visible

Shows or hides a sprite. 0 = hidden, 1 = visible.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Visible (int)`

### Set Position

Sets a sprite's world position (X, Z).

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `X (int)`, `Z (int)`

### Stop Anim

Stops the current animation playback.

*Pins:* 1 exec in, 1 exec out

### Set Anim Speed

Sets animation playback speed multiplier.

*Pins:* 1 exec in, 1 exec out; inputs: `Speed (int)`

### Set Velocity Y

Sets the player's vertical velocity directly. Works airborne.

*Pins:* 1 exec in, 1 exec out; inputs: `Velocity (float)`

### Stop Sound

Stops all sound channels.

*Pins:* 1 exec in, 1 exec out; inputs: `Sound Instance`

### Set Scale

Sets a sprite's scale. 256 = normal size.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Scale (float)`

### Screen Shake

Shakes the camera for N frames at given intensity.

*Pins:* 1 exec in, 1 exec out; inputs: `Intensity (int)`, `Frames (int)`

### Fade Out

Fades the screen to black over N frames.

*Pins:* 1 exec in, 1 exec out; inputs: `Frames (int)`

### Fade In

Fades the screen in from black over N frames.

*Pins:* 1 exec in, 1 exec out; inputs: `Frames (int)`

### Move Toward

Moves a sprite toward another sprite at a speed each frame.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Target (int)`, `Speed (int)`

### Look At

Rotates a sprite to face toward a target sprite.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Target (int)`

### Set Sprite Anim

Sets the animation index on a specific sprite (not just the player).

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Anim (int)`

### Spawn Effect

Triggers a visual effect at a position (effect ID, X, Z).

*Pins:* 1 exec in, 1 exec out; inputs: `Effect (int)`, `X (int)`, `Z (int)`

### Set HP

Sets a sprite's health points to a value.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `HP (int)`

### Damage HP

Subtracts an amount from a sprite's HP. Clamps to 0.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Amount (int)`

### Add Score

Adds an amount to the global score counter.

*Pins:* 1 exec in, 1 exec out; inputs: `Amount (int)`

### Respawn

Resets the player to their starting position and resets velocity.

*Pins:* 1 exec in, 1 exec out

### Save Data

Saves all flags and score to SRAM (persistent across power cycles).

*Pins:* 1 exec in, 1 exec out

### Load Data

Loads flags and score from SRAM.

*Pins:* 1 exec in, 1 exec out

### Flip Sprite

Flips a sprite horizontally. 1 = flipped, 0 = normal.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Flip (int)`

### Set Draw Dist

Changes the mesh draw distance at runtime.

*Pins:* 1 exec in, 1 exec out; inputs: `Distance (int)`

### Enable Collision

Enables or disables collision on a sprite. 0 = off, 1 = on.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Enable (int)`

### Countdown

Decrements an internal counter each trigger. Passes execution when it hits 0, then resets.

*Pins:* 1 exec in, 1 exec out; inputs: `Frames (int)`

### Reset Timer

Resets all countdown/timer counters on this object.

*Pins:* 1 exec in, 1 exec out

### Increment

Adds 1 to a variable slot.

*Pins:* 1 exec in, 1 exec out; inputs: `Var Slot (int)`

### Decrement

Subtracts 1 from a variable slot.

*Pins:* 1 exec in, 1 exec out; inputs: `Var Slot (int)`

### Set FOV

Sets the camera field of view.

*Pins:* 1 exec in, 1 exec out; inputs: `FOV (int)`

### Shake Stop

Immediately stops any screen shake.

*Pins:* 1 exec in, 1 exec out

### Lock Camera

Locks the camera angle (stops orbit/rotation).

*Pins:* 1 exec in, 1 exec out

### Unlock Camera

Unlocks camera rotation.

*Pins:* 1 exec in, 1 exec out

### Set Cam Speed

Sets how fast the camera follows the player.

*Pins:* 1 exec in, 1 exec out; inputs: `Speed (int)`

### Apply Force

Adds a force to the player's velocity (X, Z components).

*Pins:* 1 exec in, 1 exec out; inputs: `X (int)`, `Z (int)`

### Bounce

Reverses the player's Y velocity with damping. Use on landing for bounce pads.

*Pins:* 1 exec in, 1 exec out; inputs: `Damping (float)`

### Set Friction

Sets ground friction (how fast player decelerates). 256 = instant stop.

*Pins:* 1 exec in, 1 exec out; inputs: `Friction (float)`

### Clone Sprite

Duplicates a sprite at its current position.

*Pins:* 1 exec in, 1 exec out; inputs: `Source (int)`

### Hide All

Hides all sprites.

*Pins:* 1 exec in, 1 exec out

### Show All

Shows all sprites.

*Pins:* 1 exec in, 1 exec out

### Print

Debug: prints an integer value to mGBA log output.

*Pins:* 1 exec in, 1 exec out; inputs: `Value (int)`

### Set Color

Changes a sprite's palette color index at runtime.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Color (int)`

### Swap Sprite

Swaps a sprite's asset/tileset to a different one at runtime.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Asset (int)`

### Set Sprite Y

Sets a sprite's Y position (height).

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Y (int)`

### Stop All

Stops all running script chains immediately.

*Pins:* 1 exec in

### Set Layer

Sets a sprite's draw priority layer (0=back, 3=front).

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Layer (int)`

### Set Alpha

Sets a sprite's alpha blend level (0=transparent, 16=opaque).

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Alpha (int)`

### Flash

Flashes a sprite white for the specified number of frames.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Frames (int)`

### Set Sprite Scale

Sets the scale of a specific sprite.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Scale (float)`

### Rotate Sprite

Rotates a sprite by the given angle (brads).

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Angle (int)`

### Set HP (Clamped)

Sets HP clamped to max HP (won't exceed maximum).

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `HP (int)`

### Heal HP

Adds HP to a sprite, clamped to max HP.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Amount (int)`

### Set Max HP

Sets a sprite's maximum HP value.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `MaxHP (int)`

### Set BG Color

Sets the background clear color (RGB15).

*Pins:* 1 exec in, 1 exec out; inputs: `Color (int)`

### Face Player

Rotates a sprite to face toward the player.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`

### Move Forward

Moves a sprite forward in its facing direction at the given speed.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Speed (int)`

### Patrol

Moves a sprite back and forth between its start and (X1,Z1).

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `X1 (int)`, `Z1 (int)`, `Speed (int)`

### Chase Player

Moves a sprite toward the player at the given speed.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Speed (int)`

### Flee Player

Moves a sprite away from the player at the given speed.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Speed (int)`

### Set AI

Sets the AI behavior mode for a sprite (0=None, 1=Patrol, 2=Chase, 3=Flee).

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Mode (int)`

### Emit Particle

Spawns a particle effect at the given world position.

*Pins:* 1 exec in, 1 exec out; inputs: `Type (int)`, `X (int)`, `Z (int)`

### Set Tint

Sets a color tint on a sprite (RGB15).

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Color (int)`

### Shake Sprite

Shakes a specific sprite for N frames.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Frames (int)`

### Set Text

Sets a HUD text slot to display a numeric value.

*Pins:* 1 exec in, 1 exec out; inputs: `Slot (int)`, `Value (int)`

### Show HUD

Makes a HUD element visible (afn_hud_visible[slot]=1). Slot = the element's index in the Elements list (top = 0). Only freezes the player if the element has cursor stops (a menu) — plain gameplay elements (bars, icons, effects) just appear. The companion to hiding an element in the editor (Visible off) so it starts off-screen and is revealed by a node, e.g. On Start -> Show HUD for a persistent bar.

*Pins:* 1 exec in, 1 exec out; inputs: `Slot (int)`, `Anchor (obj)`

### Hide HUD

Hides a HUD element (afn_hud_visible[slot]=0) and clears the menu freeze (unfreezes the player). Slot = the element's index in the Elements list.

*Pins:* 1 exec in, 1 exec out; inputs: `Slot (int)`

### Array Set

Writes a value to the variable array at the given index.

*Pins:* 1 exec in, 1 exec out; inputs: `Index (int)`, `Value (int)`

### Draw Number

Draws an integer value at screen position (X, Y) using tile font.

*Pins:* 1 exec in, 1 exec out; inputs: `Value (int)`, `X (int)`, `Y (int)`

### Draw Text

Draws a predefined text string (by ID) at screen position (X, Y).

*Pins:* 1 exec in, 1 exec out; inputs: `Text ID (int)`, `X (int)`, `Y (int)`

### Clear Text

Clears all rendered text from the screen.

*Pins:* 1 exec in, 1 exec out

### Set Text Color

Sets the color used for subsequent text rendering (RGB15).

*Pins:* 1 exec in, 1 exec out; inputs: `Color (int)`

### Add Item

Adds the given amount to an inventory slot (0-15).

*Pins:* 1 exec in, 1 exec out; inputs: `Slot (int)`, `Amount (int)`

### Remove Item

Removes the given amount from an inventory slot. Won't go below 0.

*Pins:* 1 exec in, 1 exec out; inputs: `Slot (int)`, `Amount (int)`

### Set Item Count

Sets the exact count of items in an inventory slot.

*Pins:* 1 exec in, 1 exec out; inputs: `Slot (int)`, `Count (int)`

### Use Item

Consumes one item from the given inventory slot if available.

*Pins:* 1 exec in, 1 exec out; inputs: `Slot (int)`

### Show Dialogue

Opens a dialogue box displaying the text with the given ID.

*Pins:* 1 exec in, 1 exec out; inputs: `Text ID (int)`

### Hide Dialogue

Closes the currently open dialogue box.

*Pins:* 1 exec in, 1 exec out

### Next Line

Advances the dialogue to the next line of text.

*Pins:* 1 exec in, 1 exec out

### Set Speaker

Sets the current speaker index (controls portrait/name display).

*Pins:* 1 exec in, 1 exec out; inputs: `Speaker (int)`

### Set State

Sets a sprite's state machine to the given state ID.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `State (int)`

### Transition

Changes a sprite's state after an optional delay (in frames).

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `State (int)`, `Delay (int)`

### Set Collision Size

Sets the collision radius of a sprite.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Radius (int)`

### Ignore Collision

Disables collision between two specific sprites.

*Pins:* 1 exec in, 1 exec out; inputs: `Obj A (int)`, `Obj B (int)`

### Spawn At

Spawns a new sprite at the given world position.

*Pins:* 1 exec in, 1 exec out; inputs: `Asset (int)`, `X (int)`, `Z (int)`

### Destroy After

Destroys a sprite after a delay (in frames).

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Frames (int)`

### Spawn Projectile

Spawns a projectile from a sprite in its facing direction.

*Pins:* 1 exec in, 1 exec out; inputs: `Asset (int)`, `X (int)`, `Z (int)`, `Speed (int)`

### Set Lifetime

Sets the remaining lifetime (in frames) for a sprite.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Frames (int)`

### Draw Bar

Draws a horizontal bar at screen position (X, Y) with fill amount.

*Pins:* 1 exec in, 1 exec out; inputs: `X (int)`, `Y (int)`, `Value (int)`, `Max (int)`

### Draw Sprite Icon

Draws a sprite icon at screen position (X, Y).

*Pins:* 1 exec in, 1 exec out; inputs: `Asset (int)`, `X (int)`, `Y (int)`

### Show Timer

Shows the on-screen countdown timer.

*Pins:* 1 exec in, 1 exec out; inputs: `Slot (int)`, `Seconds (int)`

### Hide Timer

Hides the on-screen countdown timer.

*Pins:* 1 exec in, 1 exec out; inputs: `Slot (int)`

### Set Bar Color

Sets the color of a HUD bar (RGB15).

*Pins:* 1 exec in, 1 exec out; inputs: `Color (int)`

### Set Bar Max

Sets the maximum value for a HUD bar.

*Pins:* 1 exec in, 1 exec out; inputs: `Max (int)`

### Reload Scene

Reloads the current scene from scratch.

*Pins:* 1 exec in

### Set Checkpoint

Saves the current player position as a checkpoint.

*Pins:* 1 exec in, 1 exec out

### Load Checkpoint

Teleports the player back to the last saved checkpoint.

*Pins:* 1 exec in, 1 exec out

### Cursor Up

Moves the element's cursor to the previous stop (wraps to last).

*Pins:* 1 exec in, 1 exec out

### Cursor Down

Moves the element's cursor to the next stop (wraps to first).

*Pins:* 1 exec in, 1 exec out

### Follow Link

Navigates to the linked element at the current cursor stop.

*Pins:* 1 exec in, 1 exec out

### Follow Player

Moves a sprite toward the player, stopping at the given distance (default = collision bounds).

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `Distance (int)`, `Speed (int)`

### Is Near 2D

Gate: passes exec only if the player is currently adjacent to this blueprint's tilemap object (Mode 0).

*Pins:* 1 exec in, 1 exec out

### Is Follow Moving

Gate: passes exec only if the follow object is currently moving between tiles (Mode 0).

*Pins:* 1 exec in, 1 exec out

### Set Follow Facing

Sets the follow object's facing direction from its movement direction (Mode 0).

*Pins:* 1 exec in, 1 exec out

### Play Hud Anim

Starts a HUD animation layer (resets frame to 0).

*Pins:* 1 exec in, 1 exec out

### Stop Hud Anim

Stops a HUD animation layer.

*Pins:* 1 exec in, 1 exec out

### Reset Scene

Reloads the current scene. Player respawns at start position.

*Pins:* 1 exec in, 1 exec out

### Set Player Height

Sets the player collision height (pixels). Controls wall Y-overlap and floor snap-up distance. Default 12.

*Pins:* 1 exec in, 1 exec out; inputs: `Value (float)`

### Set HUD Value

_(see in-editor tooltip)_

*Pins:* 1 exec in, 1 exec out; inputs: `Value`, `Slot`

### Update Respawn Pos

Updates the Respawn start position to the given object's world position. Use on checkpoint trigger.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`

### Set Velocity X

Adds a world-X velocity to the player every frame, independent of input. Pair with Velocity Falloff to decay it (boost pads, knockback, wind).

*Pins:* 1 exec in, 1 exec out; inputs: `Velocity (float)`

### Set Velocity Z

Adds a world-Z velocity to the player every frame, independent of input. Pair with Velocity Falloff to decay it.

*Pins:* 1 exec in, 1 exec out; inputs: `Velocity (float)`

### Velocity Falloff

Linearly decays current vx/vz to 0 over N frames. Set after Set Velocity X/Z to define how quickly the boost/push fades.

*Pins:* 1 exec in, 1 exec out; inputs: `Frames (int)`

### Boost Forward

Pushes the player along their current view direction at the given speed. Runtime decomposes into world vx/vz using sin/cos(viewAngle). Pair with Velocity Falloff to decay.

*Pins:* 1 exec in, 1 exec out; inputs: `Speed (float)`

### Halt Momentum

Zeros all player velocity (vx, vz, vy, pending boost, falloff). Use after respawn so leftover boost-pad momentum doesn't carry over.

*Pins:* 1 exec in, 1 exec out

### Start Grind

Begin rail grinding (Mode 4). The player locks to its entry direction and slides on momentum — accelerating downhill, slowing uphill. Wire it to On Collision(rail mesh). Jumping or running off the end ends the grind.

*Pins:* 1 exec in, 1 exec out

### Stop Grind

End rail grinding immediately (restores normal input movement).

*Pins:* 1 exec in, 1 exec out

### Grind Power

Set the BASE downhill momentum gain while grinding (default 24). Higher = a descent builds speed faster. Drop it under On Start to tune the whole rail system.

*Pins:* 1 exec in, 1 exec out; inputs: `Gain (float)`

### Grind Boost

Add EXTRA downhill grind speed for this frame (only applies while descending). Gate it with a held button: On Update -> Is Key Held(B) -> Grind Boost, so holding sprint on a downslope accelerates harder.

*Pins:* 1 exec in, 1 exec out; inputs: `Force (float)`

### Grind Bleed

Set how slowly the Grind Boost's extra speed bleeds back to normal (default 6). Higher = momentum earned on a drop carries farther across flats before fading; lower = snaps back sooner; 0 = never bleeds. Persistent — fine under On Start or On Update.

*Pins:* 1 exec in, 1 exec out; inputs: `Slowness (float)`

### Grind Catch

Loosen how easily you re-catch a rail. Height = extra vertical window above the rail surface; Width = horizontal snap radius to the rail path (lands you on a thin rail without being exactly over it). Both in editor pixels, 0 = strict/off. Set under On Start.

*Pins:* 1 exec in, 1 exec out; inputs: `Height (float)`, `Width (float)`

### Set Player Width

Sets the player collision width (horizontal radius, pixels). Controls how close you get to walls before being pushed out. Default 3.

*Pins:* 1 exec in, 1 exec out; inputs: `Value (float)`

### Play Skeletal Anim

Plays a skeletal (glTF/DSMA) animation clip on the player rig in Mode 4. Wire a Skeletal Animation node into Clip. Loop/Once is set per-clip on the rig.

*Pins:* 1 exec in, 1 exec out; inputs: `Clip`

### Set Skel Anim

Sets the skeletal clip on a specific rigged NPC (like Set Sprite Anim, but for glTF rigs). Wire an Object (Instance) into Object and a Skeletal Animation into Clip.

*Pins:* 1 exec in, 1 exec out; inputs: `Object`, `Clip`

### Set Camera

Switches the player camera to a preset slot (Mode 4). Slot 0 = scene default; 1..N are the camera presets authored on the player object. Wire a number into Slot. The camera orbit-follows the player at the slot's angle/pitch/distance/height, smoothly blended.

*Pins:* 1 exec in, 1 exec out; inputs: `Slot (int)`

### Tank Camera

Tank controls (Mode 4). Wire 1 to make movement + facing follow the player heading (turned by Turn Player on the D-pad) instead of the camera, so forward/back go where the tank points while the camera still orbits freely (L/R). Wire 0 for normal camera-relative controls.

*Pins:* 1 exec in, 1 exec out; inputs: `On (int)`

### Turn Player

Rotates the tank heading (used with Tank Camera). Wire a Direction (Left/Right) and a Speed (brads/frame). Put it on On Key Held(Left)/(Right) so the D-pad turns the player in place while L/R still orbit the camera. Left-click for the Movement switch: Tank (Heading) makes movement follow the turned heading; Camera Relative keeps movement on camera axes and only steers the facing.

*Pins:* 1 exec in, 1 exec out; inputs: `Direction`, `Speed (int)`

### Cast Effect

Plays a combat/spell effect on a target object. Attach a sprite to that object and tick its 'Hidden' box (it starts invisible). Wire the target into Object: on trigger the effect shows, plays its animation once, and auto-hides. Set the effect sprite's animation to Once so it cleans up.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`

### Lock On

Lock-on camera assist (PSV): locks onto the Target; once locked the orbit always eases to face it — even off-screen — so locking on swings the camera around to frame it. Gate with Is In View to only lock onto on-screen targets. Wire Attached Sprite ("self" in a blueprint) or an Object into Target. ACQUIRE GATE: the target must be within Max Range (world px, default 70; 0 = unlimited) AND inside the facing Cone Deg half-angle (default 60; 0 = any direction) or the press does nothing (an existing lock is kept). Stays locked until Release Lock On — or, with Same-Key Release (default 1), until the key that locked is pressed again (the key is auto-captured from the On Key Pressed driving this node; 0 = only Release Lock On unlocks). Node-body settings: Zoom % = P0; Side Offset and Height are their own ints; P2 is a bitfield (bit0 = Zoom In/Out direction, bit1 = No Look-Down) — use bpVsSetBit to flip one without disturbing the other.

*Pins:* 1 exec in, 1 exec out; inputs: `Target (obj)`, `Same-Key Release (int)`, `Max Range (int)`, `Cone Deg (int)`

### Release Lock On

Releases the lock-on camera assist (pairs with Lock On — e.g. fire it next to Hide HUD when dropping the target). Also turns off Lock Strafe.

*Pins:* 1 exec in, 1 exec out

### Lock Strafe

Z-targeting movement (PSV): while a Lock On target is active, the player always FACES the target and movement becomes target-relative — Up closes in, Down backpedals, Left/Right circle-strafe around it. Fire it after Lock On; Release Lock On turns it off.

*Pins:* 1 exec in, 1 exec out

### Dash To Target

Lunges the player toward the Lock On target for a burst (PSV) — a homing dash for bullet-punch-style moves. Speed = world units/frame (like Walk/Sprint), Frames = lunge duration; stops early at melee range. No target locked -> dashes along current facing. Wire after the attack trigger; pair with Play Skel Anim(atk_phs).

*Pins:* 1 exec in, 1 exec out; inputs: `Speed (int)`, `Frames (int)`

### Strafe Anim

8-way directional animation picker (PSV lock-strafe): wire your clips (Forward, Fwd-Right, Right, Back-Right, Back, Back-Left, Left, Fwd-Left, relative to facing the target) and it plays the one matching the stick direction each frame. You DON'T need all 8 — any unwired direction falls back to the nearest wired one, so wiring just the 4 cardinals makes diagonals lean to a neighbor. One node replaces the per-direction gated Play Skel Anim chains; put it behind On Update -> Is Locked On.

*Pins:* 1 exec in, 1 exec out; inputs: `Fwd`, `Fwd-R`, `Right`, `Back-R`, `Back`, `Back-L`, `Left`, `Fwd-L`

### 8-Way Stick

Gate the left thumbstick to 8 directions (PSV): snaps the analog move vector to the nearest 45 deg (N/NE/E/SE/S/SW/W/NW) before movement and the Strafe Anim clip pick read it, so diagonals are crisp and the directional clips don't flicker between neighbors. Magnitude (push amount) is preserved. Fire it once from On Start.

*Pins:* 1 exec in, 1 exec out

### Dodge

One-button side roll (PSV): a pure LEFT/RIGHT dodge — never forward or back. On trigger the left stick's horizontal component picks the side, so diagonals trigger it too (up-left/down-left = left dodge); a neutral or pure up/down stick ALTERNATES left/right each press (ping-pong) so tap-dodging doesn't roll the same way forever. While locked on, the roll is perpendicular to the player->target line (circle-strafe). Speed = world units/frame (like Walk/Sprint), Frames = roll duration. Wire Left Clip = DodgeL and Right Clip = DodgeR. Idle Clip (optional) = the clip to snap back to when the roll ends while standing still (e.g. Idle) so the rig doesn't freeze on the dodge pose; if you're moving when it ends, Strafe Anim takes over instead. Ramp (int) = frames to ease the speed in from 0 (quadratic) so the roll accelerates instead of snapping to full velocity — softens the stiff feel and gives a windup you can time (0 = instant). Falloff (int) = frames to ease the speed back down to 0 at the END so it decelerates instead of dead-stopping (0 = hard stop). Cooldown (int) = lockout frames after a dodge fires; presses during the lockout do nothing, so mashing the button can't re-fire it (measured from the start, so set it >= Frames to also block mid-roll cancels; 0 = no cooldown). Wall-collides. Energy Cost (int) = energy spent per dodge (0 = free); it's atomic with the roll firing — a press that's blocked by the cooldown costs nothing, and the dodge won't fire at all if you can't afford it (so it doubles as an energy gate). Put it on a single On Key Pressed; pair with Is Not Dodging on the damage path for i-frames.

*Pins:* 1 exec in, 1 exec out; inputs: `Speed (int)`, `Frames (int)`, `Left Clip`, `Right Clip`, `Idle Clip`, `Ramp (int)`, `Falloff (int)`, `Cooldown (int)`, `Forward Clip`, `Back Clip`, `Energy Cost (int)`

### Charge Shot

Hold-to-charge focus blast (PSV): drive it from On Key Held. While held it shows the player's hidden "effect" sub-sprite (the focus ball) at chest height and grows it from Min Scale% to Max Scale% over Max Charge frames (180 = 3s). Sets the Is Charging gate so you can play the charge anim (atk_spc_chg, or atk_spc_chg_air behind Is Airborne). Release fires it — see Fire Charge Shot. The ball = the first hidden attached sub-sprite of the player rig (add it in the Meshes tab, tick "Hidden (effect)").

*Pins:* 1 exec in, 1 exec out; inputs: `Max Charge (int)`, `Min Scale% (int)`, `Max Scale% (int)`

### Fire Charge Shot

Fire the charged focus blast (PSV): drive it from On Key Released (same button as Charge Shot). Snapshots the charged ball into a homing projectile aimed at the Lock On target (fires straight forward if nothing is locked), then clears the charge. Damage = damage at FULL charge and scales down with how long you actually held it (min 1); Speed = projectile speed in TENTHS of px/frame (25 = 2.5, default 60 = 6.0); Hit Radius = connect slop (smaller = must be closer); Homing % = how hard it curves toward the target while the target is AHEAD (12 default; a dodge still clears it because of Circle Home); Circle Home (0/1) = with 0 (default) it stops homing + flies straight once the target is behind, so it never turns around to chase a dodge. Homing Life / Forward Life = how many frames a locked-on vs forward shot stays alive (240 / 90 default) — shorten Forward Life to clean up a MISS faster, since each fired blast is its own pooled projectile (several can be on the field at once). On reaching the target it deals damage and despawns. Pair with Play Skel Anim(atk_spc_lnc / atk_spc_lnc_air).

*Pins:* 1 exec in, 1 exec out; inputs: `Damage (int)`, `Speed (int)`, `Hit Radius`, `Homing %`, `Circle Home`, `Homing Life`, `Forward Life`

### Set Energy

Set the player's Energy resource to a value (clamped 0..max). Energy is the engine's second player resource (alongside HP) — drive a bar with it and gate abilities on it.

*Pins:* 1 exec in, 1 exec out; inputs: `Energy (int)`

### Add Energy

Accumulate Energy by Amount (clamped to max). Use it to regenerate over time (On Update -> Add Energy(1)) or grant on pickup/hit.

*Pins:* 1 exec in, 1 exec out; inputs: `Amount (int)`

### Spend Energy

Subtract Amount from Energy (clamped to 0) — the negation side. Gate it with Has Energy so an ability only fires (and only spends) when there's enough.

*Pins:* 1 exec in, 1 exec out; inputs: `Amount (int)`

### Set Max Energy

Set the Energy capacity (max). Energy is clamped to it; the energy bar reads current/max.

*Pins:* 1 exec in, 1 exec out; inputs: `Max (int)`

### Set Health

Set the player's Health to a value (clamped 0..max). The clean player Health resource (afn_health) that a health bar binds to — distinct from the per-sprite HP array.

*Pins:* 1 exec in, 1 exec out; inputs: `Health (int)`

### Damage Health

Subtract Amount from Health (clamped to 0). Drive it from On Hit / a hazard. The health bar drains automatically as Health drops.

*Pins:* 1 exec in, 1 exec out; inputs: `Amount (int)`

### Heal Health

Add Amount to Health (clamped to max). Pickups, regen (On Update -> Heal Health(1)), etc.

*Pins:* 1 exec in, 1 exec out; inputs: `Amount (int)`

### Set Max Health

Set the Health capacity (max). The health bar reads current/max.

*Pins:* 1 exec in, 1 exec out; inputs: `Max (int)`

### Spend Charge Energy

Subtract Energy scaled by how long the Charge Shot was held: Min % at a quick tap up to Max % at full charge (linear over Get Charge %). Defaults 4..33. Put it on On Key Released in place of a fixed Spend Energy so a tap costs little and a full charge costs the most. Min/Max are inputs (wire Integers to tune).

*Pins:* 1 exec in, 1 exec out; inputs: `Min % (int)`, `Max % (int)`

### Cycle Value

_(see in-editor tooltip)_

*Pins:* 1 exec in, 1 exec out; inputs: `Slot (int)`, `Delta (int)`, `Count (int)`

### Orbit Cam On Obj

BEGIN an orbit around a target object — the KO / death cinematic. Latches the anchor (angle + timer=0) and the params; pair with Orbit Cam Step (advances it each frame) and Stop Orbit Cam (ends it). Wire Attached Sprite ("self" in a blueprint) or an Object into Target; empty/-1 orbits the player. Tunable data pins (default to the KO feel when unwired): Zoom % of camera distance (45), Orbit Speed milli-rad/frame (12), Pitch centi-rad (32), Zoom Ease per-mille/frame (60 = 0.06), Look Height added world-px (0). Fire once on the trigger (On Rise).

*Pins:* 1 exec in, 1 exec out; inputs: `Target (obj)`, `Zoom %`, `Orbit Speed`, `Pitch`, `Zoom Ease`, `Look Height`

### Hold Skel Clip

Play a skeletal clip on a rigged NPC ONCE and freeze its last frame — the death collapse. Wire an Object (Instance) into Object and a Skeletal Animation (e.g. die) into Clip. Like Set Skel Anim but it holds the final pose instead of looping; a later Set Skel Anim on the same NPC clears the hold. Pair with On Update -> Is HP Zero(self) + Do Once, and Orbit Cam On Obj(self), for a KO cinematic.

*Pins:* 1 exec in, 1 exec out; inputs: `Object`, `Clip`

### Fade In Hud

Show a HUD element and crossfade its alpha from 0 to full over Frames frames (the element-level fade, on top of any piece keyframes). Wire an element index into Element and a frame count into Frames (default 60 = ~1s). Use for a results-menu / dialog fade-in: pair On Rise (win/lose) -> Delay -> Fade In Hud.

*Pins:* 1 exec in, 1 exec out; inputs: `Element`, `Frames`

### Stop Music

Stops ONLY the persistent music track (battle music), leaving one-shot SFX ringing. Differs from Stop Sound (kills SFX, keeps music) and Stop All (kills both). Use on a win/lose so a clash 'win_clash' SFX survives under the victory fanfare.

*Pins:* 1 exec in, 1 exec out

### Loop Hud Anim

Keeps a HUD element's anim layers active and looping — re-arms them and rewinds once they pass their length, so a layer that wasn't authored with Loop still blinks continuously. Drive it every frame: On Update -> Is Hud Visible(menu) -> Loop Hud Anim(cursor element).

*Pins:* 1 exec in, 1 exec out; inputs: `Element`

### Beam Clash

Enables the beam-clash mechanic and sets its feel tunables (drive from On Update). When both sides' FULL-charge beams meet, the runtime raises the 2D mash struggle; Cross taps push the balance toward the enemy (Player Push/1000 each) while the AI drains it (AI Push/1000 every ~AI Interval frames). Full Charge % sets the charge threshold, Meet Radius the collision distance. Air Fallback (default 90) = frames before a clash fires even if the beams never MEET (so it doesn't fizzle); set 0 to require a real meet within Meet Radius (stops far-apart clashes). AI Jitter (0 = dead-steady cadence, higher = more random/human), AI Fumble % (chance the masher briefly slips), AI Fumble Len (how long a slip lasts) — together these set the masher's SKILL: low jitter + low fumble = a relentless high-skill CPU; raise them for a sloppier, more beatable one (defaults 1/1/6 = high skill). AI Punish % is the chance per press the masher instead breaks into a sudden BURST of AI Punish Len fast presses (every 3 frames) — a random punish that hammers your balance and forces you to mash back hard (0 = off). Clash Dmg % (default 150) is how much of the WINNER's full attack the loser takes on resolve — 1.5x by default, so a clash can't one-shot a full-HP fighter (applied by Clash Hit Enemy / Clash Hit Player). Balance to 1 = you win (enemy takes the hit), to 0 = you lose; a resolve that drops HP to 0 still flows into the KO/death cinematic + results menu. Delete the node to disable clashing. Defaults: 85/60/50/6/18/150.

*Pins:* 1 exec in, 1 exec out; inputs: `Full Charge %`, `Player Push`, `AI Push`, `AI Interval`, `Meet Radius`, `Clash Dmg %`, `Air Fallback`, `AI Jitter`, `AI Fumble %`, `AI Fumble Len`, `AI Punish %`, `AI Punish Len`

### Suppress Beams

Kills both in-flight projectiles (player + enemy orbs). Drive it every frame while the clash HUD is up so a late-fired beam can't reappear mid-struggle.

*Pins:* 1 exec in, 1 exec out

### Clash Begin

Starts a clash: centres the balance (0.5), resets the AI press timer + button flash, and suppresses the beams. Fire ONCE on the On Rise of Is Clash Ready.

*Pins:* 1 exec in, 1 exec out

### Clash Push

The player's Cross tap pushes the clash balance toward the enemy by Player Push/1000 (the Beam Clash tunable). Wire from On Key Pressed(Cross) -> Is Hud Visible(clash) so it only counts during the struggle.

*Pins:* 1 exec in, 1 exec out

### Clash AI Step

One struggle step: the mid-skilled-human AI mashes back (drains the balance on a jittered interval with occasional fumbles), clamps the balance to [0,1], keeps the mash SFX looping + pitched by the balance, and cycles the Cross button flash. Drive every frame while the clash HUD is visible.

*Pins:* 1 exec in, 1 exec out

### Set AI State

Sets the enemy AI state (0 Roam..6 Dead), which decides what Is AI State dispatches. The transition actions of the state machine.

*Pins:* 1 exec in, 1 exec out; inputs: `State`

### AI Sense

Per-frame enemy sense: caches the enemy slot, handles death (-> Dead, KO cinematic via the BP), computes distance to the player, faces the player, ticks cooldowns, and sets the gate flags (lose/dodge/can-fire). Run it FIRST each frame: On Update -> AI Sense -> the state dispatch. Frozen during a beam clash.

*Pins:* 1 exec in, 1 exec out

### AI Roam

ROAM action: picks walk/idle clip while the navmesh drives the wander motion. Run under Is AI State(Roam).

*Pins:* 1 exec in, 1 exec out

### AI Chase

CHASE action: moves the enemy toward the player at Move Speed and sets the 'reached' flag at the strafe radius (Pref+30). Run under Is AI State(Chase).

*Pins:* 1 exec in, 1 exec out

### AI Strafe

STRAFE action: circle-strafes the player, correcting toward the preferred distance, with an 8-direction clip. Run under Is AI State(Strafe).

*Pins:* 1 exec in, 1 exec out

### AI Dodge Begin

Starts a side-roll dodge (picks a side, sets the roll vector/frames/cooldown). Fire once on the dodge-ready edge: Is AI Flag(dodge_ready) -> On Rise -> AI Dodge Begin + Set AI State(Dodge).

*Pins:* 1 exec in, 1 exec out

### AI Dodge Step

Integrates the dodge roll (ease-in/out, wall collision) and sets 'dodge_done' when finished. Run under Is AI State(Dodge); on dodge_done -> Set AI State(Strafe).

*Pins:* 1 exec in, 1 exec out

### AI Charge Begin

Starts a shot wind-up: rolls Charge % (full charge vs quick tap) and sets the wind-up length. Fire once on entering Charge. Charge SFX = the sound instance played (looped, proximity-gained, voice-tracked) during a full charge — default 4 ('charge'); set it from a Sound Instance node, or -1 for silent.

*Pins:* 1 exec in, 1 exec out; inputs: `Charge SFX`

### AI Charge Step

Holds the charge pose and grows the orb at the muzzle bone; sets 'charge_done' when the wind-up elapses. Run under Is AI State(Charge); on charge_done -> AI Fire Beam + Set AI State(Fire). Charge-dodge: an incoming blast in Dodge Range makes the enemy sidestep WITHOUT dropping the charge (orb keeps growing, plays the charge-dodge clips) — so it no longer cancels the charge to dodge.

*Pins:* 1 exec in, 1 exec out

### AI Fire Beam

Launches the enemy projectile from the muzzle toward the player (sets damage/speed/homing and the full-beam flag for clashes), and starts the recovery + attack cooldown. Charged SFX / Tap SFX = the launch sound instances, played at proximity gain — defaults 5 ('shoot', for a full charge) and 6 ('smallblast', for a tap).

*Pins:* 1 exec in, 1 exec out; inputs: `Charged SFX`, `Tap SFX`

### AI Fire Recover

Holds the launch clip and sets 'fire_done' when the recovery timer elapses. Run under Is AI State(Fire); on fire_done -> Set AI State(Strafe).

*Pins:* 1 exec in, 1 exec out

### Enemy AI

Enables the enemy combat AI and sets its tunables: Detect/Lose/Pref ranges (world px), Atk Cooldown (frames), Charge % (full-shot chance), Dodge % (dodge chance), Move Speed (x1000), Dodge Range (px from an incoming blast — homing OR forward — at which it reacts), Block % (chance to block instead of dodge an incoming blast), Block Dmg % (damage taken while blocking — 20 = take 20%; applies to BOTH the enemy and your block), Charge Speed + Tap Speed (the enemy's projectile launch speeds in TENTHS of px/frame, like your Fire Charge Shot — 20 = 2.0 full charge, 25 = 2.5 tap). Drive from On Update in the enemy BP. The state machine lives in the AI nodes; this turns it on, claims the BP's owner sprite as THE enemy (all enemy-keyed runtime systems — bone snapshots, HP seeding, physical clash, nav override, afterimage — key off it and stay dormant in projects without this node), and feeds the runtime primitives. Defaults 60/95/22/80/40/70/800/24/30/20/20/25.

*Pins:* 1 exec in, 1 exec out; inputs: `Detect Range`, `Lose Range`, `Pref Dist`, `Atk Cooldown`, `Charge %`, `Dodge %`, `Move Speed`, `Dodge Range`, `Block %`, `Block Dmg %`, `Charge Speed`, `Tap Speed`

### Orbit Cam Step

Advances the orbit camera one frame (the runtime no longer auto-advances it). Drive once per frame while orbiting: On Update -> Orbit Cam Step. Harmless when not orbiting (the timer resets on each Orbit Cam On Obj). Put it in ONE per-frame chain so the orbit advances once per frame.

*Pins:* 1 exec in, 1 exec out

### Stop Orbit Cam

Ends the orbit camera (afn_cam_orbit_active = 0) so the normal follow/lock camera eases back in. Use to stop the orbit on demand instead of waiting for a scene swap.

*Pins:* 1 exec in, 1 exec out

### Step Enemy Beam

Advances the enemy's in-flight projectile one frame — homes toward the player, deals damage + despawns on contact, expires on lifetime. Drive every frame (On Update -> Step Enemy Beam) so a shot completes even after the enemy dies. Firing is the AI Fire Beam node.

*Pins:* 1 exec in, 1 exec out

### Step Focus Blast

Advances the player's in-flight Focus Blast one frame — homes toward the captured target, deals damage to its HP + despawns on contact, expires on lifetime. Drive every frame (On Update -> Step Focus Blast). Charge/release are the Charge Shot / Fire Charge Shot nodes.

*Pins:* 1 exec in, 1 exec out

### Show HP Bar

Raises the floating 3D-anchored HP bar above an object this frame (fill = HP / Max HP). Per-frame: drive from On Update so it shows continuously; it auto-hides when the object's HP hits 0. Wire Attached Sprite(self) or an Object into Object; Max HP sets the full-bar value (default 100).

*Pins:* 1 exec in, 1 exec out; inputs: `Object`, `Max HP`

### Clash Hit Enemy

Clash win: deals Clash Dmg % (Beam Clash node, default 150 = 1.5x) of the player's full Focus Blast damage to the wired Object — instead of an instant KO, so a clash can't one-shot a full-HP enemy. If it brings HP to 0, the normal Is HP Zero -> KO cinematic + results menu fires.

*Pins:* 1 exec in, 1 exec out; inputs: `Object`

### Clash Hit Player

Clash loss: deals Clash Dmg % (default 150 = 1.5x) of the enemy's full charge damage to the player. If health reaches 0, the Is Health Zero -> death cinematic + results menu fires.

*Pins:* 1 exec in, 1 exec out

### Set Block

Sets the player's blocking flag (On 0/1). While 1, an incoming enemy hit is reduced to Block Dmg % (default 20) and the player spends Energy Cost energy per BLOCKED HIT (not per press — block freely, only pay when a hit actually lands). Wire On Key Held(Block) -> Set Block(1, cost) and On Key Released -> Set Block(0), next to your block clip.

*Pins:* 1 exec in, 1 exec out; inputs: `On (0/1)`, `Energy Cost`

### AI Block Begin

Enemy raises its guard — plays the block clip and sets the blocking flag for a short window, so your blast deals only Block Dmg % to it. Fire once on the Should AI Block edge.

*Pins:* 1 exec in, 1 exec out

### AI Block Step

Holds the enemy block stance (clip + blocking flag), counts the window down, and sets the block_done flag at the end. Run under Is AI State(Block); on block_done -> Set AI State(Strafe).

*Pins:* 1 exec in, 1 exec out

### Quick Attack

Dash-in melee (PSV): drive from On Key Pressed (e.g. Triangle). Lunges the player toward the Lock On target (or straight forward if nothing is locked), and on reaching Stop Range deals Damage once (enemy Block cuts it) and punches the camera in for impact; a whiff overshoots until Max Frames runs out, then a short Skid decelerates to a stop. Movement mirrors the Dodge (committed burst, wall-collide); normal movement is suppressed for the whole move. Tunables: Speed, Stop Range, Damage, Max Frames (dash budget), Skid Frames, Punch % (camera zoom-in, 0 = off), Lunge/Skid/Idle Clip (rig poses for each phase, -1 = leave current), Cooldown (spam-gate frames), Energy Cost, Trail Alpha (afterimage peak alpha, default 96; 0 = no trail), Trail Length (ghost count 0-6, default 6) and Trail R/G/B (ghost tint, default cyan 150/220/255 — Wild Charge's yellow still overrides while it dashes). Pair with Is Dashing (hold poses / i-frames) and Quick Attack Hit (smack SFX/FX).

*Pins:* 1 exec in, 1 exec out; inputs: `Speed (int)`, `Stop Range`, `Damage (int)`, `Max Frames`, `Skid Frames`, `Punch %`, `Lunge Clip`, `Skid Clip`, `Idle Clip`, `Cooldown (int)`, `Energy Cost (int)`, `Trail Alpha`, `Trail Length`, `Trail R`, `Trail G`, `Trail B`

### Charge Up

Hold-to-charge. While this runs each frame, it REVEALS the player's hidden attached effect models (the charge aura) and adds Energy/Frame to the Energy meter (clamped to max). The aura auto-hides the frame you stop running it. Drive it from On Key Held(Circle) so holding the button charges; release hides the aura and stops filling. Give the aura mesh 'Hidden (effect)' so it stays invisible until charging.

*Pins:* 1 exec in, 1 exec out; inputs: `Energy/Frame (int)`, `Charge Clip (Skel Anim)`

### Ai Quick Attack

Enemy AI melee reflex (run every frame from the enemy's On Update, AFTER its movement/state nodes so it can override pose + position). Two behaviours: (1) Quick Attack — when the player is within Dash Range and off cooldown, it rolls Trigger /1000 per frame to dash in at Dash Speed, dealing Damage on contact within Contact Range (a connect ends the dash; only a whiff plays the skid). (2) Jump-evade — ALWAYS hops a player Quick Attack dashing straight at it (Jump Vel x100 launch, same gravity arc as the player), even mid-charge. Auto-suppressed during the beam-clash struggle. Jump % is reserved (unused) for a future chance-gated evade. Tunables match the old #defines: Dash Range 70, Trigger 12/1000, Dash Speed 34, Contact Range 14, Damage 8, Cooldown 90, Jump Vel 150 (=1.5), Jump Cooldown 40. Whoosh SFX = the dash sound instance (proximity-gained), default 17 ('quicksweep'). Trail Alpha (afterimage peak alpha, default 96; 0 = off), Trail Length (ghost count 0-6, default 6) and Trail R/G/B (ghost tint, default white 255/255/255).

*Pins:* 1 exec in, 1 exec out; inputs: `Dash Range`, `Trigger /1000`, `Dash Speed`, `Contact Range`, `Damage`, `Cooldown`, `Jump Vel x100`, `Jump % (unused)`, `Jump Cooldown`, `Whoosh SFX`, `Trail Alpha`, `Trail Length`, `Trail R`, `Trail G`, `Trail B`

### AI Timing

Sets the enemy AI's remaining decision/timing knobs (the ones the Enemy AI node doesn't cover) — run it once from On Update BEFORE AI Sense. The state machine itself lives in the blueprint (Is AI State/Flag -> Set AI State); this just tunes the cadence. Pins (defaults match the old #defines): De-Aggro Frames (150 = ~2.5s outside Lose Range before returning to roam), Strafe Leg (90 = frames before re-rolling strafe direction), Yaw Ease x100 (35 = 0.35 turn-to-face lerp), Tap Windup (12 = quick-shot charge frames), Fire Recover (18 = post-launch recovery), Dodge Frames (20 = dodge duration), Dodge Cooldown (45 = frames between dodges), and Dodge Speed/Ramp/Falloff (default -1 = inherit the PLAYER's Dodge-node roll, so the enemy dodges identically; set >=0 to give it its own feel). Leave a pin unwired to keep its default.

*Pins:* 1 exec in, 1 exec out; inputs: `De-Aggro Frames`, `Strafe Leg`, `Yaw Ease x100`, `Tap Windup`, `Fire Recover`, `Dodge Frames`, `Dodge Cooldown`, `Dodge Speed (-1=player)`, `Dodge Ramp (-1=player)`, `Dodge Falloff (-1=player)`

### AI Clips

Sets the enemy's animation clip indices (Move, Idle, the 8-dir strafe set, Block, Charge Pose, Launch, Lunge, Skid, Jump, Jump Fall) — run once from On Update. The magic: each UNWIRED pin is name-resolved AT EXPORT to the rig's current clip index, so re-exporting the glTF (which re-sorts the anim list) can't drift the enemy's animations — same protection the player's SkelAnim nodes get. Wire a pin to a Skeletal Animation node to override a specific clip. Without this node the enemy uses the old hardcoded indices (which DO drift).

*Pins:* 1 exec in, 1 exec out; inputs: `Move`, `Idle`, `Strafe L`, `Strafe LD`, `Strafe LDFW`, `Strafe R`, `Strafe RD`, `Strafe RDFW`, `Backpeddle`, `Block`, `Charge Pose`, `Launch`, `Lunge`, `Skid`, `Jump`, `Jump Fall`

### Play Camera Anim

Takes over the game camera and plays the player's keyframed cutscene camera path (authored in the Meshes tab). Freeze Player (1) holds the player still during the cutscene; Freeze Enemy (1) holds the enemy AI (no movement, attacks, or decisions — stands in idle) until the path ends; Loop (1) repeats the path; Hold Last (1) keeps the final shot, otherwise (0) the camera eases back to the normal follow camera at the end and the player+enemy unfreeze. Anim = which path (0 for now). Player Clip = wire a Skeletal Animation node to force the player rig onto that clip (e.g. a 'winner' pose) for the WHOLE cutscene — it overrides the idle/move state machine until the path ends, then normal animation resumes; leave unwired for no override. Cry Sound = wire a Sound Instance index to play once at the cutscene start (e.g. a Pokémon cry); unwired = silent. Snap Player (1) = teleport the player to the scene-START pose the camera path was authored around (and clear lock-on/tank facing) at cut start, so a cutscene triggered MID-FIGHT (e.g. the victory cam) frames the player correctly instead of animating at the authored spot while the player is elsewhere; leave 0/unwired if the player is already in place (e.g. the intro). Snap X / Snap Z (int, world units) = an explicit snap spot instead of the scene spawn (wire BOTH; Y stays at spawn ground); leave unwired to snap to spawn. Face Angle (int, degrees 0-359) = the facing to hold for the whole cut instead of the scene-default heading; leave unwired for the default. Snap X/Z + Face Angle only apply when Snap Player is on. Freeze Input (1) = mask ALL buttons while the path is animating so no ability/lock-on/charge/dodge/movement fires during the shot — pair with Freeze Player; it auto-releases the instant the path completes (so a Hold-Last cut's results menu still gets input); 0/unwired = input stays live. Drive from On Start or any event.

*Pins:* 1 exec in, 1 exec out; inputs: `Anim (int)`, `Freeze Player (int)`, `Loop (int)`, `Hold Last (int)`, `Freeze Enemy (int)`, `Player Clip (int)`, `Cry Sound (int)`, `Snap Player (int)`, `Snap X (int)`, `Snap Z (int)`, `Face Angle (int)`, `Freeze Input (int)`

### Toggle Pause

Flips the global scene pause (drive from On Key Pressed(Start)). 'On Paused' fires the frame it pauses, 'On Unpaused' the frame it resumes — wire Show/Hide HUD + a Play Sound to each. While paused the runtime freezes the WHOLE scene (player, enemy AI, projectiles, animations) and only the key-pressed graph runs, so this node can still resume it. Self-gated: won't toggle during a cutscene (afn_cam_cut_active) or once a fighter is dead (afn_health <= 0).

*Pins:* 1 exec in, 2 exec out

### AI Dodge Clips

Sets the enemy's DODGE animation clips — the only enemy clips the AI Clips node doesn't cover. Run once from On Update (alongside AI Clips). Two sets: the standard sidestep/roll (Dodge L/R, Dodge FW/BWD = DodgeL/DodgeR/DodgeFW/DodgeBWD) used when reacting to an incoming blast, and the charge-dodge (Chg Dodge L/R = atk_spc_chg_dodge_L/_R) it plays when it sidesteps WITHOUT dropping a charge. Like AI Clips, each UNWIRED pin is name-resolved AT EXPORT to the rig's current index so a glTF re-sort can't drift them (defaults: Chg Dodge L/R = 9/10, Dodge L/R/FW/BWD = 28/29/27/26). The L/R clip the runtime picks is facing-relative (it projects the dodge move onto the enemy's actual render facing), so wire L=DodgeL and R=DodgeR straight. Wire a pin to a Skeletal Animation node to override. Without this node the enemy uses the old hardcoded indices (which DO drift).

*Pins:* 1 exec in, 1 exec out; inputs: `Chg Dodge L`, `Chg Dodge R`, `Dodge L`, `Dodge R`, `Dodge FW`, `Dodge BWD`

### Lock Player Functions

While this runs, LOCKS OUT the player's combat functions — no Charge Up (aura/energy fill), Focus Blast charge/fire, Quick Attack, Dodge, or Block can fire, even though the buttons are still pressed. HUD/menu navigation (cursor Up/Down, confirm — all On Key Pressed) still works, so it's safe to run while a menu is up. Use it for the game-over / results screen so navigating restart/title with the D-pad doesn't also trigger gameplay (e.g. holding Down to pick an option charging the player). Drive it per-frame: On Update -> Is Hud Visible(results menu) -> Lock Player Functions. Runtime: it masks the HELD keys (so On-Key-Held abilities like Charge never run — stopping the energy fill at its source) and clears the per-frame ability triggers after the graph runs (dodge/quick-attack/focus/block + the charge aura).

*Pins:* 1 exec in, 1 exec out

### Spawn Particles

Emits a burst of billboard particles at the player — a pure-code sim: each particle integrates velocity + gravity per frame, fades over its life, and faces the camera. Fire it from any event: a one-shot On Key Pressed = a burst, On Update = a continuous stream (it emits Count particles every frame it runs). Pins: Sprite = graphic frame for the billboard (-1 = solid quad); Count = particles per emit; Speed x100 = initial speed in world-units/frame ×100 (150 = 1.5); Spread 0-100 = lateral cone width (0 = straight up); Life = lifetime in frames; Size x100 = start size ×100 (shrinks to 0); Gravity x1000 = downward pull ×1000 (40 = 0.04). Spawns at the player + a small height offset (spline pathing + emitter presets come from the Effects tab).

*Pins:* 1 exec in, 1 exec out; inputs: `Sprite`, `Count`, `Speed x100`, `Spread 0-100`, `Life (frames)`, `Size x100`, `Gravity x1000`

### Lightning Beam

Casts a lightning bolt that BOUNCES across the floor — a connected jagged ribbon from the player's feet to the lock-on enemy's feet (or `Range` ahead if nothing's locked), made of `Bounces` arches that rise off the ground and touch back down between each, crawling forward as it crackles. Camera-facing, additive (glows on its own, no texture). Fire from On Key Pressed. Pins: Range = forward distance when unlocked; Width x100 = ribbon half-width ×100; Arch x100 = how high each bounce rises off the floor ×100; Jitter x100 = jagged crackle ×100 (0 = clean arches); Segments = base resolution (auto-raised so each arch is smooth); Life = frames the bolt lasts (flickering); Bounces = how many arches it skips across the floor; Decay x100 = how much SHORTER each successive bounce is (78 = each 78% of the last, like a ball losing energy; 100 = even arches); Pulse x1000 = speed of the bright 'ball' that travels the arcs to animate the bounce (0 = static glow). The bounce shape is a parabolic-arc spline (sharp at the floor contacts like a real bounce). The pink impact star is a separate Spawn Particles at the target.

*Pins:* 1 exec in, 1 exec out; inputs: `Range`, `Width x100`, `Arch x100`, `Jitter x100`, `Segments`, `Life (frames)`, `Bounces`, `Decay x100`, `Pulse x1000`

### Floor Reticle

Draws a glowing aim RETICLE on the floor a set distance ahead of the player's facing — a spinning ring + pulsing centre, additive. Drive it from On Update (or while a spell is charging) so it tracks the aim every frame; stop running it and the reticle disappears. Pins: Distance = how far ahead of the player it sits (world units); Size x10 = ring radius ×10 (40 = 4.0); Red / Green / Blue = colour 0-255. Great as the targeting marker for a Thunder-style strike, a teleport target, an AoE indicator, etc.

*Pins:* 1 exec in, 1 exec out; inputs: `Distance`, `Size x10`, `Red`, `Green`, `Blue`

### Thunder Charge

CHARGES the Thunder spell — a plane of dark rainclouds gathers overhead and a reticle tracks the aim on the floor ahead of the player, building toward a strike. Drive it from On Key Held (call it every frame the cast button is down). Stop calling it without a Thunder Strike and the clouds disperse (cancel). All look/params come from the Thunder layer in the Effects tab (cloud height, charge time, colours, aim distance, strike). Pair with Thunder Strike on release.

*Pins:* 1 exec in, 1 exec out

### Thunder Strike

Releases the THUNDER STRIKE — a vertical lightning bolt slams down from the cloud to the reticle, with an impact flash + spark burst. Drive it from On Key Released (the frame the cast button is let go) after Thunder Charge. Uses the Thunder layer's bolt params (width/crackle/filaments/colour). If called without charging first, it strikes instantly at the current aim.

*Pins:* 1 exec in, 1 exec out

### Aim Stick

FREE-AIM the reticle with the LEFT STICK — while this runs, forward/back slides the floor reticle (the Thunder strike target) nearer/farther, and left/right orbits the camera (the reticle sweeps around with it). ONLY active when NOT locked on (locked = the reticle snaps to the target). The player is frozen while charging so the stick is free to aim. Drive it from On Key Held alongside Thunder Charge. The slide + orbit SPEEDS are set on the Thunder layer in the Effects panel (Reticle speed / Reticle orbit) — this node is just the trigger.

*Pins:* 1 exec in, 1 exec out

### Ai Orb Scale

Sets the ENEMY focus-orb charge scale — Min Scale% (the seed size when the wind-up starts) and Max Scale% (the full-charge + in-flight size), as percentages of the focus_gfx base (like the player's Charge Shot). Run it once from On Update so the enemy's charge orb grows from Min to Max over the wind-up. Defaults: Min 5, Max 55. Only affects the CHARGE shot (the tap shot keeps its own size).

*Pins:* 1 exec in, 1 exec out; inputs: `Min Scale% (int)`, `Max Scale% (int)`

### Throw Ball

Throws the aimed pokeball — drive from On Key RELEASED (same key as the Aim Ball On Key Held). The pitch clip plays, the hand ball detaches at Release % of the clip, flies the aimed arc, and despawns on landing; after Cooldown frames it respawns in the hand. Any aim freeze is released the moment the ball detaches. While the ball is away (flight + cooldown) and the player stands still, Idle Clip plays instead of the BP's ball-carry stance (unwired = name-resolves 'idle'; wire -1 to disable). Pitch Clip unwired = name-resolves 'pitch'. Pair with Aim Ball; needs a bone-attached hand model + player rig.

*Pins:* 1 exec in, 1 exec out; inputs: `Pitch Clip (int)`, `Release % (int)`, `Speed x10 (int)`, `Cooldown (int)`, `Idle Clip (int)`

### Aim Ball

Aims the pokeball throw — drive from On Key HELD. While held: a dotted arc + white floor reticle preview the shot, L-stick X turns the aim, L-stick Y sets the distance (Dist Min..Max, starting at Dist Default). Freeze Aim (default 1) locks player movement while aiming. Aim Clip (unwired = keep the current anim) holds that rig pose for the whole aim. Release the key into an On Key Released -> Throw Ball to fire; letting go with no Throw Ball wired just cancels the aim. Without these nodes the system is fully dormant.

*Pins:* 1 exec in, 1 exec out; inputs: `Dist Min (int)`, `Dist Max (int)`, `Dist Default (int)`, `Turn Rate x10 (int)`, `Dist Rate x10 (int)`, `Arc % (int)`, `Freeze Aim (int)`, `Aim Clip (int)`

### Physical Clash

Arms the PHYSICAL clash (wire from On Start): when the player's Quick Attack dash and the enemy's dash meet head-on within Meet Radius, both fighters lock nose-to-nose and a pressure QTE begins — random face-button prompts shove the meter toward the enemy (Push), wrong buttons bleed it back (Miss), and the AI shoves on its own cadence (Ai Push / Ai Wait). Prompts and AI both quicken as the meter nears either edge (base Window frames). Overflow a side to resolve: winner deals Enemy/Player Dmg + launches the loser with Knockback frames of shove. Cooldown frames before it can re-trigger. Without this node the system is fully dormant. x1000 pins: 60 = 0.060 meter shove.

*Pins:* 1 exec in, 1 exec out; inputs: `Meet Radius (int)`, `Push x1000 (int)`, `Miss x1000 (int)`, `Ai Push x1000 (int)`, `Enemy Dmg (int)`, `Player Dmg (int)`, `Cooldown (int)`, `Window (int)`, `Ai Wait (int)`, `Knockback (int)`

### Lock Reticle

Draws the lock-on reticle (wire from On Start): while the camera is locked (Lock On node), a pulsing double-ring — counter-rotating outer + tight inner, additive glow — is drawn at the locked target's feet so the lock stays readable as the target wanders. Size % scales the rings (100 = default), Red/Green/Blue set the color (default gold 255/200/80), Spin % scales the rotation speed and Pulse % the breathing amount (0 = static ring). Without this node no reticle ever draws.

*Pins:* 1 exec in, 1 exec out; inputs: `Size % (int)`, `Red (int)`, `Green (int)`, `Blue (int)`, `Spin % (int)`, `Pulse % (int)`

---

## Gates / Conditions

### Branch

If condition is true, execute True path; otherwise False.

*Pins:* 1 exec in, 2 exec out; inputs: `Condition`

### Compare Var

Compares a variable slot against a value. Outputs 1 or 0.

*Pins:* inputs: `Var Slot`, `Value`; outputs: `Result`

### Is Moving

Gate: only passes execution through if the player is currently moving (d-pad held).

*Pins:* 1 exec in, 1 exec out

### Is On Ground

Gate: only passes execution through if the player is on the ground.

*Pins:* 1 exec in, 1 exec out

### Is Jumping

Gate: only passes execution through if the player is airborne and rising.

*Pins:* 1 exec in, 1 exec out

### Is Falling

Gate: only passes execution through if the player is airborne and falling.

*Pins:* 1 exec in, 1 exec out

### Check Flag

Branches on whether a flag (0-31) is set or clear.

*Pins:* 1 exec in, 2 exec out; inputs: `Flag (int)`

### Do Once

Only passes execution through once. Subsequent triggers are ignored.

*Pins:* 1 exec in, 1 exec out

### Flip Flop

Alternates between exec output A and B each time triggered.

*Pins:* 1 exec in, 2 exec out

### Gate

Passes execution only if the Open input is nonzero. 0 = blocked.

*Pins:* 1 exec in, 1 exec out; inputs: `Open (int)`

### For Loop

Executes the downstream chain Count times in a row.

*Pins:* 1 exec in, 1 exec out; inputs: `Count (int)`

### Sequence

Fires Then 0, then Then 1 in order each trigger.

*Pins:* 1 exec in, 2 exec out

### Is Flag Set

Gate: only passes execution if the specified flag bit is set.

*Pins:* 1 exec in, 1 exec out; inputs: `Flag (int)`

### Is HP Zero

Gate: only passes execution if the sprite's HP is zero.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`

### Is Near

Gate: only passes execution if two sprites are within the given radius.

*Pins:* 1 exec in, 1 exec out; inputs: `Obj A (int)`, `Obj B (int)`, `Radius (int)`

### Wait Until

Blocks execution until the connected condition becomes true.

*Pins:* 1 exec in, 1 exec out; inputs: `Condition`

### Repeat While

Keeps firing downstream every frame while condition is true.

*Pins:* 1 exec in, 1 exec out; inputs: `Condition`

### Delay

Fires downstream ONCE, N frames after the upstream gate opens (non-blocking, self-rearming). Drive it from a persistent gate (Is True / Is HP Zero / Is Health Zero), not a one-frame edge like On Rise. Re-fires fresh the next time the gate reopens (e.g. each battle).

*Pins:* 1 exec in, 1 exec out; inputs: `Frames (int)`

### Has Item

Gate: only passes execution if the inventory slot has count > 0.

*Pins:* 1 exec in, 1 exec out; inputs: `Slot (int)`

### Is Dialogue Open

Gate: only passes execution if a dialogue box is currently open.

*Pins:* 1 exec in, 1 exec out

### Dialogue Choice

Presents two choices to the player. Branches to A or B based on selection.

*Pins:* 1 exec in, 2 exec out; inputs: `Choice A (int)`, `Choice B (int)`

### Is In State

Gate: only passes execution if the sprite is in the given state.

*Pins:* 1 exec in, 1 exec out; inputs: `Object (int)`, `State (int)`

### Is Colliding

Gate: only passes execution if two sprites are overlapping.

*Pins:* 1 exec in, 1 exec out; inputs: `Obj A (int)`, `Obj B (int)`

### Is True

Gate: only passes execution through if the data input is non-zero (true).

*Pins:* 1 exec in, 1 exec out; inputs: `Value (int)`

### On Rise

Rising-edge gate: only passes execution on the first frame the upstream condition becomes true. Blocks while it keeps firing. Resets when it stops.

*Pins:* 1 exec in, 1 exec out

### Is Grinding

Gate: passes exec while the player is currently grinding a rail. Use to play a grind animation or gate a jump-off.

*Pins:* 1 exec in, 1 exec out

### Is Not Grinding

Gate: passes exec while the player is NOT grinding. Wire On Update -> Is Not Grinding -> On Rise -> Stop Sound to kill a looping grind SFX the instant you leave the rail.

*Pins:* 1 exec in, 1 exec out

### Is Locked On

Gate: passes execution only while a Lock On target is active. Branch lock-specific behavior — e.g. On Key Held(Down) -> Is Locked On -> Play Skel Anim(backpeddle), with the normal walk wired in parallel.

*Pins:* 1 exec in, 1 exec out

### Is Not Locked On

Gate: passes execution only while NO Lock On target is active — the inverse of Is Locked On. Use it to suppress the normal walk/face behavior while locked: On Key Held(Down) -> Is Not Locked On -> Play Skel Anim(walk).

*Pins:* 1 exec in, 1 exec out

### Is In View

Gate (PSV): passes execution only if the Target object is within the camera's view (on-screen). Wire Attached Sprite ("self") or an Object into Target. Gate your Lock On + Show HUD chain with it so you can only lock onto / show the ring for something you can see.

*Pins:* 1 exec in, 1 exec out; inputs: `Target (obj)`

### Is Dodging

Gate (PSV): passes exec only while a Dodge roll is active. Use it to suppress actions during a dodge (e.g. block re-triggering attacks).

*Pins:* 1 exec in, 1 exec out

### Is Not Dodging

Gate (PSV): passes exec only while NO Dodge is active — the inverse of Is Dodging. Wire incoming damage through it for dodge i-frames: On Hit -> Is Not Dodging -> Damage HP.

*Pins:* 1 exec in, 1 exec out

### Is Airborne

Gate: passes exec only while the player is off the ground — the clean inverse of Is On Ground. Drive the air animation with it: On Update -> Is Airborne -> Play Skel Anim(jump). For a rise/fall split use Is Jumping (rising) and Is Falling (descending) instead — on PSV those now read the real vertical velocity.

*Pins:* 1 exec in, 1 exec out

### Is Landing

Gate (PSV): passes exec for a short window (~12 frames) right after the player touches down. Wire On Update -> Is Landing -> Play Skel Anim(land) for a landing/squash pose. Pair with Is Not Landing on the idle chain so idle doesn't override it during the window.

*Pins:* 1 exec in, 1 exec out

### Is Not Landing

Gate (PSV): passes exec when the player is NOT in the post-touchdown land window — the inverse of Is Landing. Put it in front of your grounded idle (On Update -> ... -> Is On Ground -> Is Not Landing -> Play Idle) so the land anim plays first, then idle resumes.

*Pins:* 1 exec in, 1 exec out

### Is Charging

Gate (PSV): passes exec only while a Charge Shot is charging (button held, not yet fired). Drive the charge pose with it: On Update -> Is Charging -> Play Skel Anim(atk_spc_chg). Behind Is Airborne use atk_spc_chg_air.

*Pins:* 1 exec in, 1 exec out

### Is Firing

Gate (PSV): passes exec for a short window (~0.5s / 30 frames) right after a Charge Shot is fired — the mirror of Is Charging for the launch side. Without it the launch anim only flashes for one frame because Fire Charge Shot runs once on release. Wire On Update -> Is Firing -> Is On Ground -> Play(atk_spc_lnc), and behind Is Airborne use atk_spc_lnc_air, so the launch pose holds while the blast leaves.

*Pins:* 1 exec in, 1 exec out

### Is False

Gate: passes exec only when the Condition data input is ZERO — the inverse of Is True ('if not'). Wire a boolean expression into Condition: Compare, And/Or/Not, Get HP, Get Flag, Is Key Down, Get Player X/Y/Z, etc. Example: Get Flag(3) -> Is False -> (runs while flag 3 is clear).

*Pins:* 1 exec in, 1 exec out; inputs: `Condition`

### Switch on Int

Routes exec to one of five outputs by an integer Value: '= 0'..'= 3' fire when Value equals that case, 'Default' fires for anything else. Great for state machines: Get Variable/Get State -> Switch on Int -> per-state branches. Feed Value from any data expression (Get*, Compare, math).

*Pins:* 1 exec in, 5 exec out; inputs: `Value`

### Has Energy

Gate: passes exec only while Energy >= Amount. Shorthand for Compare(Get Energy, Amount, >=) -> Is True. Put it before Spend Energy + the ability so it only fires when affordable.

*Pins:* 1 exec in, 1 exec out; inputs: `Amount (int)`

### Is Not Charging

Gate (PSV): passes exec only while NO Charge Shot is charging — the inverse of Is Charging. Pair them off one On Key Pressed to fork an action: Is Not Charging -> normal move/dodge, Is Charging -> the charge-variant. Keeps the two mutually exclusive (e.g. so a charge-dodge and a normal dodge don't fight over the same cooldown).

*Pins:* 1 exec in, 1 exec out

### Is Health Zero

Gate: passes exec while the player's health resource (Damage Health / Set Health) is <= 0 — the player is defeated. Wire On Update -> Is Health Zero -> On Rise to fire a death cinematic / results screen once per death (On Rise re-arms when health refills on respawn).

*Pins:* 1 exec in, 1 exec out

### Is Hud Visible

Gate: passes exec only while the given HUD element slot is visible (afn_hud_visible[slot]). Use it to scope a menu's cursor-nav/confirm chain to the frames the menu is actually on screen — e.g. On Update -> Is Hud Visible(menu) -> On Key Pressed(Cross) -> Change Scene.

*Pins:* 1 exec in, 1 exec out; inputs: `Slot (int)`

### Is Clash Ready

Gate: passes exec while the runtime senses a clash is ready (both sides' full-charge beams airborne and meeting). Pair with On Rise to fire the start sequence once: On Update -> Is Clash Ready -> On Rise -> Clash Begin + Show HUD + Play Sound + Freeze Player.

*Pins:* 1 exec in, 1 exec out

### Is Clash Won

Gate: passes when the clash balance is pushed fully to the enemy (>= 1.0). Use to resolve a win: hide the clash HUD, stop the struggle/mash SFX, play win_clash, and Set HP(enemy, 0).

*Pins:* 1 exec in, 1 exec out

### Is Clash Lost

Gate: passes when the clash balance is pushed fully into the player's zone (<= 0.0). Use to resolve a loss: hide the clash HUD, stop the SFX, play win_clash, and Set Health(0).

*Pins:* 1 exec in, 1 exec out

### Is AI State

Gate: passes while the enemy AI state == State (0 Roam, 1 Chase, 2 Strafe, 3 Charge, 4 Fire, 5 Dodge, 6 Dead, 7 Block). Wire an Int into State. Dispatch each state's action node under it.

*Pins:* 1 exec in, 1 exec out; inputs: `State`

### Is Player Within

Gate: passes while the distance from the enemy to the player is <= Range (world px). AI Sense computes the distance each frame. Use for detection (Range = Detect) and chase-reached (Range = Pref+30).

*Pins:* 1 exec in, 1 exec out; inputs: `Range`

### Is Player Beyond

Gate: passes while the distance to the player is > Range (world px). Pair with the lose-timer / Is AI Flag(lose_ready) for de-aggro.

*Pins:* 1 exec in, 1 exec out; inputs: `Range`

### Is AI Flag

Gate: passes while an enemy-AI per-frame flag is set. Flag: 0 lose_ready, 1 dodge_ready, 2 can_fire, 3 charge_done, 4 dodge_done, 5 fire_done, 6 reached, 7 block_done. AI Sense and the step nodes set these. Wire an Int into Flag.

*Pins:* 1 exec in, 1 exec out; inputs: `Flag`

### Is Blast Incoming

Gate: passes when a player Focus Blast (homing or forward) is within the enemy's Dodge Range and the Dodge % chance rolls true (and it isn't already dodging / on cooldown). The dodge decision, now in the graph. Wire: AI Sense -> Is Blast Incoming -> On Rise -> AI Dodge Begin + Set AI State(Dodge).

*Pins:* 1 exec in, 1 exec out

### Should AI Block

Gate: passes when a player blast is incoming (within Dodge Range) and the Block % chance rolls true — and the AI isn't already dodging/blocking. The AI dodges OR blocks. Wire: AI Sense -> Should AI Block -> On Rise -> AI Block Begin + Set AI State(Block).

*Pins:* 1 exec in, 1 exec out

### Can Fire Blast

Gate: passes its exec ONLY while no Focus Blast is in flight (afn_fb_active == 0). The blast machine is single-shot — once a shot is on the field you can't charge or fire a new one — but the fire/charge SFX node would still play on the button press. Wire your Focus Blast charge/fire sound (and the charge start, if you want) behind this gate so it stays silent while a shot is already out: On Key Pressed(fire) -> Can Fire Blast -> Play Sound.

*Pins:* 1 exec in, 1 exec out

### Is Dashing

Gate: passes while a Quick Attack is mid-move (dash OR skid, afn_qa_phase != 0). Use it to hold the lunge/skid pose, grant i-frames, block other inputs, or loop a dash SFX for the committed window.

*Pins:* 1 exec in, 1 exec out

### Quick Attack Hit

Gate: passes on the SINGLE frame a Quick Attack dash reaches Stop Range and lands its hit (one per dash). Wire the smack SFX, impact FX, or a HUD flash behind it.

*Pins:* 1 exec in, 1 exec out

### Quick Attack Started

Gate: passes on the SINGLE frame a Quick Attack dash ACTUALLY begins (after the cooldown/energy/charge gate, unlike the raw key press). Drive On Update -> Quick Attack Started -> Play Sound for a swing-whoosh that never fires on a blocked press.

*Pins:* 1 exec in, 1 exec out

---

## Data / Math

### Integer

Outputs a constant integer value.

*Pins:* outputs: `Out`

### Key

Outputs a key constant (A, B, L, R, etc). Stick directions add Sensitivity (where the analog ramp starts tripping) and Strength (how fast a full push moves — the ramp's top speed).

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

### Group

Groups nodes into a reusable subgraph.

*Pins:* no pins

### Object

_(see in-editor tooltip)_

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

Outputs the raw D-pad analog axis value (-256..256) — NOT for analog stick directions; for left/right-stick Up/Down/Left/Right use a Key node (R-Stick Up=16, etc.) into On Key Held, not this node.

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

### Sound Instance

Outputs a sound instance index for PlaySound.

*Pins:* no pins

### Skeletal Animation

Outputs a skeletal animation clip index (feeds Play Skeletal Anim). Pick a rigged mesh and one of its glTF clips.

*Pins:* outputs: `Out`

### Attached Sprite

Outputs the sprite this blueprint instance is attached to ("self"). Wire into Show HUD's Anchor to pin the element to the owner's attached-sprite position in the world (PSV) — the element tracks the object on screen.

*Pins:* outputs: `Out`

### Bool

Constant boolean data node — outputs 1 (true) or 0 (false). Feed it into a Branch / Is True / Is False condition, or any data pin. Toggle the value in the node.

*Pins:* outputs: `Out`

### XOR

Logical exclusive-or: outputs 1 when exactly ONE of A/B is non-zero, else 0. Pure data node — wire into a condition (Branch / Is True) alongside And / Or / Not.

*Pins:* inputs: `A`, `B`; outputs: `Result`

### Get Energy

Outputs the current Energy value. Wire it into a condition (Compare / Branch / Is True) or a bar's source — e.g. Compare(Get Energy, cost, >=) to gate an ability or pulse a glow.

*Pins:* outputs: `Energy`

### Get Health

Outputs the current Health value. Wire into a condition (Compare / Branch / Is True) — e.g. Compare(Get Health, 0, <=) -> Branch for death.

*Pins:* outputs: `Health`

### Get Charge %

Outputs the Charge Shot's current charge as 0-100% (afn_fb_level / afn_fb_max). Read it on On Key Released (before the shot resets) to branch the fire by charge — e.g. Branch[Compare(Get Charge %, 99, >=)]: full = big blast, else = a tap/small shot.

*Pins:* outputs: `Charge %`

### Play Effect

Triggers an authored EFFECT INSTANCE (built in the Effects tab) by index, at the player. Each instance is a self-contained effect made of one or more composited LAYERS (e.g. a lightning bolt + 2 particle bursts) — each layer has its own emitter/lightning params and (for lightning) its own hand-dragged spline shape. Playing the instance fires ALL its layers at once. Pin: Instance = which effect instance to play (0-based, matches the [n] index in the Effects tab's Instances list). A particle layer fires a one-shot burst; a lightning layer casts a bolt that follows that layer's authored spline (the exact arc/bounce you dragged), scaling the editor's pixel params to world units. Fire from any event (On Key Pressed = one-shot, On Update = continuous). Authoring lives in the Effects tab; this node just plays an instance.

*Pins:* 1 exec in, 1 exec out; inputs: `Instance`

---
