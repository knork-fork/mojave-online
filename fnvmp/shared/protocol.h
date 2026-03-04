#pragma once

#include <cstdint>

// -----------------------------------------------
// FNVMP Protocol — v0.5 (Animation Sync)
// Shared between client (fnvmp.dll) and server (server.exe)
// -----------------------------------------------

enum MessageType : uint8_t {
    MSG_CONNECT_ACK        = 1,   // server -> client (carries NetEntityId)
    MSG_HEARTBEAT          = 2,   // client -> server
    MSG_DISCONNECT         = 3,   // either direction
    MSG_PLAYER_SNAPSHOT    = 4,   // client -> server (position/rotation/state)
    MSG_WORLD_SNAPSHOT     = 5,   // server -> client (all other players' states)
    MSG_PLAYER_CONNECT     = 6,   // server -> all (new player joined, reliable)
    MSG_PLAYER_DISCONNECT  = 7,   // server -> all (player left, reliable)
};

enum MoveDirection : uint8_t {
    DirNone         = 0,
    DirForward      = 1,
    DirBackward     = 2,
    DirLeft         = 3,
    DirRight        = 4,
    DirForwardLeft  = 5,
    DirForwardRight = 6,
    DirBackwardLeft = 7,
    DirBackwardRight= 8,
    DirTurnLeft     = 9,
    DirTurnRight    = 10,
};

// Bitmask — multiple bits can be set simultaneously
enum ActionState : uint8_t {
    ActionNone      = 0,
    ActionFiring    = 1,
    ActionReloading = 2,
    ActionAimingIS  = 4,
};

// ENet channel assignments
enum Channel : uint8_t {
    CHANNEL_UNRELIABLE = 0,  // position/rotation snapshots (future)
    CHANNEL_GAME_EVENTS = 1, // game events (future)
    CHANNEL_SYSTEM = 2,      // auth, heartbeat, disconnect
};

static constexpr uint8_t NUM_CHANNELS = 3;

#pragma pack(push, 1)

struct MsgConnectAck {
    uint8_t  msgType;      // MSG_CONNECT_ACK
    uint32_t netEntityId;  // assigned by server
};

struct MsgHeartbeat {
    uint8_t msgType;       // MSG_HEARTBEAT
};

struct MsgDisconnect {
    uint8_t msgType;       // MSG_DISCONNECT
};

struct MsgPlayerSnapshot {
    uint8_t  msgType;        // MSG_PLAYER_SNAPSHOT
    uint16_t sequence;       // rolling sequence number
    uint32_t netEntityId;    // assigned by server at connect
    uint32_t cellId;         // refID of player's current cell
    float    posX, posY, posZ;
    float    rotZ;           // yaw only
    uint8_t  moveDirection;  // MoveDirection enum
    uint8_t  isRunning;      // 0 = walk, 1 = run
    uint8_t  isSneaking;     // 0 = standing, 1 = sneaking
    uint8_t  isWeaponOut;    // 0 = holstered, 1 = drawn
    uint8_t  actionState;    // ActionState bitmask
};

// Per-entity state within a WorldSnapshot
struct EntityState {
    uint32_t netEntityId;
    uint32_t cellId;
    float    posX, posY, posZ;
    float    rotZ;
    uint8_t  moveDirection;
    uint8_t  isRunning;
    uint8_t  isSneaking;
    uint8_t  isWeaponOut;
    uint8_t  actionState;
};

// Variable-length: header followed by entityCount × EntityState
struct MsgWorldSnapshotHeader {
    uint8_t  msgType;        // MSG_WORLD_SNAPSHOT
    uint16_t entityCount;
};

struct MsgPlayerConnect {
    uint8_t  msgType;        // MSG_PLAYER_CONNECT
    uint32_t netEntityId;
};

struct MsgPlayerDisconnect {
    uint8_t  msgType;        // MSG_PLAYER_DISCONNECT
    uint32_t netEntityId;
};

#pragma pack(pop)

// Timing constants
static constexpr double HEARTBEAT_INTERVAL = 2.0;  // seconds between client heartbeats
static constexpr double HEARTBEAT_TIMEOUT  = 10.0; // seconds before server considers client dead

static constexpr uint16_t DEFAULT_PORT    = 7777;
static constexpr size_t   MAX_PLAYERS     = 8;

static constexpr double SNAPSHOT_SEND_RATE     = 20.0;                   // snapshots per second
static constexpr double SNAPSHOT_SEND_INTERVAL = 1.0 / SNAPSHOT_SEND_RATE;  // ~50ms
