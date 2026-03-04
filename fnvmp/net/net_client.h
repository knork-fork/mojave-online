#pragma once

#include <cstdint>
#include <vector>
#include <enet/enet.h>

struct EntityState;

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

    // Send a fire event to server (reliable, CHANNEL_GAME_EVENTS).
    void SendFireEvent(uint32_t netEntityId);

    bool IsConnected() const { return m_connected; }
    uint32_t GetNetEntityId() const { return m_netEntityId; }

    // World snapshot access (written by Poll, read by main loop)
    bool HasNewWorldSnapshot() const { return m_hasNewWorldSnapshot; }
    const std::vector<EntityState>& GetWorldEntities() const { return m_worldEntities; }
    void ClearWorldSnapshot() { m_hasNewWorldSnapshot = false; }

    // Disconnect events (queued by Poll, consumed by main loop)
    bool HasDisconnectEvents() const { return !m_disconnectedEntities.empty(); }
    std::vector<uint32_t> TakeDisconnectEvents();

    // Remote fire events (queued by Poll, consumed by main loop)
    bool HasFireEvents() const { return !m_fireEvents.empty(); }
    std::vector<uint32_t> TakeFireEvents();

private:
    void HandleReceive(ENetEvent& event);
    void SendHeartbeat();

    ENetHost* m_client = nullptr;
    ENetPeer* m_peer   = nullptr;

    bool     m_connected   = false;
    uint32_t m_netEntityId = 0;

    double   m_heartbeatAccum = 0.0;
    uint16_t m_snapshotSeq    = 0;

    // Latest world snapshot from server
    bool m_hasNewWorldSnapshot = false;
    std::vector<EntityState> m_worldEntities;

    // Queued disconnect events
    std::vector<uint32_t> m_disconnectedEntities;

    // Queued remote fire events (netEntityIds of players who fired)
    std::vector<uint32_t> m_fireEvents;
};
