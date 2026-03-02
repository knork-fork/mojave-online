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
};

static uint32_t g_nextEntityId = 1;
static std::map<ENetPeer*, PlayerInfo> g_players;

// Monotonic clock in seconds
static double GetTime()
{
    return (double)clock() / CLOCKS_PER_SEC;
}

static void SendConnectAck(ENetPeer* peer, uint32_t netEntityId)
{
    MsgConnectAck ack;
    ack.msgType = MSG_CONNECT_ACK;
    ack.netEntityId = netEntityId;

    ENetPacket* packet = enet_packet_create(&ack, sizeof(ack), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, CHANNEL_SYSTEM, packet);
}

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
            g_players.erase(it);
            enet_peer_disconnect(peer, 0);
        }
        break;
    }

    default:
        printf("Unknown message type %u from peer\n", msgType);
        break;
    }
}

static void CheckTimeouts()
{
    double now = GetTime();

    for (auto it = g_players.begin(); it != g_players.end(); ) {
        double elapsed = now - it->second.lastHeartbeat;
        if (elapsed > HEARTBEAT_TIMEOUT) {
            printf("Player %u timed out (%.1fs since last heartbeat)\n",
                   it->second.netEntityId, elapsed);
            enet_peer_disconnect(it->first, 0);
            it = g_players.erase(it);
        } else {
            ++it;
        }
    }
}

int main()
{
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
                    g_players.erase(it);
                }
                break;
            }

            case ENET_EVENT_TYPE_NONE:
                break;
            }
        }

        CheckTimeouts();
    }

    // Unreachable, but for completeness
    enet_host_destroy(server);
    enet_deinitialize();
    return 0;
}
