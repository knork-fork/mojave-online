#include <windows.h>

#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/SafeWrite.h"

#include <enet/enet.h>
#include "protocol.h"
#include "net_client.h"

// --------------------------------------------------
// Logging / globals
// --------------------------------------------------

IDebugLog gLog("fnvmp.log");

static PluginHandle g_pluginHandle = kPluginHandle_Invalid;

static NVSEMessagingInterface* g_msg = nullptr;
static NVSEScriptInterface* g_script = nullptr;
static NVSEConsoleInterface* g_console = nullptr;


// Required for CompileExpression / NVSE expressions
static ExpressionEvaluatorUtils g_expEvalUtils{};

// Our persistent spawned NPC reference (RefID, not pointer)
static UInt32 g_npcRefID = 0;

// Scripts compiled once
static Script* g_spawnExpr = nullptr;

// Target gating + transform
static const UInt32 kVictorsShackCellId = 0x0010AAFC;

// NCR trooper base form (you gave this)
static const UInt32 kNpcBaseFormId = 0x001206FD;

// Position/rotation, hardcoded for testing
static const float kPX = -282.30f;
static const float kPY = 470.03f;
static const float kPZ = 545.05f;

static const float kRX = 0.55f;
static const float kRY = 0.00f;
static const float kRZ = 5.63f;

// Timing
static double        g_accum = 0.0;
static LARGE_INTEGER g_freq{};
static LARGE_INTEGER g_last{};

// Networking
static NetClient g_netClient;
static bool g_enetInitialized = false;

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

static UInt32 GetPlayerCellID()
{
	if (!g_thePlayer || !*g_thePlayer) return 0;
	TESObjectREFR* player = *g_thePlayer;
	TESObjectCELL* cell = player->parentCell;
	return cell ? cell->refID : 0;
}

static void CompileExpressionsOnce()
{
	if (!g_script)
	{
		_MESSAGE("CompileExpressionsOnce: g_script is null");
		return;
	}

	// 1) Spawn expression: returns the placed reference (should be a form result)
	// NOTE: This is an expression, NOT "Begin Function", so it won't hit your function parser issue.
	_MESSAGE("Compiling spawn expression...");
	g_spawnExpr = g_script->CompileExpression("Player.PlaceAtMe 0x001206FD 1 0 0");
	_MESSAGE("Spawn expr ptr: %p", g_spawnExpr);

	if (!g_spawnExpr) _MESSAGE("ERROR: g_spawnExpr is NULL (CompileExpression failed)");
}

static bool EnsureNpcExistsInMemory()
{
	// If we already have a refID, try to resolve it.
	if (g_npcRefID)
	{
		if (ResolveRefr(g_npcRefID))
			return true;

		// If the ref can't be resolved (unloaded, destroyed, etc.), drop it.
		_MESSAGE("NPC refID %08X not resolvable; will respawn", g_npcRefID);
		g_npcRefID = 0;
	}

	// Need to spawn.
	if (!g_spawnExpr || !g_thePlayer || !*g_thePlayer)
		return false;

	NVSEArrayVarInterface::Element result;
	result.Reset();

	// Run the spawn expression with Player as calling object.
	// Expected result type: Form (the placed reference).
	const bool ok = g_script->CallFunction(g_spawnExpr, *g_thePlayer, nullptr, &result, 0);
	if (!ok)
	{
		_MESSAGE("Spawn CallFunction failed");
		return false;
	}

	TESForm* placedForm = result.GetTESForm();
	TESObjectREFR* placedRefr = placedForm ? DYNAMIC_CAST(placedForm, TESForm, TESObjectREFR) : nullptr;
	if (!placedRefr)
	{
		_MESSAGE("Spawn expression did not return a TESObjectREFR (type=%u, form=%p, formID=%08X)",
			result.GetType(),
			placedForm,
			placedForm ? placedForm->refID : 0);
		return false;
	}

	g_npcRefID = placedRefr->refID;
	_MESSAGE("Spawned NPC refID=%08X", g_npcRefID);
	return true;
}

