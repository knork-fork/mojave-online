#include "entity_manager.h"
#include "interpolation.h"
#include "animation.h"
#include "protocol.h"

#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/SafeWrite.h"

#include <map>
#include <vector>
#include <cstdio>

// --------------------------------------------------
// Internal state
// --------------------------------------------------

struct RemoteEntity {
    uint32_t netEntityId = 0;
    UInt32   refID       = 0;    // TESObjectREFR refID (0 = not yet spawned)
    bool     spawned     = false;

    uint32_t cellId      = 0;
    // Position/rotation are now in the interpolation buffer, but we keep a copy
    // for initial spawn placement (before interpolation has enough data).
    float    spawnPosX = 0, spawnPosY = 0, spawnPosZ = 0;
    float    spawnRotZ = 0;
    uint8_t  moveDirection  = 0;
    uint8_t  isRunning     = 0;
    uint8_t  isSneaking    = 0;
    uint8_t  isWeaponOut   = 0;
    uint8_t  actionState   = 0;

    EntityAnimState animState;  // per-entity animation tracking
};

static std::map<uint32_t, RemoteEntity> g_entities;       // netEntityId -> entity
static std::vector<uint32_t>            g_pendingSpawns;   // netEntityIds to spawn this tick
static std::vector<uint32_t>            g_pendingDespawns; // netEntityIds to despawn this tick

static NVSEScriptInterface*  s_script  = nullptr;
static NVSEConsoleInterface* s_console = nullptr;
static Script*               s_spawnExpr = nullptr;

static constexpr float RAD_TO_DEG = 57.2957795f;

// --------------------------------------------------
// Helpers
// --------------------------------------------------

static TESObjectREFR* ResolveRefr(UInt32 refID)
{
    if (!refID) return nullptr;
    TESForm* f = LookupFormByID(refID);
    if (!f) return nullptr;
    return DYNAMIC_CAST(f, TESForm, TESObjectREFR);
}

// Suppress console output from RunScriptLine2
static void SuppressConsole()
{
    SafeWrite8(s_Console__Print + 0, 0xC2);
    SafeWrite8(s_Console__Print + 1, 0x08);
    SafeWrite8(s_Console__Print + 2, 0x00);
}

// Restore console output
static void RestoreConsole()
{
    SafeWrite8(s_Console__Print + 0, 0x55);
    SafeWrite8(s_Console__Print + 1, 0x8B);
    SafeWrite8(s_Console__Print + 2, 0xEC);
}

// Get the local player's current cell refID (0 if unavailable)
static UInt32 GetLocalPlayerCellID()
{
    if (!g_thePlayer || !*g_thePlayer) return 0;
    TESObjectREFR* player = *g_thePlayer;
    TESObjectCELL* cell = player->parentCell;
    return cell ? cell->refID : 0;
}

// --------------------------------------------------
// Public API
// --------------------------------------------------

void EntityManager_Init(NVSEScriptInterface* script, NVSEConsoleInterface* console)
{
    s_script  = script;
    s_console = console;

    if (!s_script) {
        _MESSAGE("EntityManager_Init: script interface is null");
        return;
    }

    s_spawnExpr = s_script->CompileExpression("Player.PlaceAtMe 0x001206FD 1 0 0");
    if (!s_spawnExpr) {
        _MESSAGE("EntityManager_Init: failed to compile spawn expression");
    } else {
        _MESSAGE("EntityManager_Init: spawn expression compiled OK");
    }

    // Initialize animation system
    Animation_Init(script, console);
}

