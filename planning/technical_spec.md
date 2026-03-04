# FNVMP Technical Specification

> **Status**: v1 — all major decisions resolved
> **Based on**: `planning/initial_plan.txt`

---

## 1. Overview

FNVMP is a multiplayer mod for Fallout: New Vegas. The game was never designed for multiplayer, so the mod works by:

- Running a standalone **server** (`server.exe`) that owns authoritative game state
- Injecting a **client plugin** (`fnvmp.dll`) via NVSE that sends/receives network data
- Using a **launcher** (`join_server.exe`) that bootstraps the game with server connection info
- Representing remote players as **AI-disabled NPCs** whose transforms and animations are driven by network data

The design prioritizes **stability and performance** over feature completeness. Anti-cheat is explicitly out of scope — this is a co-op mod for friends.

---

## 2. Architecture

```
┌──────────────────────────────────────────────────────┐
│                  SERVER (server.exe)                  │
│             C++ / cross-platform / Linux-hostable     │
│                                                      │
│  ┌──────────┐  ┌──────────┐  ┌───────────────────┐  │
│  │ Net I/O  │  │ Game     │  │ Persistence       │  │
│  │ (ENet)   │◄►│ Logic    │◄►│ (SQLite)          │  │
│  └──────────┘  └──────────┘  └───────────────────┘  │
│       ▲                                              │
└───────┼──────────────────────────────────────────────┘
        │ UDP
        ▼
┌──────────────────────────────────────────────────────┐
│             CLIENT (FNV + NVSE + fnvmp.dll)           │
│                                                      │
│  ┌──────────┐  ┌──────────┐  ┌───────────────────┐  │
│  │ Net I/O  │  │ Entity   │  │ NVSE Game         │  │
│  │ (ENet)   │◄►│ Manager  │◄►│ Interface         │  │
│  └──────────┘  └──────────┘  └───────────────────┘  │
│                     ▲                                │
│                     │                                │
│              ┌──────┴──────┐                         │
│              │Interpolation│                         │
│              │   Buffer    │                         │
│              └─────────────┘                         │
└──────────────────────────────────────────────────────┘
```

### 2.1 Components

| Component | Form | Language | Description |
|-----------|------|----------|-------------|
| `server.exe` | Standalone process | C++ (cross-platform, targets Linux for hosting) | Authoritative game state server. Does NOT run the game engine. SQLite DB must be accessible by external tools. |
| `fnvmp.dll` | NVSE plugin | C++ (Win32) | Client-side plugin loaded by NVSE. Handles networking, entity management, interpolation. |
| `join_server.exe` | CLI launcher | C++ | Simple CLI: prompts for IP (and display name on first connect). Writes `fnvmp_connect.cfg`, launches FNV via NVSE. Acts as crash watchdog. |

---

## 3. Networking

### 3.1 Transport

| Property | Value |
|----------|-------|
| Protocol | UDP |
| Library | **ENet** |
| Reliability | Unreliable (snapshots) + Reliable ordered (events) via ENet channels |

ENet rationale: provides exactly the reliability model we need (unreliable + reliable ordered channels), handles connection management, and is battle-tested in game mods. Single-header C library, cross-platform, easy to integrate into both server and client.

### 3.2 Tick Rates

| Rate | Value | Notes |
|------|-------|-------|
| Client send rate | 20 Hz | Client sends player snapshot to server 20 times/sec |
| Server broadcast rate | 20 Hz | Server relays snapshots to other clients |
| Server logic tick | 30 Hz | Server processes events, health, ownership |
| Client game tick | Engine rate (~60 Hz) | Client applies interpolated state every frame |

### 3.3 Interpolation

- **Buffer depth**: 100–150ms (stores last 2–3 snapshots)
- **Method**: Linear interpolation between known states
- **Render time**: `currentTime - 100ms` (always rendering slightly in the past)
- Remote entities are positioned using interpolated state, NOT the latest snapshot directly
- If no new snapshot arrives within timeout (~500ms), extrapolate briefly then freeze

### 3.4 Channels (ENet)

| Channel | Reliability | Usage |
|---------|-------------|-------|
| 0 | Unreliable | Position/rotation/movement snapshots |
| 1 | Reliable ordered | Game events (spawn, death, equip, damage, ownership, health) |
| 2 | Reliable ordered | System messages (auth, heartbeat, disconnect) |

### 3.5 Version Compatibility

No version negotiation in v1. Client and server must be the same build. If they're not, behavior is undefined (hope for the best).

---

## 4. Protocol

### 4.1 Packet Format

All packets share a common header:

```
┌─────────┬──────────┬─────────────────┐
│ MsgType │ Sequence │ Payload         │
│ (u8)    │ (u16)    │ (variable)      │
└─────────┴──────────┴─────────────────┘
```

Serialization: **raw packed C++ structs** for v1. All data is little-endian (both client and server are x86). Versioning can be added later if needed. Position floats may be quantized in a future optimization pass (initial plan notes this as desirable), but v1 uses raw floats for simplicity.

### 4.2 Message Types — Snapshots (Unreliable)

All entities (players and NPCs) use the same per-entity state. There is no protocol distinction between a player avatar and an NPC avatar — the receiving client treats them identically.

#### EntityState (common per-entity data)
```cpp
struct EntityState {
    uint32_t netEntityId;
    float    posX, posY, posZ;
    float    rotZ;           // yaw only for v1
    uint8_t  moveDirection;  // enum: None, Forward, Backward, Left, Right, ForwardLeft, ForwardRight, BackwardLeft, BackwardRight, TurnLeft, TurnRight
    uint8_t  isRunning;      // 1 = run mode, 0 = walk mode (only meaningful when moveDirection != None)
    uint32_t weaponFormId;   // currently drawn weapon (0 = holstered)
    uint8_t  actionState;    // bitmask: None=0, Firing=1, Reloading=2, AimingIS=4 (combinations valid, e.g. Firing|AimingIS=5)
    uint8_t  isSneaking;     // 1 = sneaking, 0 = not sneaking
};
```

#### EntitySnapshot (Client → Server)
```cpp
struct EntitySnapshot {
    uint8_t  msgType;        // MSG_ENTITY_SNAPSHOT
    uint16_t entityCount;    // 1 for local player, N for zone-owned NPCs
    // followed by entityCount × EntityState
};
```

A regular client sends this with `entityCount = 1` (their own state). A zone owner sends a second `EntitySnapshot` per tick with `entityCount = N` for their owned NPCs. Both use the same message type and struct.

#### WorldSnapshot (Server → Client)
```cpp
struct WorldSnapshot {
    uint8_t  msgType;          // MSG_WORLD_SNAPSHOT
    uint16_t entityCount;
    // followed by entityCount × EntityState
};
```

