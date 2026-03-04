#include "animation.h"
#include "protocol.h"

#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"

#include <cstdio>

// --------------------------------------------------
// Module state
// --------------------------------------------------

static NVSEScriptInterface*  s_script  = nullptr;
static NVSEConsoleInterface* s_console = nullptr;

// Compiled polling expressions
static Script* s_exprIsWeaponOut       = nullptr;
static Script* s_exprIsSneaking        = nullptr;
static Script* s_exprGetEquippedWeaponType = nullptr;  // ShowOff NVSE

static constexpr double UNRESTRAIN_TIMEOUT  = 2.0;  // seconds
static constexpr double MELEE_ANIM_DURATION = 0.8;  // suppress overrides during melee swing
static constexpr double THROWN_ANIM_DURATION = 0.8;  // suppress overrides during throw

// --------------------------------------------------
// Helpers
// --------------------------------------------------

static double PollNumber(Script* expr, TESObjectREFR* actor)
{
    if (!expr || !actor || !s_script) return 0.0;

    NVSEArrayVarInterface::Element result;
    result.Reset();

    bool ok = s_script->CallFunction(expr, actor, nullptr, &result, 0);
    if (!ok) return 0.0;

    return result.GetNumber();
}

// Run a console command on an NPC ref (console output must already be suppressed)
static void RunCmd(TESObjectREFR* npc, const char* cmd)
{
    s_console->RunScriptLine2(cmd, npc, true);
}

// --------------------------------------------------
// Locomotion
// --------------------------------------------------

static void ApplyLocomotion(TESObjectREFR* npc, uint8_t moveDir, uint8_t isRunning,
                            double currentTime, EntityAnimState& st)
{
    // Don't cancel full-body attack animations (melee/thrown)
    if (currentTime < st.attackAnimEndTime) return;

    if (moveDir == st.prevMoveDirection && isRunning == st.prevIsRunning) return;

    switch (moveDir) {
    case DirNone:
        RunCmd(npc, "PlayGroup Idle 1");
        break;
    case DirForward:
        RunCmd(npc, isRunning ? "PlayGroup FastForward 1" : "PlayGroup Forward 1");
        break;
    case DirBackward:
        RunCmd(npc, isRunning ? "PlayGroup FastBackward 1" : "PlayGroup Backward 1");
        break;
    case DirLeft:
        RunCmd(npc, isRunning ? "PlayGroup FastLeft 1" : "PlayGroup Left 1");
        break;
    case DirRight:
        RunCmd(npc, isRunning ? "PlayGroup FastRight 1" : "PlayGroup Right 1");
        break;
    case DirForwardLeft:
        RunCmd(npc, isRunning ? "PlayGroup FastForward 1\nPlayGroup FastLeft 1"
                              : "PlayGroup Forward 1\nPlayGroup Left 1");
        break;
    case DirForwardRight:
        RunCmd(npc, isRunning ? "PlayGroup FastForward 1\nPlayGroup FastRight 1"
                              : "PlayGroup Forward 1\nPlayGroup Right 1");
        break;
    case DirBackwardLeft:
        RunCmd(npc, isRunning ? "PlayGroup FastBackward 1\nPlayGroup FastLeft 1"
                              : "PlayGroup Backward 1\nPlayGroup Left 1");
        break;
    case DirBackwardRight:
        RunCmd(npc, isRunning ? "PlayGroup FastBackward 1\nPlayGroup FastRight 1"
                              : "PlayGroup Backward 1\nPlayGroup Right 1");
        break;
    case DirTurnLeft:
        RunCmd(npc, "PlayGroup TurnLeft 1");
        break;
    case DirTurnRight:
        RunCmd(npc, "PlayGroup TurnRight 1");
        break;
    }
}

// --------------------------------------------------
// Sneak transitions
// --------------------------------------------------

static void ApplySneak(TESObjectREFR* npc, uint8_t isSneaking, double currentTime, EntityAnimState& st)
{
    if (isSneaking == st.prevIsSneaking) return;

    if (isSneaking) {
        // Enter sneak
        if (!st.isUnrestrained) {
            RunCmd(npc, "SetRestrained 0");
            st.isUnrestrained = true;
            st.unrestrainStartTime = currentTime;
        }
        RunCmd(npc, "SetForceSneak 1");
        st.pendingOps |= PENDING_SNEAK_ENTER;
    } else {
        // Exit sneak
        if (!st.isUnrestrained) {
            RunCmd(npc, "SetRestrained 0");
            st.isUnrestrained = true;
            st.unrestrainStartTime = currentTime;
        }
        RunCmd(npc, "SetForceSneak 0");
        st.pendingOps |= PENDING_SNEAK_EXIT;
    }

    // forceReapplyActions is set in PollUnrestrain when the sneak transition
    // completes and the NPC is re-restrained — not here, because issuing
    // action commands (e.g. PlayGroup AimIS) during the unrestrain window
    // interferes with the sneak transition.
}