static bool isWalking = false;
static void MoveNpcToTarget()
{
	if (!g_console) {
		_MESSAGE("MoveNpcToTarget: g_console is null");
		return;
	}

    TESObjectREFR* npc = ResolveRefr(g_npcRefID);
    if (!npc) return;

    char buf[128];

	sprintf_s(buf,
		"SetPos X %f\n"
		"SetPos Y %f\n"
		"SetPos Z %f\n"
		"SetAngle X %f\n"
		"SetAngle Y %f\n"
		"SetAngle Z %f",
		kPX, kPY, kPZ, kRX, kRY, kRZ
	);

	// Patch Console::Print to "retn 8" so RunScriptLine2 produces no output
	SafeWrite8(s_Console__Print + 0, 0xC2);
	SafeWrite8(s_Console__Print + 1, 0x08);
	SafeWrite8(s_Console__Print + 2, 0x00);

	g_console->RunScriptLine2(buf, npc, true);

	if (isWalking) {
		g_console->RunScriptLine2("PlayGroup Idle 1", npc, true);
		isWalking = false;
	} else {
		g_console->RunScriptLine2("PlayGroup Forward 1", npc, true);
		isWalking = true;
	}

	// Restore Console::Print: "push ebp ; mov ebp, esp"
	SafeWrite8(s_Console__Print + 0, 0x55);
	SafeWrite8(s_Console__Print + 1, 0x8B);
	SafeWrite8(s_Console__Print + 2, 0xEC);
}



// --------------------------------------------------
// Tick (once per second)
// --------------------------------------------------

static void Tick_OncePerSecond()
{
	// Gate by cell
	const UInt32 cellId = GetPlayerCellID();
	if (cellId != kVictorsShackCellId)
		return;

	// Spawn if needed
	if (!EnsureNpcExistsInMemory())
		return;

	// Move/rotate
	MoveNpcToTarget();
}

// --------------------------------------------------
// Messaging
// --------------------------------------------------

static void MessageHandler(NVSEMessagingInterface::Message* msg)
{
	if (!msg) return;

	// Clean up networking on game exit
	if (msg->type == NVSEMessagingInterface::kMessage_ExitGame ||
		msg->type == NVSEMessagingInterface::kMessage_ExitGame_Console)
	{
		_MESSAGE("Game exiting — disconnecting network");
		g_netClient.Disconnect();
		if (g_enetInitialized)
		{
			enet_deinitialize();
			g_enetInitialized = false;
		}
		return;
	}

	// Compile expressions at DeferredInit (safe point for expression system)
	if (msg->type == NVSEMessagingInterface::kMessage_DeferredInit)
	{
		_MESSAGE("DeferredInit received: compiling expressions");
		CompileExpressionsOnce();

		// Initialize ENet and connect to server
		if (enet_initialize() != 0)
		{
			_MESSAGE("ERROR: Failed to initialize ENet");
			return;
		}
		g_enetInitialized = true;
		_MESSAGE("ENet initialized");

		g_netClient.Connect("127.0.0.1", DEFAULT_PORT);
		return;
	}

	if (msg->type != NVSEMessagingInterface::kMessage_MainGameLoop)
		return;

	if (g_freq.QuadPart == 0)
	{
		QueryPerformanceFrequency(&g_freq);
		QueryPerformanceCounter(&g_last);
		return;
	}

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);

	const double dt = double(now.QuadPart - g_last.QuadPart) / double(g_freq.QuadPart);
	g_last = now;

	// Poll network every tick
	g_netClient.Poll(dt);

	g_accum += dt;
	if (g_accum >= 1.0)
	{
		g_accum -= 1.0;
		Tick_OncePerSecond();
	}
}

// --------------------------------------------------
// NVSE entry points
// --------------------------------------------------

extern "C"
{
	bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
	{
		info->infoVersion = PluginInfo::kInfoVersion;
		info->name = "FNVMP";
		info->version = 1;

		if (nvse->nvseVersion < PACKED_NVSE_VERSION)
			return false;

		if (!nvse->isEditor && nvse->runtimeVersion < RUNTIME_VERSION_1_4_0_525)
			return false;

		return true;
	}

	bool NVSEPlugin_Load(NVSEInterface* nvse)
	{
		g_pluginHandle = nvse->GetPluginHandle();

		// Required for CompileExpression / NVSE expression evaluator utils.
		nvse->InitExpressionEvaluatorUtils(&g_expEvalUtils);

		g_msg = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
		g_script = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
		if (!g_msg || !g_script)
			return false;
		
		g_console = (NVSEConsoleInterface*)nvse->QueryInterface(kInterface_Console);
		if (!g_console) return false;

		// Register for NVSE messages (DeferredInit + MainGameLoop)
		g_msg->RegisterListener(g_pluginHandle, "NVSE", MessageHandler);

		_MESSAGE("FNVMP plugin loaded");
		return true;
	}
}
