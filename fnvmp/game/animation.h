#pragma once

#include <cstdint>

// Forward declarations
struct NVSEScriptInterface;
struct NVSEConsoleInterface;
class TESObjectREFR;

// Pending unrestrain operations (bitmask)
enum PendingOp : uint8_t {
    PENDING_NONE          = 0,
    PENDING_WEAPON_DRAW   = 1,
    PENDING_WEAPON_HOLSTER= 2,
    PENDING_SNEAK_ENTER   = 4,
    PENDING_SNEAK_EXIT    = 8,
};

// Per-entity animation state (tracks previous tick values for change detection)
struct EntityAnimState {
    uint8_t  prevMoveDirection = 0;
    uint8_t  prevIsRunning     = 0;
    uint8_t  prevActionState   = 0;
    uint8_t  prevIsSneaking    = 0;

    uint8_t  pendingOps        = 0;   // PendingOp bitmask
    bool     isUnrestrained    = false;
    double   unrestrainStartTime = 0.0;

    int      cachedWeaponType  = -1;  // -1 = unknown

    // Tracked NPC weapon state (updated on draw/holster completion)
    bool     npcWeaponOut      = false;

    // True once we've seen GetAnimAction 0 or 1 for the pending weapon operation.
    // Prevents false-positive completion detection on the tick before animation starts.
    bool     weaponAnimSeen    = false;

    // Sneak transitions can disrupt action animations (e.g. aim).
    // Set true to force ApplyActions to re-issue commands next tick.
    bool     forceReapplyActions = false;

    // Melee/thrown attack animations are full-body and must not be
    // cancelled by locomotion or action-idle overrides.
    double   attackAnimEndTime = 0.0;
};

// Initialize animation system. Call once at DeferredInit.
void Animation_Init(NVSEScriptInterface* script, NVSEConsoleInterface* console);

// Apply network state as animations on a remote NPC. Call every tick per spawned entity.
void Animation_ApplyState(TESObjectREFR* npc, uint32_t netEntityId,
                          uint8_t moveDirection, uint8_t isRunning,
                          uint8_t isWeaponOut, uint8_t actionState,
                          uint8_t isSneaking, double currentTime,
                          EntityAnimState& animState);

// Apply a fire event to a remote NPC. Called on reliable fire event receipt.
void Animation_ApplyFire(TESObjectREFR* npc, double currentTime, EntityAnimState& animState);

// Clean up compiled expressions.
void Animation_Shutdown();
