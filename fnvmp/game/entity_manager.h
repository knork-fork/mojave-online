#pragma once

#include <cstdint>

struct EntityState;

// Forward declarations for NVSE interfaces (match PluginAPI.h declarations)
struct NVSEScriptInterface;
struct NVSEConsoleInterface;

// Initialize entity manager. Call once at DeferredInit.
void EntityManager_Init(NVSEScriptInterface* script, NVSEConsoleInterface* console);

// Process a received WorldSnapshot. Pushes snapshots into interpolation buffer and queues spawns for new entities.
// snapshotTime = client-local monotonic clock at time of receipt.
void EntityManager_UpdateFromWorldSnapshot(const EntityState* states, uint16_t count, double snapshotTime);

// Apply a fire event to a remote entity. Called when MSG_REMOTE_FIRE is received.
void EntityManager_ApplyFireEvent(uint32_t netEntityId, double currentTime);

// Queue an entity for despawn (called on PlayerDisconnect).
void EntityManager_RemoveEntity(uint32_t netEntityId);

// Execute all pending engine operations (spawn, despawn, interpolated position update).
// Must be called every game tick from MainGameLoop.
// currentTime = client-local monotonic clock.
void EntityManager_Tick(double currentTime);

// Despawn all entities and clean up. Call on game exit.
void EntityManager_Shutdown();