// --------------------------------------------------
// Weapon draw / holster
// Uses actual NPC state (IsWeaponOut poll) as source of truth,
// never relies on cached prev state. This makes it self-correcting
// even under rapid draw/holster transitions.
// --------------------------------------------------

static void ApplyWeapon(TESObjectREFR* npc, uint8_t netIsWeaponOut, double currentTime, EntityAnimState& st)
{
    // If a weapon operation is already pending, let PollUnrestrain handle it
    if (st.pendingOps & (PENDING_WEAPON_DRAW | PENDING_WEAPON_HOLSTER)) return;

    // Poll actual NPC weapon state
    bool actualWeaponOut = (PollNumber(s_exprIsWeaponOut, npc) >= 1.0);
    bool desiredWeaponOut = (netIsWeaponOut != 0);

    if (actualWeaponOut == desiredWeaponOut) return;  // already in sync

    if (desiredWeaponOut && !actualWeaponOut) {
        // Draw weapon
        if (!st.isUnrestrained) {
            RunCmd(npc, "SetRestrained 0");
            st.isUnrestrained = true;
            st.unrestrainStartTime = currentTime;
        }
        RunCmd(npc, "SetWeaponOut 1\nSetAlert 1");
        st.pendingOps |= PENDING_WEAPON_DRAW;
        st.cachedWeaponType = -1;
    } else if (!desiredWeaponOut && actualWeaponOut) {
        // Holster weapon
        if (!st.isUnrestrained) {
            RunCmd(npc, "SetRestrained 0");
            st.isUnrestrained = true;
            st.unrestrainStartTime = currentTime;
        }
        RunCmd(npc, "SetWeaponOut 0\nSetAlert 0");
        st.pendingOps |= PENDING_WEAPON_HOLSTER;
        st.cachedWeaponType = -1;
    }
}

// --------------------------------------------------
// Unrestrain polling
// --------------------------------------------------

static void PollUnrestrain(TESObjectREFR* npc, double currentTime, EntityAnimState& st)
{
    if (!st.isUnrestrained) return;

    // Safety timeout
    if (currentTime - st.unrestrainStartTime > UNRESTRAIN_TIMEOUT) {
        _MESSAGE("Animation: unrestrain timeout for NPC %08X, forcing re-restrain", npc->refID);
        RunCmd(npc, "SetRestrained 1");
        st.isUnrestrained = false;
        st.pendingOps = 0;
        return;
    }

    if (st.pendingOps & PENDING_WEAPON_DRAW) {
        double val = PollNumber(s_exprIsWeaponOut, npc);
        if (val >= 1.0) {
            st.pendingOps &= ~PENDING_WEAPON_DRAW;
            // Classify weapon type
            if (s_exprGetEquippedWeaponType) {
                st.cachedWeaponType = (int)PollNumber(s_exprGetEquippedWeaponType, npc);
            }
        }
    }

    if (st.pendingOps & PENDING_WEAPON_HOLSTER) {
        double val = PollNumber(s_exprIsWeaponOut, npc);
        if (val < 1.0) {
            st.pendingOps &= ~PENDING_WEAPON_HOLSTER;
        }
    }

    if (st.pendingOps & PENDING_SNEAK_ENTER) {
        double val = PollNumber(s_exprIsSneaking, npc);
        if (val >= 1.0) {
            st.pendingOps &= ~PENDING_SNEAK_ENTER;
        }
    }

    if (st.pendingOps & PENDING_SNEAK_EXIT) {
        double val = PollNumber(s_exprIsSneaking, npc);
        if (val < 1.0) {
            st.pendingOps &= ~PENDING_SNEAK_EXIT;
        }
    }

    // All pending operations resolved — re-restrain
    if (st.pendingOps == 0) {
        RunCmd(npc, "SetRestrained 1");
        st.isUnrestrained = false;
        // Sneak/weapon transitions may have disrupted action animations (e.g. aim).
        // Now that the NPC is re-restrained, force re-application of current action state.
        st.forceReapplyActions = true;
    }
}

// --------------------------------------------------
// Upper-body actions (firing, reloading, aiming)
// --------------------------------------------------

