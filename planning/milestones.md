# FNVMP Implementation Milestones

> Each milestone is a **buildable, testable** increment. Earlier milestones use hardcoded
> values and minimal scope; later milestones replace hardcodes with real systems.
>
> **Convention**: test criteria describe what a human tester should observe.
> "Server log" = stdout from server.exe. "Client log" = fnvmp plugin log file.
>
> **Testing shortcut (v0.1–v0.10)**: No Doc Mitchell intro required. Testers load any
> existing savegame and connect. The base save system is introduced in v0.11 when
> persistence is implemented. This keeps iteration fast during development.

---

## v0.1 — Network Foundation (COMPLETED)

**Goal**: ENet compiled into both client and server. Two processes can connect, stay connected, and detect disconnection.

**Scope**:
- `server.exe`: bare ENet host — listens on hardcoded port 7777, accepts connections, assigns `NetEntityId` (incrementing counter), sends assignment back to client
- `fnvmp.dll`: links ENet, connects to **hardcoded** `127.0.0.1:7777` on plugin load (DeferredInit)
- Heartbeat: client sends every 2 s, server times out after 5 s
- Protocol: only three message types — `MSG_CONNECT_ACK` (server → client, carries NetEntityId), `MSG_HEARTBEAT` (client → server), `MSG_DISCONNECT` (either direction)
- Packet format: raw packed structs with `uint8_t msgType` header (no sequence number yet)

**Hardcoded**: server IP/port, no authentication, no game state, no config file

**Not included**: any game interaction, snapshots, NPC spawning

**Test**:
1. Start `server.exe` → logs "Listening on port 7777"
2. Launch FNV with plugin → client log says "Connected, assigned NetEntityId 1"
3. Server log shows "Player 1 connected" and periodic heartbeat receipts
4. Close FNV → server detects timeout within 5 s, logs "Player 1 disconnected"

---

## v0.2 — Player Snapshot Pipeline (Single Client) (COMPLETED)

**Goal**: Client reads its own position from the game engine and sends it to the server. Server receives and logs it. Proves the full send path works with one client before involving a second.

**Scope**:
- Client samples local player `posX, posY, posZ, rotZ, cellId` every game tick (only when a savegame is loaded — `parentCell` must be non-null)
- Client sends `EntitySnapshot` at **30 Hz** (pos + rot + cellId; `movementState` hardcoded to `Idle`, `weaponFormId = 0`, `actionState = None`)
- Server receives `EntitySnapshot`, logs position to stdout (behind `--verbose` flag; off by default to avoid spam in later milestones)
- Server stores latest snapshot per connected player (in memory)
- Sequence numbers added to packet header (`uint16_t`)

**Hardcoded**: movementState, weaponFormId, actionState all stub values

**Not included**: second client, snapshot relay, NPC spawning, interpolation, animation

**Test** (single client only):
1. Start server, launch one FNV instance → connects
2. Walk around in FNV → server log shows coordinates updating in real-time
3. Stand still → coordinates stabilize. Walk again → coordinates resume updating
4. Verify server receives ~20 snapshots/sec (check log timestamps)

---

## v0.3 — Two-Client Relay + Remote Player Avatar (COMPLETED)

**Goal**: Server relays snapshots between two clients. Each client spawns a visible NPC representing the other player.

**Scope**:
- Server packs stored snapshots into `WorldSnapshot`, broadcasts to all **other** connected clients at 30 Hz
- On first `WorldSnapshot` containing an unknown `NetEntityId`: spawn NPC via `PlaceAtMe` using hardcoded base form (`0x001206FD` — NCR trooper)
- Configure spawned NPC as a restrained teammate:
  1. `SetRestrained 1` — prevents autonomous movement; position driven by network data. NPC remains visible to AI (targetable by hostile NPCs)
  2. `AddToFaction playerFaction 1` — faction hostility propagates to all player avatars
  3. `SetPlayerTeammate 1` — NPC treated as player's combat teammate
