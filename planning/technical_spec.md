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

#### PlayerSnapshot (Client → Server)
```cpp
struct PlayerSnapshot {
    uint8_t  msgType;        // MSG_PLAYER_SNAPSHOT
    uint32_t netEntityId;    // assigned by server at connect
    float    posX, posY, posZ;
    float    rotZ;           // yaw only for v1
    uint8_t  movementState;  // enum: Idle, Walk, Run, Sneak, SneakWalk, SneakRun
    uint32_t weaponFormId;   // currently drawn weapon (0 = holstered)
    uint8_t  actionState;    // enum: None, Firing, Reloading, Melee, AimingIS
};
```

#### WorldSnapshot (Server → Client)
```cpp
struct WorldSnapshot {
    uint8_t  msgType;          // MSG_WORLD_SNAPSHOT
    uint16_t entityCount;
    // followed by entityCount × EntityState:
    struct EntityState {
        uint32_t netEntityId;
        float    posX, posY, posZ;
        float    rotZ;
        uint8_t  movementState;
        uint32_t weaponFormId;
        uint8_t  actionState;
    };
};
```

#### NPCSnapshot (Zone Owner → Server)
```cpp
struct NPCSnapshot {
    uint8_t  msgType;        // MSG_NPC_SNAPSHOT
    uint16_t npcCount;
    // followed by npcCount × NPCState:
    struct NPCState {
        uint32_t netEntityId;
        float    posX, posY, posZ;
        float    rotZ;
        uint8_t  movementState;
    };
};
```

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
| `Player` | Remote player avatar (AI-disabled NPC) |
| `NPC` | Non-player NPC synced by zone owner |

### 7.3 Player Avatar Spawning

- Remote players are spawned as NPCs via `PlaceAtMe` or equivalent NVSE call
- AI is immediately disabled on the spawned NPC
- **Single generic base form** for all player avatars (e.g. NCR trooper `0x001206FD` as in prototype). Player appearance is not synced in v1 — only equipped items differentiate players visually.
- NPC is **renamed** to the player's display name (as entered on first server connect)
- Client maintains `NetEntityId → TESObjectREFR*` mapping

### 7.4 NPC Lifecycle

**When zone owner spawns an NPC (vanilla engine)**:
1. Owner detects new NPC in cell
2. Owner sends `SpawnNPC { baseFormId, position, rotation, isPersistent }`
3. Server assigns `NetEntityId`, broadcasts to all in zone
4. Other clients spawn exact same `BaseFormID` manually, map `NetEntityId → local ref`
5. Non-owners disable AI on their local copy

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
- Implementation: add all player avatar NPCs to the **engine's existing companion faction**
- This leverages the engine's existing faction hostility system — if player attacks an NPC, that NPC also becomes hostile to player's companions (which are the other player avatars)

---

## 10. Animation Sync

### 10.1 v1 Scope (Players)

Synced animations for player avatars, driven by two independent axes:

**Locomotion** (from `MovementState`):

| Animation | Trigger | AnimGroup |
|-----------|---------|-----------|
| Idle | `movementState == Idle` | `Idle` (0x00) |
| Walk forward | `movementState == Walk` | `Forward` (0x03) |
| Run forward | `movementState == Run` | `FastForward` (0x07) |
| Sneak idle | `movementState == Sneak` | `Idle` (sneak variant) |
| Sneak move | `movementState == SneakWalk` | `Forward` (sneak variant) |
| Sneak run | `movementState == SneakRun` | `FastForward` (sneak variant) |

**Weapon draw/holster** (from `weaponFormId` changes):

| Animation | Trigger | Method |
|-----------|---------|--------|
| Drawing weapon | `weaponFormId` changed from 0 to non-zero | `ref.SetWeaponOut 1` (JIP) |
| Holstering weapon | `weaponFormId` changed from non-zero to 0 | `ref.SetWeaponOut 0` (JIP) |

**Upper-body actions** (from `ActionState`, layered on top of locomotion):