Contains all entities relevant to the receiving client (other players + NPCs in their zone). The client applies the same animation/position logic to every entity regardless of whether it represents a player or NPC.

### 4.3 Message Types — Events (Reliable)

| Event | Direction | Payload |
|-------|-----------|---------|
| `SpawnNPC` | Owner → Server → All | `{ baseFormId, posX/Y/Z, rotZ, isPersistent }` → server assigns `netEntityId` |
| `DespawnNPC` | Owner → Server → All | `{ netEntityId }` |
| `HitEvent` | Attacker → Server | `{ targetNetEntityId, damage, weaponFormId, hitLocation }` |
| `HealthUpdate` | Server → All | `{ netEntityId, currentHP, maxHP }` |
| `DeathEvent` | Owner → Server → All | `{ netEntityId }` |
| `EquipChange` | Player → Server → All | `{ netEntityId, slot, formId, isEquip }` |
| `InventorySync` | Player → Server → All | `{ netEntityId, itemCount, items[] }` |
| `CorpseSpawn` | Server → All | `{ netEntityId, posX/Y/Z, rotZ, inventorySnapshot }` |
| `OwnershipTransfer` | Server → All | `{ zoneKey, newOwnerNetEntityId }` |
| `PlayerConnect` | Server → All | `{ netEntityId, playerName }` |
| `PlayerDisconnect` | Server → All | `{ netEntityId }` |

---

## 5. Authority Model

| System | Authority | Notes |
|--------|-----------|-------|
| Player movement | Each player (client-authoritative) | Server relays, does not validate position |
| Player damage | Server | Server owns HP, validates basic sanity |
| NPC AI | Zone owner | Vanilla AI runs only on owner's client |
| NPC spawn | Zone owner | Owner reports spawns, server assigns NetEntityId |
| NPC death | Zone owner | Owner executes kill, sends DeathEvent |
| Inventory drops | Zone owner | Owner handles loot tables |
| Persistence | Server | Server stores player data |

### 5.1 Combat + Damage Flow

```
Client (attacker)          Server                    Zone Owner / Target
    │                         │                            │
    │── HitEvent ───────────►│                            │
    │   (netEntityId,         │                            │
    │    damage, weapon)      │                            │
    │                         │── Validate sanity ──┐     │
    │                         │   (distance, rate)   │     │
    │                         │◄─────────────────────┘     │
    │                         │                            │
    │                         │── Subtract HP             │
    │                         │── HealthUpdate ──────────►│
    │◄── HealthUpdate ────────│                            │
    │                         │                            │
    │                         │   [If HP <= 0:]            │
    │                         │◄── DeathEvent ─────────────│
    │◄── DeathEvent ──────────│                            │
    │   (apply kill locally)  │                            │
```

- Server owns health values
- Health updates are **events** (reliable channel), not part of snapshots
- Zone owner executes death (ragdoll, AI cleanup)
- All clients apply kill locally upon receiving DeathEvent

---

## 6. Zone Ownership

### 6.1 Definitions

- **Zone**: For interiors, a zone = single cell (`CellFormID`). For exteriors, a zone = loaded grid cluster (e.g. 3×3 cells) keyed by `(WorldspaceID, gridX, gridY)`.
- **Zone Owner**: The player whose client runs vanilla AI for NPCs in that zone and reports NPC state to the server.
- Interior and exterior zones follow identical ownership rules.

### 6.2 Ownership Rules

1. First player entering a zone becomes **Zone Owner**
2. Ownership tracked by zone key: `(WorldspaceID, gridX, gridY)` for exteriors, `CellFormID` for interiors
3. If owner leaves zone:
   - Server waits **5 seconds** (grace period for re-entry)
   - Then promotes next player present in zone (by earliest join time)
4. If owner disconnects/crashes:
   - Immediate promotion of next player (no grace period)
5. If no players remain in zone:
   - Zone state is **deleted** (NPCs forgotten, corpses forgotten)
   - Next player to enter gets fresh vanilla spawns

### 6.3 Ownership Transfer Process

1. Server sends `OwnershipTransfer` event to all clients in zone
2. New owner enables AI on local NPCs, begins sending NPC snapshots
3. Old owner (if still connected) disables AI on those NPCs
4. Brief NPC freeze (few seconds) during transfer is acceptable

### 6.4 Cell Transitions

Nothing special. When a player enters a cell/zone that doesn't contain other players, they automatically become zone owner. Standard loading screen behavior is unchanged.

---

## 7. Entity Management

### 7.1 NetEntityId

- Server-assigned `uint32_t`, globally unique across the session
- Counter starts at 1, increments per assignment (0 = invalid)
- Used in ALL network messages to refer to entities

### 7.2 Entity Types

| Type | Description |
|------|-------------|
| `Player` | Remote player avatar (restrained companion NPC) |
| `NPC` | Non-player NPC synced by zone owner |

### 7.3 Player Avatar Spawning

- Remote players are spawned as NPCs via `PlaceAtMe` or equivalent NVSE call
- Three commands are applied immediately after spawning:
  1. `SetRestrained 1` — prevents autonomous movement/combat while keeping the NPC visible to the AI system (targetable by hostile NPCs)
  2. `AddToFaction playerFaction 1` — adds to the player's faction so faction hostility propagates (if player aggros an NPC, that NPC also targets player avatars)
  3. `SetPlayerTeammate 1` — makes the NPC behave as a combat teammate of the player
- **Note**: `SetRestrained` causes the NPC to have effectively infinite health (companions go unconscious at 0 HP instead of dying). Death handling for player avatars requires temporarily clearing restrained state and applying damage via script — addressed in v0.8 (Combat).
- **Single generic base form** for all player avatars (e.g. NCR trooper `0x001206FD` as in prototype). Player appearance is not synced in v1 — only equipped items differentiate players visually.
- NPC is **renamed** to the player's display name (as entered on first server connect)
- Client maintains `NetEntityId → TESObjectREFR*` mapping

### 7.4 NPC Lifecycle