- Maintain `NetEntityId → TESObjectREFR*` map (entity manager)
- Each tick: set NPC position/rotation directly from **latest** received snapshot (no interpolation — simple teleport)
- On `PlayerDisconnect` event (or timeout): despawn (disable + mark for delete) the remote NPC
- `PlayerConnect` / `PlayerDisconnect` events added to protocol (reliable channel)

**Hardcoded**: base form, NPC name (just "Player 2"), no interpolation

**Not included**: interpolation, animation, launcher

**Test** (two clients — first time a second client is introduced):
1. Two clients connected. Both load any savegame where they're in Goodsprings
2. Each sees an NCR trooper NPC appear representing the other player
3. Client A walks around → the NPC in B's game visibly moves to follow (will be jerky/teleporting — that's expected)
4. Client B disconnects → NPC disappears from client A's game within a few seconds

---

## v0.4 — Interpolation (Completed)

**Goal**: Remote player NPC moves smoothly instead of teleporting between snapshots.

**Scope**:
- Interpolation buffer stores last 2–3 snapshots per remote entity (timestamped)
- Render time = `currentTime - 100ms` (always rendering slightly in the past)
- Each tick: lerp between the two snapshots bracketing render time (position + rotation)
- If no new snapshot for 500 ms: extrapolate briefly (continue last velocity), then freeze
- Apply interpolated position/rotation to NPC each tick

**Hardcoded**: buffer depth (100 ms), extrapolation timeout (500 ms)

**Not included**: animation, only positional smoothing

**Test**:
1. Two clients connected in same area
2. Client A walks at a steady pace → remote NPC in client B follows smoothly, no visible teleporting
3. Client A stops → NPC smoothly decelerates to a stop (slight ~100 ms delay is expected)
4. Simulate packet loss (e.g., walk behind a wall and back) → NPC briefly extrapolates, then resumes smooth movement

---

## v0.5 — Animation Sync (Completed)

**Goal**: Remote NPCs play correct locomotion and combat animations instead of sliding around in idle pose.

This milestone is split into two sub-phases that are implemented together:

### v0.5a — Detection (State Sampling)

Sampling animation state from actors (local player in v0.5, zone-owned NPCs in v0.7) to include in snapshots. The same detection logic applies to both — v0.5 proves it on the local player, v0.7 extends it to NPCs. All detection functions work for both players and NPCs.

**Plugin dependencies**: ShowOff NVSE (`IsAiming`, `GetEquippedWeaponType`). JIP LN NVSE dependency removed — `IsAttacking` replaced by `GetAnimAction` edge detection (base game function).

**Movement direction** — inferred from position + rotation each tick:
- Compute angle between position delta (movement vector) and `rotZ` (facing direction)
- Map to 8-direction bin (45° sectors): Forward, Backward, Left, Right, ForwardLeft, ForwardRight, BackwardLeft, BackwardRight
- No position change + rotation change → TurnLeft/TurnRight
- No position change + no rotation change → None (idle)
- Works correctly regardless of simultaneous mouse rotation

**State flags** — polled per tick:
- `IsRunning` (base game) → `isRunning` flag. Note: returns 1 in "run mode" even when idle (Caps Lock toggles), so only meaningful when combined with movement. Sprinting treated as running for v0.5
- `IsSneaking` (base game) → `isSneaking` flag
- `GetAnimAction` (base game) → values 0 (holstering) / 1 (drawing) detect weapon draw/holster transitions. `IsWeaponOut` used as 30Hz fallback if `GetAnimAction` misses a transition
- `IsAiming` (ShowOff) → `ActionAimingIS` bit in `actionState`. Stays 1 during aimed fire
- `GetAnimAction` (base game) → edge detection for multiple actions: values 0/1 for weapon draw/holster (see above), value 9 = `ActionReloading`. Also replaces `IsAttacking` for fire detection via edge detection on attack set {2,3,5,6} (4=AttackLatency excluded so semi-auto 2→4→2 cycles each trigger a new fire event; no JIP LN NVSE dependency)

