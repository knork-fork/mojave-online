#pragma once

#include <cstdint>

struct EntityState;

// Forward declarations for NVSE interfaces (match PluginAPI.h declarations)
struct NVSEScriptInterface;
struct NVSEConsoleInterface;

// Initialize entity manager. Call once at DeferredInit.
void EntityManager_Init(NVSEScriptInterface* script, NVSEConsoleInterface* console);

// Process a received WorldSnapshot. Queues spawns for new entities, marks dirty for existing ones.
void EntityManager_UpdateFromWorldSnapshot(const EntityState* states, uint16_t count);

// Queue an entity for despawn (called on PlayerDisconnect).
void EntityManager_RemoveEntity(uint32_t netEntityId);

// Execute all pending engine operations (spawn, despawn, position update).
// Must be called every game tick from MainGameLoop.
void EntityManager_Tick();

// Despawn all entities and clean up. Call on game exit.
void EntityManager_Shutdown();