| Animation | Trigger | AnimGroup |
|-----------|---------|-----------|
| Weapon drawn idle | `actionState == None` and `weaponFormId != 0` | `Aim` (0x11) |
| Firing (ranged) | `actionState == Firing` | `AttackLeft` (0x1A) |
| Melee swing | `actionState == Melee` | `AttackLeft` (melee) / `AttackPower` (0x5C) |
| Reloading | `actionState == Reloading` | `ReloadA`+ (0xB1+, weapon-dependent) |
| Iron sights (tentative) | `actionState == AimingIS` | `AimIS` (0x14) |

NOT synced in v1: jumping, fine rotation, lip sync, facial expressions, hit reactions, stagger.

### 10.2 v1 Scope (NPCs)

- Zone owner sends `(position, rotation, movementState)` per NPC
- Non-owners apply position directly and set matching animation group
- Combat targeting: zone owner sends target NetEntityId per NPC so non-owners can orient the NPC toward the correct target
- NPC weapon firing animations are NOT explicitly synced — non-owners rely on position/rotation updates; combat visuals may differ slightly between clients

### 10.3 Implementation

Two distinct layers:

**Plugin state layer** — tracks authoritative state from network snapshots. Pure data, no engine calls:
- `MovementState` — current locomotion mode
- `ActionState` — current upper-body action
- `weaponFormId` — currently drawn weapon (0 = holstered)

**Engine application layer** — translates plugin state into engine calls each tick. These calls are idempotent — no need to track previous state or detect transitions:

| Plugin state | Engine call | Notes |
|---|---|---|
| `MovementState` | `PlayGroup <AnimGroup>` | Every tick; idempotent, prevents engine reverting to idle |
| `weaponFormId` | `ref.SetWeaponOut 1/0` (JIP) | Every tick; idempotent, engine handles draw/holster anim natively |
| `ActionState` (non-None) | `PlayGroup <ActionAnimGroup>` | Every tick; idempotent |
| `ActionState` (None) | No action call | Engine stays in locomotion/aim stance |

Console output suppression via `Console::Print` patch (already implemented in prototype).

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
    ├── Send PlayerSnapshot to server
    └── If zone owner: send NPCSnapshot to server
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
- [ ] Client: send PlayerSnapshot at 20 Hz
- [ ] Server: relay snapshots to other connected clients
- [ ] Client: spawn remote player avatar NPCs (AI-disabled, named, companion faction)
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
| 8 | Faction mechanism | Engine's existing companion faction |
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

## Appendix B: MovementState Enum

```cpp
enum MovementState : uint8_t {
    Idle       = 0,
    Walk       = 1,
    Run        = 2,
    Sneak      = 3,
    SneakWalk  = 4,
    SneakRun   = 5,
};
```

## Appendix C: ActionState Enum

Upper-body action overlaid on locomotion. Independent of `MovementState` — the receiving
client combines both to determine the full animation.

**Plugin state** (network protocol values):

```cpp
enum ActionState : uint8_t {
    ActionNone       = 0,  // no special action
    ActionFiring     = 1,  // ranged attack
    ActionReloading  = 2,  // weapon reload
    ActionMelee      = 3,  // melee swing
    ActionAimingIS   = 4,  // iron sights [tentative]
};
```

Weapon draw/holster is tracked via `weaponFormId`, not `ActionState`.

**Engine application** (applied every tick, all calls idempotent):

1. **Locomotion**: `PlayGroup` from `MovementState` (`Forward`, `FastForward`, sneak variants, etc.)
2. **Weapon**: `ref.SetWeaponOut 1` if `weaponFormId != 0`, else `ref.SetWeaponOut 0` (JIP)
3. **Actions**: If `ActionState != None`, `PlayGroup` the action anim:
   - `Firing` → `AttackLeft` (0x1A)
   - `Reloading` → `ReloadA`+ (0xB1+, weapon-dependent)
   - `Melee` → `AttackLeft` (melee) / `AttackPower` (0x5C)
   - `AimingIS` → `AimIS` (0x14)
4. **Idle stance**: If `ActionState == None` and `weaponFormId != 0`, `PlayGroup Aim` (0x11)
5. **Default**: If `ActionState == None` and `weaponFormId == 0`, just locomotion