**Fire events**: Firing is not sent via the unreliable snapshot `actionState` bitmask. Instead, fire events use reliable `MSG_PLAYER_FIRE` (client → server) / `MSG_REMOTE_FIRE` (server → client) messages on `CHANNEL_GAME_EVENTS`, ensuring no missed shots due to packet loss.

**ActionState priority**: Reloading (highest) > AimingIS. Bitmask allows combinations where applicable

**Weapon type classification** — done on the *receiving* side, not the sending side. Receiver calls `GetEquippedWeaponType` (ShowOff) on the received `weaponFormId`: 0–2 = melee, 3–9 = ranged, 10–13 = thrown

### v0.5b — Application (State → Animation)

Applying received animation state to remote player NPCs. All findings from in-game research:

**Basic locomotion** (all loop, all work while restrained):
- `PlayGroup` for Idle, Forward, Backward, FastForward, FastBackward, Left, Right, FastLeft, FastRight, TurnLeft, TurnRight
- If actor is turning on the spot, use TurnLeft/TurnRight
- If actor is strafing (left/right without forward/backward), use just Left/Right (or FastLeft/FastRight if running)
- If actor is moving diagonally, combine Forward/Backward with Left/Right at the same time (or FastForward/FastBackward with FastLeft/FastRight if running)
- Backward diagonals supported: Backward + Left, Backward + Right (and fast variants)

**Sneak** (requires unrestrain workaround):
- `SetForceSneak 1` works but `SetRestrained` must be 0 first
- After entering/exiting sneak, actor naturally continues whatever animgroup was active — no need for PlayGroup Idle after SetForceSneak
- Basic locomotion animgroups applied normally while sneaking
- `IsSneaking` used for polling before re-restraining; returns `"is sneaking"` / `"is not sneaking"` (text, not 1/0)

**Weapon draw/holster** (requires unrestrain workaround):
- Already researched — see v0.4 notes and tech spec §10.1
- Completion detection on receiver uses `GetAnimAction` polling (instead of `IsWeaponOut`) to determine when to re-restrain

**Non-melee weapons (pistols, rifles)** — all independent of SetRestrained:
- Aiming: `PlayGroup AimIS 1` (aim down sights), `PlayGroup Aim 1` (lower weapon)
- Hip-fire: `PlayGroup AttackLeft 1`
- Aimed fire: `PlayGroup AttackLeftIS 1` → `PlayGroup AimIS 0` (0 ensures previous animation completes before returning to AimIS)
- Reload: `PlayGroup ReloadA 1` (has built-in sound; weapon-specific variants like ReloadB deferred to v0.13)
- AttackLeft/AttackLeftIS are animation only — no sound or muzzle flash
- Sound per shot: `GetEquippedWeapon` → `GetWeaponSound weapon 0` ("Attack Sound 3D") → `PlaySound3D`
- For every shot (single or one round of automatic fire), both attack animgroup and sound must be played
- Automatic weapon sustained fire optimization deferred to v0.13
- Muzzle flash deferred to v0.13

**Melee weapons (swords, knives, power gloves)**:
- `PlayGroup AttackRight 1` for both one-handed and two-handed — good enough for now
- Multiple attack patterns, blocking deferred to v0.13

**Thrown weapons (spears, grenades)**:
- Highly weapon-dependent (spears = AttackThrow7, grenades = AttackThrow)
- `PlayGroup AttackThrow6 1` as universal placeholder for all thrown weapons
- Per-weapon mapping deferred to v0.13

**Non-human attacks (creatures/animals)**:
- Creature must "unholster" weapon first (enter alert combat state) before attack animation works
- `PlayGroup AttackRight 1` as universal placeholder
- No special human/non-human check needed — just check alert state + melee weapon equipped
- `GetEquippedWeapon` shows "Fists" for creatures like dogs (despite animation being biting)
- Attack variety deferred to v0.13

