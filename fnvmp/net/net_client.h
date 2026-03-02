#pragma once

#include <cstdint>
#include <enet/enet.h>

// Thin wrapper around ENet client operations for fnvmp.dll.
// Manages connection to server, heartbeat sending, and packet dispatch.

class NetClient {
public:
    // Connect to server. Returns true if connection attempt started.
    // Actual connection completes asynchronously via Poll().
    bool Connect(const char* host, uint16_t port);

    // Gracefully disconnect from server.
    void Disconnect();

    // Service ENet events. Call every game tick.
    // dt = time since last call in seconds (for heartbeat accumulator).
    void Poll(double dt);

    // Send raw data on a channel.
    void Send(const void* data, size_t len, uint8_t channel, bool reliable);

    // Send a player snapshot. Fills in msgType and sequence automatically.
    void SendPlayerSnapshot(struct MsgPlayerSnapshot& snapshot);

    bool IsConnected() const { return m_connected; }
    uint32_t GetNetEntityId() const { return m_netEntityId; }

private:
    void HandleReceive(ENetEvent& event);
    void SendHeartbeat();

    ENetHost* m_client = nullptr;
    ENetPeer* m_peer   = nullptr;

    bool     m_connected   = false;
    uint32_t m_netEntityId = 0;

    double   m_heartbeatAccum = 0.0;
    uint16_t m_snapshotSeq    = 0;
};
