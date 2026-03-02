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
- Client sends `PlayerSnapshot` at **20 Hz** (pos + rot + cellId; `movementState` hardcoded to `Idle`, `weaponFormId = 0`, `actionState = None`)
- Server receives `PlayerSnapshot`, logs position to stdout (behind `--verbose` flag; off by default to avoid spam in later milestones)
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

## v0.3 — Two-Client Relay + Remote Player Avatar

**Goal**: Server relays snapshots between two clients. Each client spawns a visible NPC representing the other player.

**Scope**:
- Server packs stored snapshots into `WorldSnapshot`, broadcasts to all **other** connected clients at 20 Hz
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

## v0.4 — Interpolation

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

## v0.5 — Animation Sync

**Goal**: Remote NPCs play correct locomotion and combat animations instead of sliding around in T-pose.

**Scope**:
- `MovementState` sampled from local player and included in `PlayerSnapshot` (Idle, Walk, Run, Sneak, SneakWalk, SneakRun)
- `weaponFormId` sampled (currently drawn weapon FormID, 0 = holstered)
- `ActionState` sampled (None, Firing, Reloading, Melee, AimingIS)
- Receiving client applies every tick (all idempotent):
  - **Locomotion**: `PlayGroup` matching `MovementState`
  - **Weapon**: `SetWeaponOut 1/0` (JIP LN) based on `weaponFormId`
  - **Action**: `PlayGroup` for action anim if `ActionState != None`
  - **Idle stance**: `PlayGroup Aim` if weapon drawn + no action
- Console output suppression via `SafeWrite8` on `Console::Print` (already proven in prototype)

**Hardcoded**: nothing new; all state is sampled live

**Not included**: NPC animations (only player avatars), equipment visuals

**Test**:
1. Client A stands still → remote NPC in client B plays idle animation
2. Client A walks → NPC walks. Client A runs → NPC runs. Client A sneaks → NPC sneaks
3. Client A draws weapon → NPC draws weapon. Client A holsters → NPC holsters
4. Client A fires → NPC plays fire animation. Client A reloads → NPC plays reload animation
5. No console spam visible in-game during any of the above

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
  - Sends `NPCSnapshot` at 20 Hz (position, rotation, movementState per NPC)
  - Sends `DespawnNPC` when NPCs leave/die
- **Non-owner clients**:
  - Spawn matching NPC (same baseFormId) on receiving SpawnNPC
  - Disable AI on local copy (`SetActorsAI 0` — full disable; NPC copies are visual mirrors, not targetable)
  - Apply position/rotation from NPCSnapshots (with interpolation)
  - Apply basic locomotion animation (Idle/Walk/Run) from movementState
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
```

v0.6 (Launcher) and v0.7–v0.10 (gameplay systems) can be developed in parallel
after v0.5 is complete. They converge at v0.11 where persistence ties everything together.
