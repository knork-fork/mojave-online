#pragma once

#include <cstdint>

// -----------------------------------------------
// FNVMP Protocol — v0.2 (Player Snapshot Pipeline)
// Shared between client (fnvmp.dll) and server (server.exe)
// -----------------------------------------------

enum MessageType : uint8_t {
    MSG_CONNECT_ACK      = 1,   // server -> client (carries NetEntityId)
    MSG_HEARTBEAT        = 2,   // client -> server
    MSG_DISCONNECT       = 3,   // either direction
    MSG_PLAYER_SNAPSHOT  = 4,   // client -> server (position/rotation/state)
};

enum MovementState : uint8_t {
    MS_Idle      = 0,
    MS_Walk      = 1,
    MS_Run       = 2,
    MS_Sneak     = 3,
    MS_SneakWalk = 4,
    MS_SneakRun  = 5,
};

enum ActionState : uint8_t {
    AS_None      = 0,
    AS_Firing    = 1,
    AS_Reloading = 2,
    AS_Melee     = 3,
    AS_AimingIS  = 4,
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
    uint8_t  movementState;  // MovementState enum
    uint32_t weaponFormId;   // 0 = holstered
    uint8_t  actionState;    // ActionState enum
};

#pragma pack(pop)

// Timing constants
static constexpr double HEARTBEAT_INTERVAL = 2.0;  // seconds between client heartbeats
static constexpr double HEARTBEAT_TIMEOUT  = 5.0;  // seconds before server considers client dead

static constexpr uint16_t DEFAULT_PORT    = 7777;
static constexpr size_t   MAX_PLAYERS     = 8;

static constexpr double SNAPSHOT_SEND_RATE     = 20.0;                   // snapshots per second
static constexpr double SNAPSHOT_SEND_INTERVAL = 1.0 / SNAPSHOT_SEND_RATE;  // ~50ms
