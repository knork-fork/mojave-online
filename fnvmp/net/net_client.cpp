#include "net_client.h"
#include "protocol.h"
#include "nvse/PluginAPI.h"  // for _MESSAGE

#include <cstring>

bool NetClient::Connect(const char* host, uint16_t port)
{
    if (m_client) {
        _MESSAGE("NetClient::Connect — already have a host, ignoring");
        return false;
    }

    m_client = enet_host_create(
        nullptr,        // no binding (client)
        1,              // 1 outgoing connection
        NUM_CHANNELS,
        0, 0            // no bandwidth limits
    );

    if (!m_client) {
        _MESSAGE("NetClient::Connect — enet_host_create failed");
        return false;
    }

    ENetAddress address;
    enet_address_set_host(&address, host);
    address.port = port;

    m_peer = enet_host_connect(m_client, &address, NUM_CHANNELS, 0);
    if (!m_peer) {
        _MESSAGE("NetClient::Connect — enet_host_connect failed");
        enet_host_destroy(m_client);
        m_client = nullptr;
        return false;
    }

    _MESSAGE("NetClient: connecting to %s:%u...", host, port);
    return true;
}

void NetClient::Disconnect()
{
    if (!m_client) return;

    if (m_peer && m_connected) {
        // Send explicit disconnect message
        MsgDisconnect msg;
        msg.msgType = MSG_DISCONNECT;
        Send(&msg, sizeof(msg), CHANNEL_SYSTEM, true);

        enet_peer_disconnect(m_peer, 0);

        // Allow up to 1 second for graceful disconnect
        ENetEvent event;
        bool disconnected = false;
        for (int i = 0; i < 10; ++i) {
            if (enet_host_service(m_client, &event, 100) > 0) {
                if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    disconnected = true;
                    break;
                }
                if (event.type == ENET_EVENT_TYPE_RECEIVE)
                    enet_packet_destroy(event.packet);
            }
        }

        if (!disconnected)
            enet_peer_reset(m_peer);
    }
    else if (m_peer) {
        enet_peer_reset(m_peer);
    }

    enet_host_destroy(m_client);
    m_client = nullptr;
    m_peer = nullptr;
    m_connected = false;
    m_netEntityId = 0;
    m_heartbeatAccum = 0.0;

    _MESSAGE("NetClient: disconnected");
}

void NetClient::Poll(double dt)
{
    if (!m_client) return;

    ENetEvent event;
    while (enet_host_service(m_client, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            _MESSAGE("NetClient: connection established, waiting for ConnectAck...");
            break;

        case ENET_EVENT_TYPE_RECEIVE:
            HandleReceive(event);
            enet_packet_destroy(event.packet);
            break;

        case ENET_EVENT_TYPE_DISCONNECT:
            _MESSAGE("NetClient: server disconnected us");
            m_connected = false;
            m_netEntityId = 0;
            m_peer = nullptr;
            break;

        case ENET_EVENT_TYPE_NONE:
            break;
        }
    }

    // Send heartbeat on interval
    if (m_connected) {
        m_heartbeatAccum += dt;
        if (m_heartbeatAccum >= HEARTBEAT_INTERVAL) {
            m_heartbeatAccum -= HEARTBEAT_INTERVAL;
            SendHeartbeat();
        }
    }
}

void NetClient::Send(const void* data, size_t len, uint8_t channel, bool reliable)
{
    if (!m_peer) return;

    uint32_t flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* packet = enet_packet_create(data, len, flags);
    enet_peer_send(m_peer, channel, packet);
}

void NetClient::HandleReceive(ENetEvent& event)
{
    if (event.packet->dataLength < 1) return;

    uint8_t msgType = event.packet->data[0];

    switch (msgType) {
    case MSG_CONNECT_ACK: {
        if (event.packet->dataLength < sizeof(MsgConnectAck)) {
            _MESSAGE("NetClient: ConnectAck too short");
            break;
        }
        MsgConnectAck ack;
        std::memcpy(&ack, event.packet->data, sizeof(ack));
        m_netEntityId = ack.netEntityId;
        m_connected = true;
        m_heartbeatAccum = 0.0;
        _MESSAGE("Connected, assigned NetEntityId %u", m_netEntityId);
        break;
    }

    case MSG_DISCONNECT:
        _MESSAGE("NetClient: server sent disconnect");
        m_connected = false;
        m_netEntityId = 0;
        break;

    default:
        _MESSAGE("NetClient: unknown message type %u", msgType);
        break;
    }
}

void NetClient::SendHeartbeat()
{
    MsgHeartbeat msg;
    msg.msgType = MSG_HEARTBEAT;
    Send(&msg, sizeof(msg), CHANNEL_SYSTEM, true);
}