void EntityManager_UpdateFromWorldSnapshot(const EntityState* states, uint16_t count, double snapshotTime)
{
    // No savegame loaded yet — ignore snapshots entirely
    UInt32 localCellId = GetLocalPlayerCellID();
    if (localCellId == 0) return;

    for (uint16_t i = 0; i < count; ++i) {
        const EntityState& es = states[i];

        auto it = g_entities.find(es.netEntityId);
        if (it == g_entities.end()) {
            // New entity — only spawn if in the same cell
            if (localCellId != 0 && es.cellId != localCellId) continue;

            RemoteEntity ent;
            ent.netEntityId   = es.netEntityId;
            ent.cellId        = es.cellId;
            ent.spawnPosX     = es.posX;
            ent.spawnPosY     = es.posY;
            ent.spawnPosZ     = es.posZ;
            ent.spawnRotZ     = es.rotZ;
            ent.moveDirection = es.moveDirection;
            ent.isRunning     = es.isRunning;
            ent.isSneaking    = es.isSneaking;
            ent.isWeaponOut   = es.isWeaponOut;
            ent.actionState   = es.actionState;

            g_entities[es.netEntityId] = ent;
            g_pendingSpawns.push_back(es.netEntityId);
            _MESSAGE("EntityManager: new remote entity %u queued for spawn", es.netEntityId);
        } else {
            // Existing entity — update non-positional state
            RemoteEntity& ent = it->second;
            ent.cellId        = es.cellId;
            ent.moveDirection = es.moveDirection;
            ent.isRunning     = es.isRunning;
            ent.isSneaking    = es.isSneaking;
            ent.isWeaponOut   = es.isWeaponOut;
            ent.actionState   = es.actionState;
        }

        // Push position into interpolation buffer (for both new and existing entities)
        Interp_PushSnapshot(es.netEntityId, es.posX, es.posY, es.posZ, es.rotZ, snapshotTime);
    }
}

void EntityManager_ApplyFireEvent(uint32_t netEntityId, double currentTime)
{
    auto it = g_entities.find(netEntityId);
    if (it == g_entities.end()) return;

    RemoteEntity& ent = it->second;
    if (!ent.spawned) return;

    TESObjectREFR* npc = ResolveRefr(ent.refID);
    if (!npc) return;

    SuppressConsole();
    Animation_ApplyFire(npc, currentTime, ent.animState);
    RestoreConsole();
}

void EntityManager_RemoveEntity(uint32_t netEntityId)
{
    auto it = g_entities.find(netEntityId);
    if (it != g_entities.end()) {
        g_pendingDespawns.push_back(netEntityId);
        Interp_RemoveEntity(netEntityId);
        _MESSAGE("EntityManager: entity %u queued for despawn", netEntityId);
    }
}