**Other**:
- Console output suppression via `SafeWrite8` on `Console::Print` (already proven in prototype)
- Rename existing `PlayerSnapshot`/`NPCSnapshot` in codebase to unified `EntitySnapshot`/`EntityState` (see tech spec §4.2)
- `EntityState` struct uses `moveDirection` + `isRunning` instead of old `movementState` enum (see tech spec §4.2, Appendix B)

**Hardcoded**: nothing new; all state is sampled live

**Not included**: NPC animation detection and relay (researched here but implemented in v0.7 — zone owner samples NPC state, non-owners apply using the same techniques), equipment visuals, jump animations, stagger/hit reactions, muzzle flash, weapon-specific reload variants, per-weapon thrown animations, melee attack variety, automatic weapon fire optimization, creature attack variety (last seven deferred to v0.13)

**Test**:
1. Client A stands still → remote NPC in client B plays idle animation
2. Client A walks → NPC walks. Client A runs → NPC runs. Client A sneaks → NPC sneaks
3. Client A draws weapon → NPC draws weapon. Client A holsters → NPC holsters
4. Client A fires → NPC plays fire animation with sound. Client A reloads → NPC plays reload animation
5. No console spam visible in-game during any of the above

**Full list of animations that are synced as of v0.5:**
- turning on the spot
- walking, running, sprinting
- strafing
- walking diagonally
- crouching, walking while crouching
- holstering, unholstering
- aiming, aiming while crouching, aiming while moving
- shooting, shooting while aiming
- shooting with semi-auto, full-auto
- attacking with melee one-handed, two-handed, fists
- throwing spears, grenades
- reloading
- (un)holstering while crouching and standing up

---

## v0.6 — Launcher + Identity

**Goal**: Players connect via a launcher instead of hardcoded values. Players have persistent identity (token) and display names.

**Scope**:
- `join_server.exe` CLI: prompts for IP (default port 7777), checks for stored token, prompts for display name if first time
- Writes `fnvmp_connect.cfg` to `Data/NVSE/Plugins/`
- `fnvmp.dll` reads `fnvmp_connect.cfg` on load, deletes after reading; if file missing, plugin is inert (single-player mode)
- Server generates UUID v4 token on first connect, sends to client
- Client stores token in `Data/NVSE/Plugins/fnvmp/<server_ip>.token`
- Returning player: launcher sends stored token → server recognizes, assigns same NetEntityId lineage
- `PlayerConnect` event now includes `playerName`
- Remote NPC renamed to player's display name

**Hardcoded**: nothing — all connection info from launcher/config

**Not included**: base save management, bootstrap, persistence

**Test**:
1. Run `join_server.exe` → prompted for IP and name → FNV launches
2. Second player does the same → each sees a named NPC representing the other
3. Close FNV, re-run launcher with same server IP → no name prompt (token found), reconnects as same player
4. Without launcher (no cfg file) → plugin does nothing, normal single-player game

---

## v0.7 — Zone Ownership + NPC Sync

**Goal**: NPCs in the world are synced between clients. One player per zone "owns" the NPCs and reports their state to others.

**Scope**:
- **Server zone tracking**: zones keyed by `(WorldspaceID, gridX, gridY)` for exteriors, `CellFormID` for interiors
- Client reports zone entry/exit to server
- First player in zone = zone owner; server sends `OwnershipTransfer` event
- **Zone owner responsibilities**:
  - Detects NPC spawns in cell, sends `SpawnNPC` (baseFormId, pos, rot, isPersistent)
  - Server assigns NetEntityId, broadcasts to all in zone
  - Sends `EntitySnapshot` at 30 Hz (same format as player snapshots, batched for owned NPCs)
  - Sends `DespawnNPC` when NPCs leave/die
