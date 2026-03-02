#pragma once

#include <cstdint>

// -----------------------------------------------
// FNVMP Protocol — v0.1 (Network Foundation)
// Shared between client (fnvmp.dll) and server (server.exe)
// -----------------------------------------------

enum MessageType : uint8_t {
    MSG_CONNECT_ACK = 1,   // server -> client (carries NetEntityId)
    MSG_HEARTBEAT   = 2,   // client -> server
    MSG_DISCONNECT  = 3,   // either direction
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

#pragma pack(pop)

// Timing constants
static constexpr double HEARTBEAT_INTERVAL = 2.0;  // seconds between client heartbeats
static constexpr double HEARTBEAT_TIMEOUT  = 5.0;  // seconds before server considers client dead

static constexpr uint16_t DEFAULT_PORT    = 7777;
static constexpr size_t   MAX_PLAYERS     = 8;
