#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <map>

#include <enet/enet.h>
#include "protocol.h"

// Per-connected-player state
struct PlayerInfo {
    uint32_t netEntityId;
    double   lastHeartbeat;  // seconds since server start

    // Latest received snapshot
    bool     hasSnapshot    = false;
    uint16_t lastSequence   = 0;
    uint32_t cellId         = 0;
    float    posX = 0, posY = 0, posZ = 0;
    float    rotZ = 0;
    uint8_t  moveDirection  = 0;
    uint8_t  isRunning      = 0;
    uint8_t  isSneaking     = 0;
    uint8_t  isWeaponOut    = 0;
    uint8_t  actionState    = 0;
};

static uint32_t g_nextEntityId = 1;
static std::map<ENetPeer*, PlayerInfo> g_players;
static bool g_verbose = false;

// Broadcast timer
static double g_lastBroadcastTime = 0.0;
static double g_broadcastAccum    = 0.0;

// Monotonic clock in seconds
static double GetTime()
{
    return (double)clock() / CLOCKS_PER_SEC;
}

// --------------------------------------------------
// Send helpers
// --------------------------------------------------

static void SendConnectAck(ENetPeer* peer, uint32_t netEntityId)
{
    MsgConnectAck ack;
    ack.msgType = MSG_CONNECT_ACK;
    ack.netEntityId = netEntityId;

    ENetPacket* packet = enet_packet_create(&ack, sizeof(ack), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, CHANNEL_SYSTEM, packet);
}