**When zone owner spawns an NPC (vanilla engine)**:
1. Owner detects new NPC in cell
2. Owner sends `SpawnNPC { baseFormId, position, rotation, isPersistent }`
3. Server assigns `NetEntityId`, broadcasts to all in zone
4. Other clients spawn exact same `BaseFormID` manually, map `NetEntityId → local ref`
5. Non-owners disable AI on their local copy (full AI disable via `SetActorsAI 0` — unlike player avatars, NPC copies don't need to be targetable; they are visual mirrors only)

**Named/persistent NPCs** (already exist in cell):
- Identified by `FormID + CellID` as identity key
- Server still wraps in `NetEntityId`
- Only zone owner runs their AI

**Leveled spawns**:
- Zone owner resolves the leveled variant (engine decides)
- Owner reports resolved `BaseFormID` + stats
- Non-owners spawn exact same `BaseFormID`, apply health/stats from owner
- Non-owners ignore engine's own leveled resolution

**NPC despawn**:
- When zone has no players: NPCs forgotten entirely
- When new owner loads zone: fresh vanilla spawns
- No long-term NPC world persistence

---

## 8. Player Connection Flow

### 8.1 First-Time Setup (Base Save)

```
┌─────────────────────────────────────────────────────┐
│ 1. Player runs join_server.exe                      │
│ 2. CLI prompts for server IP                        │
│ 3. No stored token for this server → prompts for    │
│    display name                                     │
│ 4. Launcher writes fnvmp_connect.cfg with IP, port, │
│    and display name                                 │
│ 5. Launcher starts FNV via NVSE                     │
│ 6. fnvmp.dll loads, reads fnvmp_connect.cfg,        │
│    connects to server                               │
│ 7. Server sees: no token → new player               │
│ 8. Player plays through Doc Mitchell's intro        │
│    (character creation, SPECIAL allocation)          │
│ 9. On exiting Doc's house → base save created       │
│ 10. fnvmp.dll marks this save as server entrypoint  │
│ 11. Server issues auth token, client stores it      │
│ 12. Player is now "online" — other players become   │
│     visible, data starts syncing                    │
└─────────────────────────────────────────────────────┘
```

Key points:
- No multiplayer interaction during the intro → prevents base save corruption
- Base save is local to the client, server does NOT store or serve savegames
- Each server connection has its own base save

### 8.2 Returning Player

```
┌─────────────────────────────────────────────────────┐
│ 1. Player runs join_server.exe                      │
│ 2. CLI prompts for server IP                        │
│ 3. Stored token found for this server → no name     │
│    prompt                                           │
│ 4. Launcher writes fnvmp_connect.cfg with IP, port, │
│    and token                                        │
│ 5. FNV starts, loads base save for that server      │
│ 6. fnvmp.dll connects, presents stored token        │
│ 7. Server recognizes token → loads player state     │
│ 8. Server sends bootstrap data:                     │
│    - Teleport to last known position                │
│    - Equip stored inventory                         │
│    - Set HP/level/stats/skills/perks                │
│ 9. Client applies bootstrap (with loading screen    │
│    overlay to hide teleport)                        │
│ 10. Player is online                                │
└─────────────────────────────────────────────────────┘
```

### 8.3 Identity & Tokens

- Server generates a random token (e.g. UUID v4) on first connect
- Client stores token in a file in `Data/NVSE/Plugins/fnvmp/` directory, keyed by server IP
- Token presented on all future connections for identification
- Token is NOT a security mechanism — it's an identity handle for co-op play
- If token is lost, player starts fresh (new character)

### 8.4 Connection Info File

`fnvmp_connect.cfg` is written by the launcher to `Data/NVSE/Plugins/fnvmp_connect.cfg`:

```
ip=192.168.1.100
port=7777
token=a1b2c3d4-e5f6-7890-abcd-ef1234567890
name=PlayerOne
```

- Plugin reads this file on load; if missing, plugin does nothing (single-player mode)
- Plugin deletes the file after reading to avoid stale state

---

## 9. Player Faction Model ("Single Faction")

All connected players are effectively companions to each other:

- When player A attacks an NPC, the NPC should also become hostile to players B, C, etc.
- Implementation: four commands on each spawned player avatar NPC:
  1. `SetRestrained 1` — prevents autonomous movement (position driven by network data) while remaining visible to the AI system so hostile NPCs can target it
  2. `SetCombatDisabled 1` — prevents the NPC from initiating combat when `SetRestrained` is temporarily lifted to allow animations (weapon draw/holster)
  3. `AddToFaction playerFaction 1` — adds to the player's faction; leverages the engine's faction hostility system so aggro propagates to all player avatars
  4. `SetPlayerTeammate 1` — NPC behaves as a combat teammate of the player for targeting/combat purposes
- **Verified in-game**: hostile NPCs correctly target both the local player and restrained player-faction teammate NPCs
- **Known limitation**: `SetRestrained` prevents death (companion unconscious behavior). Death handling requires temporarily clearing restrained state — see §7.3 note and v0.8 milestone.
- **Known limitation**: `SetRestrained` blocks many animations including weapon draw/holster. Any animation that requires the AI loop to process (weapon state changes, possibly others) will be silently ignored. The workaround is to temporarily drop `SetRestrained 0`, trigger the animation, poll `IsWeaponOut` each tick, and re-apply `SetRestrained 1` once the state change is confirmed. `SetCombatDisabled 1` + per-tick `SetPos` mitigate the unrestrained window. See §10.1 for full draw/holster procedure.

---

## 10. Animation Sync

v0.5 is split into two sub-phases that are implemented together. These apply to all synced avatars — both remote player NPCs and zone-owner-reported NPCs:
- **Detection**: Sampling animation state from the local player and from zone-owned NPCs. Some states are derived from movement (position delta vs facing direction). Some states are polled from engine functions (`IsRunning`, `IsSneaking`, `IsAiming`, `IsAttacking`, `IsWeaponOut`, `GetAnimAction`). Fully researched — see §10.4.
- **Application**: Applying received animation state to remote avatars via `PlayGroup` and related commands. Fully researched — see §10.1.

### 10.1 Application — All Avatars (Players and NPCs)

#### Phase 1 — Basic Locomotion

Basic locomotion animations all loop (e.g. `TurnLeft` will loop until another basic locomotion animation state is set). All work while restrained — no unrestrain workaround needed.

- If actor is turning on the spot, use `TurnLeft` / `TurnRight`.
- If actor is strafing (going left/right without going forward or backward), use just `Left` / `Right` (or `FastLeft` / `FastRight` if running).
- If actor is moving diagonally (forward/backward + left/right), use both animgroups at the same time (e.g. `Forward` + `Left`, or `FastForward` + `FastLeft` if running).

| Animation | AnimGroup |
|-----------|-----------|
| Idle | `Idle` (0x00) |
| Walk forward | `Forward` (0x03) |
| Walk backward | `Backward` (0x04) |
| Strafe left | `Left` (0x05) |
| Strafe right | `Right` (0x06) |
| Run forward | `FastForward` (0x07) |
| Run backward | `FastBackward` (0x08) |
| Run strafe left | `FastLeft` (0x09) |
| Run strafe right | `FastRight` (0x0A) |
| Diagonal forward-left | `Forward` (0x03) + `Left` (0x05) |
| Diagonal forward-right | `Forward` (0x03) + `Right` (0x06) |
| Diagonal backward-left | `Backward` (0x04) + `Left` (0x05) |
| Diagonal backward-right | `Backward` (0x04) + `Right` (0x06) |
| Run diagonal forward-left | `FastForward` (0x07) + `FastLeft` (0x09) |
| Run diagonal forward-right | `FastForward` (0x07) + `FastRight` (0x0A) |
| Run diagonal backward-left | `FastBackward` (0x08) + `FastLeft` (0x09) |
| Run diagonal backward-right | `FastBackward` (0x08) + `FastRight` (0x0A) |
| Turn left (on spot) | `TurnLeft` (0x0F) |
| Turn right (on spot) | `TurnRight` (0x10) |

Diagonal combinations: play both animgroups on the same tick.

#### Phase 2 — Sneak

`SetForceSneak 1` works, but `SetRestrained` must be set to `0` first (same unrestrain workaround pattern as weapon draw/holster).

- After entering or exiting sneak, the actor naturally continues whatever locomotion animgroup was active before — there is no need to do `PlayGroup Idle` after `SetForceSneak`.
- Basic locomotion animgroups (Idle, Forward, FastForward, etc.) are applied normally while sneaking.
- `IsSneaking` can be used to poll whether the actor has entered/exited sneak mode, similarly to the `IsWeaponOut` workaround for holster/unholster. However, `IsSneaking` returns the strings `"is sneaking"` / `"is not sneaking"` instead of `1` / `0`.

**Sneak enter**: `SetRestrained 0` → `SetForceSneak 1` → each tick poll `IsSneaking` → when returns `"is sneaking"`, `SetRestrained 1`

**Sneak exit**: `SetRestrained 0` → `SetForceSneak 0` → each tick poll `IsSneaking` → when returns `"is not sneaking"`, `SetRestrained 1`

#### Weapon Draw/Holster

> `SetRestrained 1` suppresses the AI loop, which blocks `SetWeaponOut` and any animation requiring AI processing. The solution is to temporarily unrestrain the actor, trigger the animation, and poll `IsWeaponOut` each tick to re-restrain as soon as the state change is confirmed.
>
> Prerequisites (set once at NPC spawn): `SetCombatDisabled 1` (prevents combat during unrestrained windows). Per-tick `SetPos` (already applied for interpolation) corrects any autonomous movement.
>
> **Draw**: `SetRestrained 0` → `SetWeaponOut 1` → `SetAlert 1` (all same tick, in order) → each tick check `IsWeaponOut` → when returns 1, `SetRestrained 1`
>
> **Holster**: `SetRestrained 0` → `SetWeaponOut 0` → `SetAlert 0` (all same tick, in order) → each tick check `IsWeaponOut` → when returns 0, `SetRestrained 1`
>
> `IsWeaponOut` updates before the animation visually finishes, but re-applying `SetRestrained 1` at that point is safe — the animation continues to completion. `SetAlert 1` keeps the weapon unholstered after draw; `SetAlert 0` allows holster to proceed.

#### Phase 3/4 — Upper-Body Actions

Most upper-body animations are independent of locomotion animations and can run at the same time (e.g. `AimIS` is kept despite changing from Idle to Forward and then back to Idle). They are also independent of `SetRestrained` — no unrestrain workaround needed for combat animations.

Before the correct animation can be applied, the receiving client checks the weapon type via `GetEquippedWeaponType` (ShowOff NVSE) on the `weaponFormId` to determine melee vs ranged vs thrown (0–2 = melee, 3–9 = ranged, 10–13 = thrown). Detection of *when* the actor is performing an action is handled by polling engine functions — see §10.4.

##### Non-melee weapons (pistols, rifles)

While a non-melee weapon is unholstered:
- **Aiming**: `PlayGroup AimIS 1` (actor starts aiming down the sights). Stopped with `PlayGroup Aim 1` (actor lowers their weapon into normal position). Works independent of `SetRestrained`, no further action needed.
- **Reloading**: `PlayGroup ReloadA 1` for most weapons tested (also independent of `SetRestrained`). Some weapons have a slightly different animation (e.g. `ReloadB`) — leave weapon-specific reload variants for v0.13.
- **Hip-fire (not aiming)**: `PlayGroup AttackLeft 1`.
- **Aimed fire**: `PlayGroup AttackLeftIS 1`, however the weapon gets lowered out of aim-down-sights as soon as the animation completes. To maintain AimIS after firing, chain: `PlayGroup AttackLeftIS 1` → `PlayGroup AimIS 0` (the `0` flag ensures the previous animation completes before starting the new one, so as soon as `AttackLeftIS` finishes the actor returns to `AimIS` state).
- **Sound**: `ReloadA` comes with sound by default. `AttackLeft` / `AttackLeftIS` are animation only — no sound or muzzle flash. For sound, use:
  ```
  ref rWeapon
  ref rSound
  set rWeapon to actor.GetEquippedWeapon
  set rSound to GetWeaponSound rWeapon 0   ; 0 = "Attack Sound 3D"
  actor.PlaySound3D rSound
  ```
  For every shot the actor takes (single shot or one round of automatic fire), both `AttackLeft`/`AttackLeftIS` and sound need to be played.
- **Muzzle flash**: Not included — deferred to v0.13.
- **Automatic weapons**: Server can handle this in a more optimized way so shooting is more smooth — deferred to v0.13.

##### Melee weapons (swords, knives, power gloves)

Melee weapons are complicated (multiple different attack patterns, blocking, etc.), but for now `PlayGroup AttackRight 1` is good enough for both one-handed and two-handed melee weapons. Leave the rest for v0.13.

##### Thrown weapons (spears, grenades)

Thrown weapon animation is highly dependent on the weapon itself — e.g. spears use `AttackThrow7`, grenades use `AttackThrow`. For now, use `PlayGroup AttackThrow6 1` for all thrown weapons. Leave per-weapon mapping for v0.13.

##### Non-human attacks (creatures/animals)

Like melee weapons, there are multiple different attack types — leave variety for v0.13. For now:
- A creature/animal needs to "unholster" their "weapon" first (i.e. enter alert combat state) before attack animations can be played.
- `PlayGroup AttackRight 1` works well enough as a universal placeholder.
- Example: for a dog attack, the unarmed weapon must first be unholstered (dog enters alert combat state), then attack animation can play.
- There doesn't need to be a special check for whether the NPC is human or not — just checking for alert state and whether a melee weapon is equipped is enough.
- For NPCs like dogs, `GetEquippedWeapon` shows "Fists" (despite the actual animation being biting).

#### Not synced in v0.5 (deferred to v0.13)

Jump animations, stagger / hit reaction / damage-taking animations, muzzle flash, weapon-specific reload variants (ReloadB, ReloadC, etc.), per-weapon thrown animations, multiple melee attack patterns and blocking, automatic weapon sustained fire optimization, non-human creature attack variety. Also not synced: fine rotation, lip sync, facial expressions.

### 10.2 NPC Animation Scope

The animation techniques in §10.1 apply equally to zone-owner-synced NPCs. The zone owner samples NPC state (detection) and non-owner clients apply it (application) using the same PlayGroup calls.

- Zone owner sends `EntitySnapshot` containing per-NPC `EntityState` (same fields as player entities)
- Non-owners apply position directly and set matching animations using the same engine application logic (§10.3)
- v0.5 proves detection (§10.4) and application (§10.1–§10.3) on the local player; v0.7 extends the same detection to zone-owned NPCs
- Combat targeting: zone owner sends target NetEntityId per NPC so non-owners can orient the NPC toward the correct target

### 10.3 Implementation

Two distinct layers:

**Plugin state layer** — tracks authoritative state from network snapshots. Pure data, no engine calls:
- `moveDirection` — movement direction relative to facing (None, Forward, Backward, Left, Right, ForwardLeft, ForwardRight, BackwardLeft, BackwardRight, TurnLeft, TurnRight)
- `isRunning` — walk vs run mode (determines Walk vs Fast animation variants)
- `actionState` — current upper-body action (bitmask: None, Firing, Reloading, AimingIS — combinations like Firing|AimingIS = aimed fire)
- `weaponFormId` — currently drawn weapon (0 = holstered)
- `isSneaking` — sneak mode flag

**Engine application layer** — translates plugin state into engine calls each tick:

| Plugin state | Engine call | Notes |
|---|---|---|
| `moveDirection` + `isRunning` | `PlayGroup <AnimGroup>` | Every tick; all locomotion anims loop. Direction selects the animgroup(s), isRunning selects walk vs fast variant. Combine two animgroups for diagonals (e.g. `Forward` + `Left`). See §10.1 Phase 1 table |
| `isSneaking` changed | Unrestrain → `SetForceSneak` → poll `IsSneaking` (returns text, not 0/1) → re-restrain | Same pattern as weapon draw/holster. Locomotion anims continue naturally after sneak toggle |
| `weaponFormId` changed | Unrestrain → `SetWeaponOut` + `SetAlert` → poll `IsWeaponOut` → re-restrain | See §10.1 weapon draw/holster |
| `actionState` has Firing (ranged, no AimingIS) | `PlayGroup AttackLeft 1` + `PlaySound3D` (weapon sound) | Sound via `GetWeaponSound weapon 0`. Per shot. Weapon type from `GetEquippedWeaponType` on `weaponFormId` |
| `actionState` has Firing + AimingIS (ranged) | `PlayGroup AttackLeftIS 1` → `PlayGroup AimIS 0` + `PlaySound3D` | Chain ensures return to AimIS after attack |
| `actionState` has Firing (melee) | `PlayGroup AttackRight 1` | Works for both 1H and 2H melee. Weapon type: `GetEquippedWeaponType` returns 0–2 |
| `actionState` has Firing (thrown) | `PlayGroup AttackThrow6 1` | Universal placeholder. Weapon type: `GetEquippedWeaponType` returns 10–13 |
| `actionState` has Firing (creature) | Alert/unholster + `PlayGroup AttackRight 1` | Check alert state + melee weapon, not human/non-human |
| `actionState` == AimingIS only | `PlayGroup AimIS 1` | Independent of SetRestrained and locomotion |
| `actionState` has Reloading | `PlayGroup ReloadA 1` | Independent of SetRestrained. Has built-in sound. Highest priority — overrides firing/aiming |
| `actionState` == None, weapon drawn | `PlayGroup Aim 1` | Weapon lowered idle stance |
| `actionState` == None, no weapon | Just locomotion | No upper-body override |

Weapon type classification on the receiving side uses `GetEquippedWeaponType` (ShowOff NVSE) on the received `weaponFormId`: 0–2 = melee → `AttackRight`, 3–9 = ranged → `AttackLeft`/`AttackLeftIS`, 10–13 = thrown → `AttackThrow6`.

Console output suppression via `Console::Print` patch (already implemented in prototype).

### 10.4 Detection (State Sampling)

Detection runs **on the sender only** — the client that "owns" an actor polls its state and includes the results in the `EntityState` sent to the server. For the local player character, this is every client (v0.5). For zone-owned NPCs, this is the zone owner (v0.7). Receiving clients **never poll these functions** for remote actors — they apply the state received in snapshots via the engine application layer (§10.3).

The same detection logic and functions apply to both players and NPCs. All functions below work for both.

**Plugin dependencies**: ShowOff NVSE (`IsAiming`, `GetEquippedWeaponType`), JIP LN NVSE (`IsAttacking`). Both are common dependencies used by many mods.

#### Movement direction (inferred from position + rotation)

Each tick, compute position delta (current pos − previous pos) and rotation delta (current rotZ − previous rotZ):

1. If position delta magnitude < idle threshold:
   - If rotation delta > turn threshold → `TurnLeft` or `TurnRight` (based on sign)
   - Else → `None` (idle)
2. If position delta magnitude ≥ idle threshold:
   - Compute angle between movement vector and current `rotZ`
   - Map to 8-direction bin (45° sectors): Forward (~0°), ForwardRight (~45°), Right (~90°), BackwardRight (~135°), Backward (~180°), BackwardLeft (~-135°), Left (~-90°), ForwardLeft (~-45°)

This works correctly regardless of simultaneous mouse rotation — the relative angle between instantaneous movement direction and instantaneous facing direction is always correct.

#### State flags (polled per tick)

| Function | Source | Returns | Maps to |
|---|---|---|---|
| `IsRunning` | Base game | 0/1 | `isRunning` flag. Note: returns 1 in "run mode" even when standing still (Caps Lock toggles). Only meaningful when combined with movement detection |
| `IsSneaking` | Base game | 0/1 | `isSneaking` flag |
| `IsWeaponOut` | Base game | 0/1 | When changed: update `weaponFormId` via `GetEquippedWeapon`. 0 → holstered (`weaponFormId = 0`) |
| `IsAiming` | ShowOff NVSE | 0/1 | `ActionAimingIS` bit in `actionState`. Stays 1 during aimed fire (both `IsAiming` and `IsAttacking` are 1 simultaneously) |
| `IsAttacking` | JIP LN NVSE | 0/1 | `ActionFiring` bit in `actionState`. Duration-based: stays 1 for the full attack. Semi-auto/melee/thrown: one 0→1 transition = one attack. Automatic: stays 1 while firing |
| `GetAnimAction` | Base game | int | Only used for reload detection: value `9` = `ActionReloading` in `actionState` |

#### ActionState priority (per tick)

`actionState` is a bitmask built from the above flags:
1. If `GetAnimAction == 9` → `ActionReloading` (highest priority, overrides all)
2. If `IsAttacking == 1` → set `ActionFiring` bit
3. If `IsAiming == 1` → set `ActionAimingIS` bit
4. Both `ActionFiring | ActionAimingIS` = aimed fire (receiver plays `AttackLeftIS` → `AimIS`)
5. `ActionFiring` alone = hip fire (receiver plays `AttackLeft`)
6. `ActionAimingIS` alone = just aiming (receiver plays `AimIS`)
7. Neither = `ActionNone`

#### Weapon type classification (receiving side only)

The sender transmits `weaponFormId`. The receiver calls `GetEquippedWeaponType` (ShowOff NVSE) on the weapon form to determine attack animgroup:

| `GetEquippedWeaponType` | Category | Attack AnimGroup |
|---|---|---|
| 0 (HandToHandMelee) | Melee | `AttackRight` |
| 1 (OneHandMelee) | Melee | `AttackRight` |
| 2 (TwoHandMelee) | Melee | `AttackRight` |
| 3 (OneHandPistol) | Ranged | `AttackLeft` / `AttackLeftIS` |
| 4 (OneHandPistolEnergy) | Ranged | `AttackLeft` / `AttackLeftIS` |
| 5 (TwoHandRifle) | Ranged | `AttackLeft` / `AttackLeftIS` |
| 6 (TwoHandAutomatic) | Ranged | `AttackLeft` / `AttackLeftIS` |
| 7 (TwoHandRifleEnergy) | Ranged | `AttackLeft` / `AttackLeftIS` |
| 8 (TwoHandHandle) | Ranged | `AttackLeft` / `AttackLeftIS` |
| 9 (TwoHandLauncher) | Ranged | `AttackLeft` / `AttackLeftIS` |
| 10 (OneHandGrenade) | Thrown | `AttackThrow6` |
| 11 (OneHandMine) | Thrown | `AttackThrow6` |
| 12 (OneHandLunchboxMine) | Thrown | `AttackThrow6` |
| 13 (OneHandThrown/Spears) | Thrown | `AttackThrow6` |

---

## 11. Death & Respawn

### 11.1 Player Death

1. Server detects player HP <= 0 (via HealthUpdate after HitEvent)
2. Server broadcasts `DeathEvent` for that player
3. All clients: execute `Kill` on the player's NPC avatar → engine handles ragdoll locally
4. Dead player's client sees death (camera effect, etc.)

### 11.2 Corpse Handling

- Ragdoll is NOT synced — each client's engine handles it independently
- Corpse persists for a **configurable duration** (server setting, default 300 seconds)
- Corpse inventory is **local only** — each client already knows the dead player's inventory (from prior EquipChange/InventorySync events), so all clients see the same loot on the corpse. However, removing items from the corpse is NOT synced between clients in v1.
- If dead player re-enters the area, they see their own corpse (spawned from server data)

### 11.3 Respawn

- Dead player respawns as a **new NPC** (new NetEntityId) — old avatar remains as corpse
- **Respawn location**: Doc Mitchell's house (interior cell)
- Respawn behavior is **server-configurable**:

| Setting | Default | Notes |
|---------|---------|-------|
| `respawn_mode` | `keep_all` | See respawn modes below |
| `corpse_persist_seconds` | `300` | How long corpse NPC stays in the world |
| `respawn_delay_seconds` | `5` | Delay before respawn |

#### Respawn Modes

| Mode | Description |
|------|-------------|
| `keep_all` | Respawn with full inventory and current levels and perks |
| `reset_inventory` | Respawn with current levels and perks, but inventory reset to whatever was in the base savegame |
| `empty_inventory` | Respawn with current levels and perks, but empty inventory |
| `full_reset` | Respawn from base savegame, including resetting levels and perks back to base savegame |

---

## 12. Equipment & Inventory Sync

### 12.1 Sync Triggers

Equipment/inventory is synced as **reliable events**, NOT as part of position snapshots. Sync happens on:

| Trigger | Description |
|---------|-------------|
| Container open/close | Player opens or closes any container (including own inventory Pip-Boy) |
| Cell enter | Player enters a new cell |
| Death | Full inventory snapshot sent on death |
| Periodic autosave | Every few minutes if no other sync event occurred |

### 12.2 Sync Data

`InventorySync` event contains:
```cpp
struct InventorySync {
    uint8_t  msgType;       // MSG_INVENTORY_SYNC
    uint32_t netEntityId;
    uint16_t itemCount;
    // followed by itemCount × InventoryItem:
    struct InventoryItem {
        uint32_t formId;
        uint16_t count;
        uint8_t  isEquipped;  // 1 if currently equipped
        uint8_t  slot;        // equipment slot if equipped
    };
};
```

### 12.3 Visual Equipment

When an `InventorySync` or `EquipChange` event is received for a remote player, the client:
1. Updates the remote player NPC's inventory
2. Equips/unequips the appropriate items so the NPC visually matches

---

## 13. Persistence

### 13.1 What Is Persisted (per player)

| Field | Type | Notes |
|-------|------|-------|
| Token ID | string (UUID) | Primary identifier |
| Player name | string | Display name (set on first connect) |
| Level | uint16 | |
| XP | uint32 | |
| Current HP | float | |
| Max HP | float | |
| Current AP | float | |
| Location | WorldspaceID + CellID + posX/Y/Z + rotZ | Last known position |
| Inventory | Array of `{ formId, count }` | |
| Equipped items | Array of `{ formId, slot }` | |
| SPECIAL stats | 7 × uint8 | S.P.E.C.I.A.L. |
| Skills | Map of skill → value | All 13 skills |
| Perks | Array of formId | All acquired perks |

### 13.2 What Is NOT Persisted

- Quest states
- Faction reputation
- Cleared areas
- Container contents (other than player inventory)
- World modifications (moved objects, opened doors, picked locks)
- NPC state (forgotten when zone empties)
- Corpses (forgotten when zone empties or timer expires)
- Vendor inventories

### 13.3 Storage Backend

**SQLite** — single file, zero-config, handles concurrent reads, cross-platform, easy to inspect with any SQLite browser. The database file must be accessible by external tools (admin scripts, backup utilities, etc.) while the server is running.

---

## 14. World State Philosophy

The world is **local to each client**. Nothing about the world itself is synced or persisted. This means:

| Scenario | Behavior |
|----------|----------|
| Player picks a lock | Lock resets when zone resets (no players present) |
| Player picks up a world item | Other players can also pick up the same item (duplication) |
| Quest changes the world | Each client sees their own local quest state; inconsistencies accepted |
| Vendor inventory | Each player has independent vendor stock; not synced |
| Container contents | Local only; not synced between players |
| Doors/traps | Local only |

This is explicitly acceptable for a co-op mod. The alternative (syncing world state) is a massive engineering effort with diminishing returns for friend groups.

---

## 15. Server Configuration

Server reads a config file at startup (`server.cfg`):

```
port = 7777
max_players = 8
server_name = "My FNV Server"
tick_rate = 30
snapshot_rate = 20

respawn_mode = "keep_all"
respawn_delay_seconds = 5
corpse_persist_seconds = 300

inventory_autosave_seconds = 180

player_data_path = "./playerdata.db"
```

Max players: **8** — practical limit due to FNV engine constraints. More than 8 NPCs in a cell with per-tick updates is unlikely to remain stable.

---

## 16. Client Plugin Architecture (fnvmp.dll)

### 16.1 Module Breakdown

```
fnvmp.dll
├── main.cpp              // NVSE entry points, message handler
├── net/
│   ├── net_client.h/cpp  // ENet wrapper, connect/disconnect/send/recv
│   └── protocol.h        // All packet structs and message type enums
├── game/
│   ├── entity_manager.h/cpp  // NetEntityId ↔ TESObjectREFR* mapping
│   ├── player_avatar.h/cpp   // Spawn/despawn/update remote player NPCs
│   ├── npc_sync.h/cpp        // Zone owner NPC reporting + non-owner NPC updates
│   ├── interpolation.h/cpp   // Snapshot buffer + lerp logic
│   └── animation.h/cpp       // PlayGroup calls, MovementState + ActionState → anim mapping
├── systems/
│   ├── zone_ownership.h/cpp  // Track current zone, ownership state
│   ├── combat.h/cpp          // Hit detection, HitEvent sending
│   ├── inventory_sync.h/cpp  // Equipment/inventory sync logic + triggers
│   └── persistence.h/cpp     // Token storage, bootstrap application
└── util/
    ├── console_patch.h/cpp   // Console::Print suppression
    └── timing.h/cpp          // High-res timer, accumulator for send rates
```

### 16.2 Main Loop Integration

Plugin hooks into NVSE's `kMessage_MainGameLoop`:

```
Every game tick (~60 Hz):
├── Read incoming network packets (non-blocking)
│   ├── Apply snapshot updates to interpolation buffer
│   └── Process reliable events (spawn, death, equip, health, etc.)
├── Update interpolation for all remote entities
│   └── Set position/rotation/animation for each remote NPC
├── Sample local player state
├── Check inventory sync triggers (container close, cell change, etc.)
└── If send timer elapsed (20 Hz):
    ├── Send EntitySnapshot (self) to server
    └── If zone owner: send EntitySnapshot (owned NPCs) to server
```

---

## 17. Launcher (join_server.exe)

### 17.1 Behavior (CLI for v1)

```
$ join_server.exe
FNVMP Launcher
Enter server IP: 192.168.1.100
Enter server port [7777]:
No saved token for 192.168.1.100 — first time connecting.
Enter display name: Luka
Launching Fallout: New Vegas...
Watching process...
```

1. Prompt for server IP and port (port defaults to 7777)
2. Check for stored token for this server IP
   - If no token: prompt for display name
   - If token exists: skip name prompt
3. Write `fnvmp_connect.cfg` to plugin directory
4. Launch `FalloutNV.exe` via NVSE loader
5. Monitor the game process (crash watchdog)

### 17.2 Connection Info Passing

File-based: launcher writes `Data/NVSE/Plugins/fnvmp_connect.cfg`, plugin reads it on load and deletes after reading.

---

## 18. Crash Containment

### 18.1 Watchdog Process

`join_server.exe` monitors the game process after launch:

- If `FalloutNV.exe` crashes or exits unexpectedly:
  - Sends disconnect notification to server (launcher maintains its own UDP connection for this purpose)
  - Server immediately promotes new zone owner where applicable
- If server doesn't receive heartbeat for **5 seconds**:
  - Considers player disconnected
  - Promotes new zone owner
  - NPC freeze during transition is acceptable

### 18.2 Heartbeat

- Client sends heartbeat every **2 seconds** on reliable channel
- Server tracks last heartbeat per player
- Timeout: **5 seconds** without heartbeat = disconnect

---

## 19. Chat System

No chat system in v1. Players communicate via external tools (Discord, etc.).

---

## 20. Implementation Phases

### Phase 1: Foundation
- [ ] Set up ENet integration in both client and server
- [ ] Define protocol structs and message types
- [ ] Client: basic connect/disconnect/heartbeat
- [ ] Server: accept connections, track connected players
- [ ] Player token generation and storage
- [ ] `fnvmp_connect.cfg` read/write

### Phase 2: Player Sync
- [ ] Client: sample local player position/rotation/movement each tick
- [ ] Client: send EntitySnapshot at 20 Hz
- [ ] Server: relay snapshots to other connected clients
- [ ] Client: spawn remote player avatar NPCs (restrained, playerFaction, teammate, named)
- [ ] Client: interpolation buffer + lerp positioning
- [ ] Client: basic animation sync (idle, walk, weapon stance)

### Phase 3: Zone Ownership + NPC Sync
- [ ] Server: zone tracking and ownership assignment
- [ ] Client: detect zone entry/exit, report to server
- [ ] Zone owner: detect and report NPC spawns
- [ ] Zone owner: send NPC snapshots at 20 Hz
- [ ] Non-owners: spawn + position NPCs from network data
- [ ] Ownership transfer on owner departure

### Phase 4: Combat
- [ ] Client: detect hits, send HitEvent
- [ ] Server: health tracking, sanity validation, HealthUpdate broadcast
- [ ] Zone owner: death execution + DeathEvent
- [ ] Client: apply remote deaths (Kill command)

### Phase 5: Inventory & Equipment Sync
- [ ] Client: detect sync triggers (container close, cell enter, death, periodic)
- [ ] Client: send InventorySync / EquipChange events
- [ ] Client: apply equipment to remote player NPCs
- [ ] Server: store inventory in persistence layer

### Phase 6: Persistence + Bootstrap
- [ ] Server: SQLite player data storage (all fields from §13.1)
- [ ] Server: bootstrap data on reconnect
- [ ] Client: apply bootstrap (teleport, equip, set stats/skills/perks)
- [ ] Base save detection and management

### Phase 7: Death & Respawn
- [ ] Server: respawn logic (Doc Mitchell's house, keep equipment)
- [ ] Client: corpse NPC spawning
- [ ] Corpse persistence timer
- [ ] New NPC avatar on respawn

### Phase 8: Launcher
- [ ] join_server.exe CLI
- [ ] Token storage per server
- [ ] fnvmp_connect.cfg generation
- [ ] Game launch via NVSE
- [ ] Crash watchdog

---

## Appendix A: Resolved Decisions

| # | Topic | Decision |
|---|-------|----------|
| 1 | Server language | C++ (cross-platform, Linux-hostable) |
| 2 | Networking library | ENet |
| 3 | Serialization format | Raw packed structs for v1 |
| 4 | Persistence storage | SQLite |
| 5 | Token storage location | File in `Data/NVSE/Plugins/fnvmp/` |
| 6 | Connection info passing | File (`fnvmp_connect.cfg`) |
| 7 | Player avatar base form | Single generic (NCR trooper), equipment-only differentiation |
| 8 | Faction mechanism | `SetRestrained 1` + `AddToFaction playerFaction 1` + `SetPlayerTeammate 1` |
| 9 | Corpse loot sync | No sync; corpse has inventory locally but removing items is not synced |
| 10 | NPC weapon anim sync | Position/rotation only; no explicit firing animation sync |
| 11 | Max player count (v1) | 8 |
| 12 | Persist skills/perks | Yes |
| 13 | Launcher | C++ CLI for v1 |
| 14 | Health sync | Events (reliable), not snapshots |
| 15 | Respawn location | Doc Mitchell's house, configurable mode (keep_all / reset_inventory / empty_inventory / full_reset) |
| 16 | Version compatibility | None in v1 (must match builds) |
| 17 | Chat | None in v1 |
| 18 | Player names | NPC renamed to display name from first connect |
| 19 | World state | Entirely local, no sync, no persistence |

---

## Appendix B: MoveDirection Enum

```cpp
enum MoveDirection : uint8_t {
    DirNone          = 0,   // idle (or turning — see TurnLeft/TurnRight)
    DirForward       = 1,
    DirBackward      = 2,
    DirLeft          = 3,   // pure strafe left
    DirRight         = 4,   // pure strafe right
    DirForwardLeft   = 5,   // diagonal
    DirForwardRight  = 6,   // diagonal
    DirBackwardLeft  = 7,   // diagonal
    DirBackwardRight = 8,   // diagonal
    DirTurnLeft      = 9,   // rotation changing, position not changing
    DirTurnRight     = 10,  // rotation changing, position not changing
};
```

> **Directional detection**: Computed from the angle between position delta (movement vector) and `rotZ` (facing direction). 45° sectors centered on each cardinal/diagonal direction. See §10.4 for detection logic.
>
> **Walk vs Run**: Encoded separately via `isRunning` flag (from `IsRunning` engine function). The application side selects walk vs fast animation variants — e.g. `DirForward` + `isRunning=0` → `PlayGroup Forward`, `DirForward` + `isRunning=1` → `PlayGroup FastForward`.
>
> **Sneaking**: Encoded separately via `isSneaking` flag. Sneaking does not have its own movement speed — locomotion animgroups (Forward, FastForward, etc.) work normally while in sneak posture. There is no "SneakRun" in-game; running while sneaking just uses FastForward etc. with sneak posture active.
>
> **Strafing vs diagonal**: Pure strafe (only Left/Right) and diagonal (Forward/Backward + Left/Right) use *different* animations. Pure strafe = `Left`/`Right` only. Diagonal = combination of `Forward`/`Backward` + `Left`/`Right`. See §10.1 Phase 1 table.

## Appendix C: ActionState Bitmask

Upper-body action overlaid on locomotion. Independent of `moveDirection` / `isRunning` — the receiving client combines both to determine the full animation. Upper-body animations are also independent of `SetRestrained` — no unrestrain workaround needed.

**Plugin state** (network protocol values — bitmask, combinations valid):

```cpp
enum ActionState : uint8_t {
    ActionNone       = 0,  // no special action
    ActionFiring     = 1,  // attack (animgroup depends on weapon type)
    ActionReloading  = 2,  // weapon reload (ranged only)
    ActionAimingIS   = 4,  // iron sights (ranged only)
    // Combinations:
    // ActionFiring | ActionAimingIS = 5  → aimed fire (AttackLeftIS → AimIS)
};
```

Weapon draw/holster is tracked via `weaponFormId`, not `actionState`.
Sneak enter/exit is tracked via `isSneaking` flag, not `actionState`.

**Detection** (see §10.4 for details): `actionState` is built per tick from `IsAttacking` (bit 1), `GetAnimAction == 9` (bit 2), and `IsAiming` (bit 4). Reloading has highest priority and overrides firing/aiming bits.

**Engine application** (see §10.3 for full table):

1. **Locomotion**: `PlayGroup` from `moveDirection` + `isRunning`. All locomotion anims loop. Combine animgroups for diagonals.
2. **Sneak**: Unrestrain → `SetForceSneak` → poll `IsSneaking` (returns text `"is sneaking"` / `"is not sneaking"`, not 0/1) → re-restrain.
3. **Weapon**: Temporary unrestrain + poll `IsWeaponOut` (see §10.1 weapon draw/holster).
4. **Actions**: If `actionState != None`, the animgroup depends on weapon type (via `GetEquippedWeaponType` on `weaponFormId`):
   - `Firing` (ranged, no AimingIS bit) → `AttackLeft` (0x1A) + `PlaySound3D` (weapon sound)
   - `Firing | AimingIS` (ranged) → `AttackLeftIS` (0x1D) → `AimIS` (0x14) with flag 0 + `PlaySound3D`
   - `Firing` (melee, type 0–2) → `AttackRight` (0x20) — works for both 1H and 2H
   - `Firing` (thrown, type 10–13) → `AttackThrow6` (0x96) — universal placeholder
   - `Firing` (creature/non-human) → `AttackRight` (0x20) after alert/unholster
   - `Reloading` → `ReloadA` (0xB1) — has built-in sound. Highest priority
   - `AimingIS` only → `AimIS` (0x14)
5. **Idle stance**: If `actionState == None` and `weaponFormId != 0`, `PlayGroup Aim` (0x11)
6. **Default**: If `actionState == None` and `weaponFormId == 0`, just locomotion