- **Non-owner clients**:
  - Spawn matching NPC (same baseFormId) on receiving SpawnNPC
  - Disable AI on local copy (`SetActorsAI 0` — full disable; NPC copies are visual mirrors, not targetable)
  - Apply position/rotation from received EntitySnapshots (with interpolation)
  - Apply animations using the same techniques proven in v0.5 (locomotion, sneak, weapon draw/holster, combat actions — see tech spec §10)
- **Ownership transfer**:
  - Owner leaves zone → 5 s grace period → promote next player
  - Owner disconnects → immediate promotion
  - No players in zone → zone state deleted (NPCs forgotten)
- Named/persistent NPCs identified by `FormID + CellID`

**Hardcoded**: nothing new

**Not included**: NPC combat sync, NPC death events

**Test**:
1. Client A enters Goodsprings → becomes zone owner, server logs it
2. Client B enters same area → sees same NPCs in same positions as client A
3. NPCs walk around on client A (vanilla AI) → same movement visible on client B
4. Client A leaves the area → after 5 s, client B becomes zone owner, NPCs briefly freeze then resume under B's control
5. Both players leave → zone forgotten. Re-entering spawns fresh vanilla NPCs

---

## v0.8 — Combat

**Goal**: Players can fight NPCs (and each other) with damage and death synced across all clients.

**Scope**:
- Client detects weapon hit, sends `HitEvent` (targetNetEntityId, damage, weaponFormId, hitLocation)
- Server health tracking: maintains HP per entity, validates basic sanity (hit rate, distance)
- Server broadcasts `HealthUpdate` (netEntityId, currentHP, maxHP) on reliable channel
- NPC death: zone owner detects HP ≤ 0, executes `Kill` locally, sends `DeathEvent`
- Server broadcasts `DeathEvent` → all clients apply `Kill` on their local copy
- Player damage: same flow but player avatar NPC is the target
- **Player avatar death handling**: `SetRestrained` prevents normal death (companion unconscious behavior). To kill a player avatar: temporarily `SetRestrained 0`, apply `Kill`, then clean up. Exact mechanism TBD during implementation.
- No respawn yet (dead = dead until next milestone)

**Hardcoded**: sanity check thresholds (distance, rate)

**Not included**: respawn, corpse persistence, loot