static void BroadcastPlayerConnect(uint32_t netEntityId, ENetPeer* exclude)
{
    MsgPlayerConnect msg;
    msg.msgType = MSG_PLAYER_CONNECT;
    msg.netEntityId = netEntityId;

    for (auto& kv : g_players) {
        if (kv.first == exclude) continue;
        ENetPacket* packet = enet_packet_create(&msg, sizeof(msg), ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(kv.first, CHANNEL_GAME_EVENTS, packet);
    }
}

static void BroadcastPlayerDisconnect(uint32_t netEntityId, ENetPeer* exclude)
{
    MsgPlayerDisconnect msg;
    msg.msgType = MSG_PLAYER_DISCONNECT;
    msg.netEntityId = netEntityId;

    for (auto& kv : g_players) {
        if (kv.first == exclude) continue;
        ENetPacket* packet = enet_packet_create(&msg, sizeof(msg), ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(kv.first, CHANNEL_GAME_EVENTS, packet);
    }
}

// Build and send a WorldSnapshot to each peer containing all OTHER peers' states
static void BroadcastWorldSnapshots()
{
    if (g_players.size() < 2) return;

    // Stack buffer: header + up to MAX_PLAYERS EntityStates
    uint8_t buf[sizeof(MsgWorldSnapshotHeader) + MAX_PLAYERS * sizeof(EntityState)];

    for (auto& recipient : g_players) {
        MsgWorldSnapshotHeader* hdr = reinterpret_cast<MsgWorldSnapshotHeader*>(buf);
        hdr->msgType = MSG_WORLD_SNAPSHOT;
        hdr->entityCount = 0;

        EntityState* states = reinterpret_cast<EntityState*>(buf + sizeof(MsgWorldSnapshotHeader));

        for (auto& other : g_players) {
            if (other.first == recipient.first) continue;
            if (!other.second.hasSnapshot) continue;

            const PlayerInfo& pi = other.second;
            EntityState& es = states[hdr->entityCount];
            es.netEntityId  = pi.netEntityId;
            es.cellId       = pi.cellId;
            es.posX         = pi.posX;
            es.posY         = pi.posY;
            es.posZ         = pi.posZ;
            es.rotZ         = pi.rotZ;
            es.moveDirection = pi.moveDirection;
            es.isRunning     = pi.isRunning;
            es.isSneaking    = pi.isSneaking;
            es.isWeaponOut   = pi.isWeaponOut;
            es.actionState   = pi.actionState;

            hdr->entityCount++;
        }

        if (hdr->entityCount == 0) continue;

        size_t totalLen = sizeof(MsgWorldSnapshotHeader) + hdr->entityCount * sizeof(EntityState);
        ENetPacket* packet = enet_packet_create(buf, totalLen, ENET_PACKET_FLAG_UNSEQUENCED);
        enet_peer_send(recipient.first, CHANNEL_UNRELIABLE, packet);
    }
}

// --------------------------------------------------
// Receive handler
// --------------------------------------------------

static void HandleReceive(ENetPeer* peer, ENetPacket* packet)
{
    if (packet->dataLength < 1) return;

    uint8_t msgType = packet->data[0];
    auto it = g_players.find(peer);

    switch (msgType) {
    case MSG_HEARTBEAT: {
        if (it != g_players.end()) {
            it->second.lastHeartbeat = GetTime();
        }
        break;
    }

    case MSG_DISCONNECT: {
        if (it != g_players.end()) {
            printf("Player %u sent disconnect\n", it->second.netEntityId);
            BroadcastPlayerDisconnect(it->second.netEntityId, peer);
            g_players.erase(it);
            enet_peer_disconnect(peer, 0);
        }
        break;
    }

    case MSG_PLAYER_SNAPSHOT: {
        if (packet->dataLength < sizeof(MsgPlayerSnapshot)) {
            printf("PlayerSnapshot too short from peer\n");
            break;
        }
        if (it == g_players.end()) break;

        MsgPlayerSnapshot snap;
        std::memcpy(&snap, packet->data, sizeof(snap));

        if (snap.netEntityId != it->second.netEntityId) {
            printf("PlayerSnapshot netEntityId mismatch: got %u, expected %u\n",
                   snap.netEntityId, it->second.netEntityId);
            break;
        }

        PlayerInfo& pi = it->second;
        pi.hasSnapshot   = true;
        pi.lastSequence  = snap.sequence;
        pi.cellId        = snap.cellId;
        pi.posX          = snap.posX;
        pi.posY          = snap.posY;
        pi.posZ          = snap.posZ;
        pi.rotZ          = snap.rotZ;
        pi.moveDirection = snap.moveDirection;
        pi.isRunning     = snap.isRunning;
        pi.isSneaking    = snap.isSneaking;
        pi.isWeaponOut   = snap.isWeaponOut;
        pi.actionState   = snap.actionState;

        if (g_verbose) {
            printf("Player %u snapshot seq=%u cell=%08X pos=(%.1f, %.1f, %.1f) rotZ=%.2f\n",
                   pi.netEntityId, snap.sequence, snap.cellId,
                   snap.posX, snap.posY, snap.posZ, snap.rotZ);
        }
        break;
    }

    default:
        printf("Unknown message type %u from peer\n", msgType);
        break;
    }
}

// --------------------------------------------------
// Timeout checker
// --------------------------------------------------

static void CheckTimeouts()
{
    double now = GetTime();

    for (auto it = g_players.begin(); it != g_players.end(); ) {
        double elapsed = now - it->second.lastHeartbeat;
        if (elapsed > HEARTBEAT_TIMEOUT) {
            printf("Player %u timed out (%.1fs since last heartbeat)\n",
                   it->second.netEntityId, elapsed);
            BroadcastPlayerDisconnect(it->second.netEntityId, it->first);
            enet_peer_disconnect(it->first, 0);
            it = g_players.erase(it);
        } else {
            ++it;
        }
    }
}

// --------------------------------------------------
// Main
// --------------------------------------------------

int main(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--verbose") == 0) {
            g_verbose = true;
            printf("Verbose logging enabled\n");
        }
    }

    if (enet_initialize() != 0) {
        fprintf(stderr, "Failed to initialize ENet\n");
        return 1;
    }

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = DEFAULT_PORT;

    ENetHost* server = enet_host_create(
        &address,
        MAX_PLAYERS,
        NUM_CHANNELS,
        0, 0  // no bandwidth limits
    );

    if (!server) {
        fprintf(stderr, "Failed to create ENet server on port %u\n", DEFAULT_PORT);
        enet_deinitialize();
        return 1;
    }

    printf("Listening on port %u\n", DEFAULT_PORT);

    g_lastBroadcastTime = GetTime();

    ENetEvent event;
    while (true) {
        while (enet_host_service(server, &event, 10) > 0) {
            switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                uint32_t id = g_nextEntityId++;
                PlayerInfo info;
                info.netEntityId = id;
                info.lastHeartbeat = GetTime();
                g_players[event.peer] = info;

                SendConnectAck(event.peer, id);
                BroadcastPlayerConnect(id, event.peer);
                printf("Player %u connected (%u/%zu players)\n",
                       id, (unsigned)g_players.size(), MAX_PLAYERS);
                break;
            }

            case ENET_EVENT_TYPE_RECEIVE:
                HandleReceive(event.peer, event.packet);
                enet_packet_destroy(event.packet);
                break;

            case ENET_EVENT_TYPE_DISCONNECT: {
                auto it = g_players.find(event.peer);
                if (it != g_players.end()) {
                    printf("Player %u disconnected (%u/%zu players)\n",
                           it->second.netEntityId,
                           (unsigned)(g_players.size() - 1), MAX_PLAYERS);
                    BroadcastPlayerDisconnect(it->second.netEntityId, event.peer);
                    g_players.erase(it);
                }
                break;
            }

            case ENET_EVENT_TYPE_NONE:
                break;
            }
        }

        CheckTimeouts();

        // Broadcast world snapshots at 20 Hz
        double now = GetTime();
        g_broadcastAccum += (now - g_lastBroadcastTime);
        g_lastBroadcastTime = now;
        while (g_broadcastAccum >= SNAPSHOT_SEND_INTERVAL) {
            g_broadcastAccum -= SNAPSHOT_SEND_INTERVAL;
            BroadcastWorldSnapshots();
        }
    }

    // Unreachable, but for completeness
    enet_host_destroy(server);
    enet_deinitialize();
    return 0;
}