static void ApplyActions(TESObjectREFR* npc, uint8_t actionState, uint8_t isWeaponOut,
                         double currentTime, EntityAnimState& st)
{
    // During full-body attack animations (melee/thrown), suppress overrides.
    // Still update prev state so we don't replay stale transitions afterward.
    if (currentTime < st.attackAnimEndTime) {
        st.prevActionState = actionState;
        st.forceReapplyActions = false;
        return;
    }

    bool stateChanged = (actionState != st.prevActionState) || st.forceReapplyActions;
    st.forceReapplyActions = false;

    if (!stateChanged) return;

    bool firing   = (actionState & ActionFiring) != 0;
    bool reloading= (actionState & ActionReloading) != 0;
    bool aimingIS = (actionState & ActionAimingIS) != 0;

    if (reloading) {
        RunCmd(npc, "PlayGroup ReloadA 1");
        return;
    }

    if (firing) {
        int wtype = st.cachedWeaponType;

        if (wtype >= 3 && wtype <= 9) {
            // Ranged weapon
            if (aimingIS) {
                RunCmd(npc, "PlayGroup AttackLeftIS 1\nPlayGroup AimIS 0");
            } else {
                RunCmd(npc, "PlayGroup AttackLeft 1");
            }
            // Weapon sound
            RunCmd(npc, "PlaySound3D (GetWeaponSound (GetEquippedWeapon) 0)");
        } else if (wtype >= 0 && wtype <= 2) {
            // Melee weapon
            RunCmd(npc, "PlayGroup AttackRight 1");
            st.attackAnimEndTime = currentTime + MELEE_ANIM_DURATION;
        } else if (wtype >= 10 && wtype <= 13) {
            // Thrown weapon
            RunCmd(npc, "PlayGroup AttackThrow6 1");
            st.attackAnimEndTime = currentTime + THROWN_ANIM_DURATION;
        } else {
            // Unknown weapon type, fallback to generic attack
            RunCmd(npc, "PlayGroup AttackLeft 1");
        }
        return;
    }

    if (aimingIS) {
        RunCmd(npc, "PlayGroup AimIS 1");
        return;
    }

    // actionState == ActionNone
    if (isWeaponOut) {
        RunCmd(npc, "PlayGroup Aim 1");  // weapon lowered idle
    }
    // else: no weapon, no upper-body override — locomotion handles it
}

// --------------------------------------------------
// Public API
// --------------------------------------------------

void Animation_Init(NVSEScriptInterface* script, NVSEConsoleInterface* console)
{
    s_script  = script;
    s_console = console;

    if (!s_script) {
        _MESSAGE("Animation_Init: script interface is null");
        return;
    }

    s_exprIsWeaponOut = s_script->CompileExpression("IsWeaponOut");
    if (!s_exprIsWeaponOut) {
        _MESSAGE("Animation_Init: WARNING — failed to compile IsWeaponOut");
    }

    s_exprIsSneaking = s_script->CompileExpression("IsSneaking");
    if (!s_exprIsSneaking) {
        _MESSAGE("Animation_Init: WARNING — failed to compile IsSneaking");
    }

    // ShowOff NVSE — optional dependency
    s_exprGetEquippedWeaponType = s_script->CompileExpression("GetEquippedWeaponType");
    if (!s_exprGetEquippedWeaponType) {
        _MESSAGE("Animation_Init: GetEquippedWeaponType not available (ShowOff NVSE not installed?)");
    }

    _MESSAGE("Animation_Init: complete (IsWeaponOut=%p, IsSneaking=%p, GetEquippedWeaponType=%p)",
             s_exprIsWeaponOut, s_exprIsSneaking, s_exprGetEquippedWeaponType);
}

void Animation_ApplyState(TESObjectREFR* npc, uint32_t netEntityId,
                          uint8_t moveDirection, uint8_t isRunning,
                          uint8_t isWeaponOut, uint8_t actionState,
                          uint8_t isSneaking, double currentTime,
                          EntityAnimState& animState)
{
    if (!npc || !s_console) return;

    // 1. Poll pending unrestrain operations first
    PollUnrestrain(npc, currentTime, animState);

    // 2. Sneak transitions (may trigger unrestrain)
    ApplySneak(npc, isSneaking, currentTime, animState);

    // 3. Weapon draw/holster — compares against actual NPC state, self-correcting
    ApplyWeapon(npc, isWeaponOut, currentTime, animState);

    // 4. Locomotion (always safe while restrained, suppressed during melee/thrown anims)
    ApplyLocomotion(npc, moveDirection, isRunning, currentTime, animState);

    // 5. Upper-body actions (firing, reloading, aiming — safe while restrained)
    ApplyActions(npc, actionState, isWeaponOut, currentTime, animState);

    // Update prev-tick state
    animState.prevMoveDirection = moveDirection;
    animState.prevIsRunning     = isRunning;
    animState.prevIsSneaking    = isSneaking;
    animState.prevActionState   = actionState;
}

void Animation_Shutdown()
{
    s_exprIsWeaponOut = nullptr;
    s_exprIsSneaking  = nullptr;
    s_exprGetEquippedWeaponType = nullptr;
    _MESSAGE("Animation_Shutdown: complete");
}