**Test**:
1. Client A shoots an NPC → NPC takes damage, `HealthUpdate` visible in both clients' logs
2. Continued shooting → NPC dies on all clients (ragdoll may differ, that's expected)
3. Client A shoots client B's avatar → B receives health update, sees damage feedback
4. Excessive hit rate → server rejects/logs as suspicious

---

## v0.9 — Equipment + Inventory Sync

**Goal**: Players can see each other's equipped gear. Inventory is tracked for persistence prep.

**Scope**:
- Client detects sync triggers: container close (including Pip-Boy), cell enter, death, periodic (every 180 s)
- Sends `InventorySync` event: full list of `{ formId, count, isEquipped, slot }`
- Sends `EquipChange` event on individual equip/unequip
- Receiving client: updates remote player NPC inventory, equips/unequips items to match visually
- Server stores latest inventory in memory (persistence comes next milestone)

**Hardcoded**: autosave interval (180 s)

**Not included**: persistence to disk, NPC inventory/loot sync, corpse loot

**Test**:
1. Client A equips a piece of armor → client B's NPC avatar of A visually shows the armor
2. Client A unequips → armor disappears on B's view
3. Client A switches weapons → visible on B
4. Open and close Pip-Boy → inventory sync event fires (visible in logs)

---

## v0.10 — Death, Respawn + Corpse

**Goal**: Dead players respawn as new NPCs. Old avatar persists as a lootable corpse.

**Scope**:
- Player death triggers:
  - Server broadcasts `DeathEvent`
  - All clients: execute `Kill` on player avatar NPC (becomes corpse)
  - Dead player's client: death camera/overlay
- After `respawn_delay_seconds` (hardcoded 5 s): server sends respawn command
  - Player respawns at Doc Mitchell's house (hardcoded cell + position)
  - New NPC avatar spawned (new NetEntityId); old avatar remains as corpse
- Corpse persistence: server tracks corpse timer (hardcoded 300 s), sends `DespawnNPC` when expired
- Respawn mode: `keep_all` only (player keeps full inventory and stats)
- Corpse inventory: populated locally from last known `InventorySync` — not synced between clients when looted

**Hardcoded**: respawn location, respawn delay, corpse timer, respawn mode

**Not included**: configurable respawn modes (reset_inventory, etc.), multiple respawn locations

**Test**:
1. Client A kills client B → B dies, ragdoll plays on both clients
2. After 5 s, B respawns in Doc Mitchell's house with full inventory
3. B's corpse is visible to A (and to B if they return to the area)
4. After 300 s, corpse disappears
5. B has a new NPC avatar (different NetEntityId than before death)

---

## v0.11 — Persistence + Bootstrap

**Goal**: Player state survives server restarts and reconnections. Returning players are restored to their last known state. This milestone introduces the Doc Mitchell intro and base save system — prior milestones used any existing savegame.

**Scope**:
- **SQLite database** (`playerdata.db`):
  - Table: `players` — token, name, level, XP, HP, AP, location (worldspace + cell + pos + rot), SPECIAL stats, skills, perks
  - Table: `inventory` — per-player item list (formId, count, equipped, slot)
- Server persists player state: on disconnect, periodic autosave, death
- **Bootstrap on reconnect**:
  1. Server recognizes token → loads saved state
  2. Sends bootstrap packet: position, inventory, equipped items, HP, level, stats, skills, perks
  3. Client applies bootstrap (teleport to position, equip items, set stats) behind a loading screen overlay
- **Base save system** (new in this milestone):
  - First-time player plays through Doc Mitchell's intro (character creation, SPECIAL allocation)
  - On exiting Doc's house → base save created and marked as this server's entrypoint
  - No multiplayer interaction during the intro (prevents base save corruption)
  - Returning players load the base save, then server bootstraps them to correct state
  - Each server connection has its own base save

**Hardcoded**: nothing — all values from DB

**Not included**: quest/faction/world state persistence (explicitly out of scope per spec)

**Test**:
1. Player connects for first time → plays through Doc Mitchell intro → base save created
2. Player walks to Goodsprings, equips items, gains some XP
3. Player disconnects → server saves state to SQLite (verify with any SQLite browser)
4. Player reconnects → loads base save → server bootstraps to last position with same gear, level, XP, HP
5. Server restarts → player reconnects → still has their data

---

## v0.12 — Configuration + Polish

**Goal**: All hardcoded values moved to config files. Full end-to-end experience for up to 8 players.

**Scope**:
- **server.cfg** parsing — all settings from tech spec §15:
  - `port`, `max_players`, `server_name`, `tick_rate`, `snapshot_rate`
  - `respawn_mode` (all four modes functional), `respawn_delay_seconds`, `corpse_persist_seconds`
  - `inventory_autosave_seconds`, `player_data_path`
- **Crash watchdog**: `join_server.exe` monitors `FalloutNV.exe` process after launch
  - On crash/unexpected exit → sends disconnect notification to server via its own UDP connection
  - Server immediately promotes new zone owners as needed
- Max 8 simultaneous clients tested
- Edge case handling:
  - Rapid connect/disconnect cycles
  - Zone with no players → clean wipe
  - Multiple zone ownership transfers in sequence
  - Player dies while zone owner → ownership transfers, corpse persists
  - Reconnect while corpse is still in the world
- **Disconnect notice**: when server sends `MSG_DISCONNECT` (or connection is lost), client shows an in-game message box (e.g. "Disconnected from server") and then exits the game
- Remove all remaining hardcoded values (all come from config or protocol)

**Hardcoded**: nothing

**Not included**: GUI launcher (CLI only per spec), chat, anti-cheat, version negotiation (all deferred per spec)

**Test (full integration)**:
1. Configure `server.cfg` with custom port and respawn mode → server uses those values
2. 4+ players connect via launcher, enter same area → all see each other's named NPCs
3. Players walk, run, sneak, draw weapons, fire, reload → all animations visible to all others
4. Players fight NPCs → NPCs die on all clients, zone ownership transfers work
5. Player equips new gear → visible to others
6. Player dies → respawns per configured mode, corpse visible to others
7. Player disconnects and reconnects → restored to last state
8. Force-crash a client → server detects within 5 s, other players unaffected
9. Server restart → all returning players fully restored from database
10. Server shuts down while client is playing → client shows disconnect message and exits

---

## v0.13 — Advanced Animation & Combat Polish

**Goal**: Improve animation fidelity with weapon-specific animations, additional combat visuals, and movement animations that were deferred from v0.5.

**Scope**:
- Weapon-specific reload variants (ReloadB, ReloadC, etc. — some weapons use different reload animations than ReloadA)
- Per-weapon thrown animation mapping (spears use AttackThrow7, grenades use AttackThrow, etc. instead of universal AttackThrow6)
- Multiple melee attack patterns and blocking (v0.5 uses only AttackRight for all melee)
- Automatic weapon sustained fire: `GetAnimAction` returns 2 for the entire duration of automatic fire, so only the first shot is detected. Needs a different approach (e.g. timer-based repeated fire events while animAction stays 2)
- Muzzle flash for ranged weapon attacks (verify not already present from `PlayGroup Attack` — may need explicit particle attach)
- Bullet impact decals (bullet holes on hit surfaces) and ejected shell casings from weapon
- Jump animations (JumpStart, JumpLoop, JumpLand)
- Stagger / hit reaction / damage-taking animations
- Non-human creature attack variety (v0.5 uses only AttackRight for all creatures)

**Hardcoded**: nothing

**Not included**: lip sync, facial expressions, ragdoll sync, fine rotation (pitch/roll)

**Test**:
1. Player reloads different weapon types → each plays the correct reload animation variant
2. Player throws a spear → correct throw animation plays. Player throws a grenade → different animation
3. Player uses melee weapon → varied attack patterns, not just AttackRight every time
4. Player fires automatic weapon → sustained fire animation with repeated shots on remote NPC (not just one shot)
5. Player fires ranged weapon → muzzle flash visible on remote NPC (if not already triggered by attack animgroup)
6. Player fires ranged weapon → bullet holes appear on hit surfaces, shell casings eject from weapon on remote NPC
7. Player jumps → remote NPC plays jump start/loop/land sequence
8. Player takes damage → remote NPC shows hit reaction

---

## Milestone Dependency Graph

```
v0.1  Network Foundation
  │
  v
v0.2  Player Snapshot Pipeline
  │
  v
v0.3  Remote Player Avatar
  │
  v
v0.4  Interpolation
  │
  v
v0.5  Animation Sync
  │
  ├──────────────────┐
  v                  v
v0.6  Launcher    v0.7  Zone Ownership + NPC Sync
  │                  │
  │                  v
  │               v0.8  Combat
  │                  │
  │                  v
  │               v0.9  Equipment + Inventory Sync
  │                  │
  │                  v
  │              v0.10  Death, Respawn + Corpse
  │                  │
  ├──────────────────┘
  v
v0.11  Persistence + Bootstrap
  │
  v
v0.12  Configuration + Polish
  │
  v
v0.13  Advanced Animation & Combat Polish
```

v0.6 (Launcher) and v0.7–v0.10 (gameplay systems) can be developed in parallel
after v0.5 is complete. They converge at v0.11 where persistence ties everything together.
v0.13 is an optional polish milestone after the core v1 feature set is complete.