void EntityManager_Tick(double currentTime)
{
    if (!s_script || !s_console) return;
    if (!g_thePlayer || !*g_thePlayer) return;

    bool hasSpawnedEntities = false;
    for (auto& kv : g_entities) {
        if (kv.second.spawned) { hasSpawnedEntities = true; break; }
    }

    bool hasPendingWork = !g_pendingSpawns.empty() || !g_pendingDespawns.empty() || hasSpawnedEntities;
    if (!hasPendingWork) return;

    UInt32 localCellId = GetLocalPlayerCellID();
    double renderTime = currentTime - INTERP_DELAY;

    // Suppress console for all engine operations this tick
    SuppressConsole();

    // --- Process despawns first (before spawns, to handle rapid reconnect) ---
    for (uint32_t id : g_pendingDespawns) {
        auto it = g_entities.find(id);
        if (it == g_entities.end()) continue;

        if (it->second.refID) {
            TESObjectREFR* npc = ResolveRefr(it->second.refID);
            if (npc) {
                s_console->RunScriptLine2("Disable", npc, true);
                s_console->RunScriptLine2("MarkForDelete", npc, true);
                _MESSAGE("EntityManager: despawned entity %u (ref %08X)", id, it->second.refID);
            }
        }

        g_entities.erase(it);
    }
    g_pendingDespawns.clear();

    // --- Process spawns ---
    for (uint32_t id : g_pendingSpawns) {
        auto it = g_entities.find(id);
        if (it == g_entities.end()) continue;  // may have been despawned already

        RemoteEntity& ent = it->second;
        if (ent.spawned) continue;  // already spawned

        if (!s_spawnExpr) {
            _MESSAGE("EntityManager: spawn expression not compiled, cannot spawn entity %u", id);
            g_entities.erase(it);
            continue;
        }

        // Spawn NPC via PlaceAtMe
        NVSEArrayVarInterface::Element result;
        result.Reset();

        bool ok = s_script->CallFunction(s_spawnExpr, *g_thePlayer, nullptr, &result, 0);
        if (!ok) {
            _MESSAGE("EntityManager: PlaceAtMe failed for entity %u", id);
            g_entities.erase(it);
            continue;
        }

        TESForm* placedForm = result.GetTESForm();
        TESObjectREFR* placedRefr = placedForm ? DYNAMIC_CAST(placedForm, TESForm, TESObjectREFR) : nullptr;
        if (!placedRefr) {
            _MESSAGE("EntityManager: spawn did not return a ref for entity %u", id);
            g_entities.erase(it);
            continue;
        }

        ent.refID = placedRefr->refID;
        ent.spawned = true;

        // Configure as restrained teammate
        s_console->RunScriptLine2("SetRestrained 1", placedRefr, true);
        s_console->RunScriptLine2("SetCombatDisabled 1", placedRefr, true);
        s_console->RunScriptLine2("AddToFaction 0001B2A4 1", placedRefr, true);
        s_console->RunScriptLine2("SetPlayerTeammate 1", placedRefr, true);

        // Set initial position from spawn data
        char buf[256];
        sprintf_s(buf, sizeof(buf),
            "SetPos X %.2f\nSetPos Y %.2f\nSetPos Z %.2f\nSetAngle Z %.2f",
            ent.spawnPosX, ent.spawnPosY, ent.spawnPosZ, ent.spawnRotZ * RAD_TO_DEG);
        s_console->RunScriptLine2(buf, placedRefr, true);

        _MESSAGE("EntityManager: spawned entity %u as ref %08X at (%.1f, %.1f, %.1f)",
                 id, ent.refID, ent.spawnPosX, ent.spawnPosY, ent.spawnPosZ);
    }
    g_pendingSpawns.clear();

    // --- Update all spawned entities via interpolation ---
    for (auto& kv : g_entities) {
        RemoteEntity& ent = kv.second;
        if (!ent.spawned) continue;

        // Skip position updates for entities in a different cell
        if (localCellId != 0 && ent.cellId != localCellId) continue;

        float interpX, interpY, interpZ, interpRotZ;
        if (!Interp_GetState(ent.netEntityId, renderTime, currentTime,
                             interpX, interpY, interpZ, interpRotZ))
            continue;

        TESObjectREFR* npc = ResolveRefr(ent.refID);
        if (!npc) {
            _MESSAGE("EntityManager: ref %08X for entity %u no longer resolvable",
                     ent.refID, ent.netEntityId);
            continue;
        }

        char buf[256];
        sprintf_s(buf, sizeof(buf),
            "SetPos X %.2f\nSetPos Y %.2f\nSetPos Z %.2f\nSetAngle Z %.2f",
            interpX, interpY, interpZ, interpRotZ * RAD_TO_DEG);
        s_console->RunScriptLine2(buf, npc, true);
    }

    // --- Apply animation state to all spawned entities ---
    for (auto& kv : g_entities) {
        RemoteEntity& ent = kv.second;
        if (!ent.spawned) continue;

        if (localCellId != 0 && ent.cellId != localCellId) continue;

        TESObjectREFR* npc = ResolveRefr(ent.refID);
        if (!npc) continue;

        Animation_ApplyState(npc, ent.netEntityId,
                             ent.moveDirection, ent.isRunning,
                             ent.isWeaponOut, ent.actionState,
                             ent.isSneaking, currentTime,
                             ent.animState);
    }

    RestoreConsole();
}

void EntityManager_Shutdown()
{
    if (!s_console) {
        g_entities.clear();
        g_pendingSpawns.clear();
        g_pendingDespawns.clear();
        Interp_Clear();
        return;
    }

    SuppressConsole();

    for (auto& kv : g_entities) {
        if (!kv.second.refID) continue;
        TESObjectREFR* npc = ResolveRefr(kv.second.refID);
        if (npc) {
            s_console->RunScriptLine2("Disable", npc, true);
            s_console->RunScriptLine2("MarkForDelete", npc, true);
        }
    }

    RestoreConsole();

    g_entities.clear();
    g_pendingSpawns.clear();
    g_pendingDespawns.clear();
    Interp_Clear();
    Animation_Shutdown();

    _MESSAGE("EntityManager: shutdown complete");
}
