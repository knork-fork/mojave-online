#include <windows.h>

#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"

#include <enet/enet.h>
#include "protocol.h"
#include "net_client.h"
#include "entity_manager.h"

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

// Timing
static LARGE_INTEGER g_freq{};
static LARGE_INTEGER g_last{};
static double g_gameTime = 0.0;  // monotonic cumulative clock (seconds)

// Networking
static NetClient g_netClient;
static bool g_enetInitialized = false;
static double g_snapshotAccum = 0.0;

// --------------------------------------------------
// Helpers
// --------------------------------------------------

static UInt32 GetPlayerCellID()
{
	if (!g_thePlayer || !*g_thePlayer) return 0;
	TESObjectREFR* player = *g_thePlayer;
	TESObjectCELL* cell = player->parentCell;
	return cell ? cell->refID : 0;
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
		_MESSAGE("Game exiting — cleaning up");
		EntityManager_Shutdown();
		g_netClient.Disconnect();
		if (g_enetInitialized)
		{
			enet_deinitialize();
			g_enetInitialized = false;
		}
		return;
	}

	// Initialize at DeferredInit (safe point for expression system)
	if (msg->type == NVSEMessagingInterface::kMessage_DeferredInit)
	{
		_MESSAGE("DeferredInit received");

		// Initialize entity manager (compiles spawn expression)
		EntityManager_Init(g_script, g_console);

		// Initialize ENet and connect to server
		if (enet_initialize() != 0)
		{
			_MESSAGE("ERROR: Failed to initialize ENet");
			return;
		}
		g_enetInitialized = true;
		_MESSAGE("ENet initialized");

		g_netClient.Connect("192.168.1.149", DEFAULT_PORT);
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
	g_gameTime += dt;

	// Poll network every tick
	g_netClient.Poll(dt);

	// Process disconnect events from server
	if (g_netClient.HasDisconnectEvents())
	{
		auto events = g_netClient.TakeDisconnectEvents();
		for (uint32_t id : events)
		{
			EntityManager_RemoveEntity(id);
		}
	}

	// Process world snapshot from server
	if (g_netClient.HasNewWorldSnapshot())
	{
		const auto& entities = g_netClient.GetWorldEntities();
		if (!entities.empty())
		{
			EntityManager_UpdateFromWorldSnapshot(entities.data(), (uint16_t)entities.size(), g_gameTime);
		}
		g_netClient.ClearWorldSnapshot();
	}

	// Execute pending entity operations (spawn, despawn, interpolated position update)
	EntityManager_Tick(g_gameTime);

	// Send player snapshot at 20 Hz (only when a savegame is loaded)
	if (g_netClient.IsConnected() && g_thePlayer && *g_thePlayer
		&& (*g_thePlayer)->parentCell)
	{
		g_snapshotAccum += dt;
		if (g_snapshotAccum >= SNAPSHOT_SEND_INTERVAL)
		{
			g_snapshotAccum -= SNAPSHOT_SEND_INTERVAL;

			TESObjectREFR* player = *g_thePlayer;

			MsgPlayerSnapshot snap;
			snap.netEntityId   = g_netClient.GetNetEntityId();
			snap.cellId        = player->parentCell->refID;
			snap.posX          = player->posX;
			snap.posY          = player->posY;
			snap.posZ          = player->posZ;
			snap.rotZ          = player->rotZ;
			snap.movementState = MS_Idle;   // hardcoded for v0.3
			snap.weaponFormId  = 0;         // hardcoded for v0.3
			snap.actionState   = AS_None;   // hardcoded for v0.3

			g_netClient.SendPlayerSnapshot(snap);
		}
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
