#include <windows.h>

#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"

#include <cmath>

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

// Detection — compiled expressions
static Script* s_exprIsRunning       = nullptr;
static Script* s_exprIsSneaking      = nullptr;
static Script* s_exprIsWeaponOut     = nullptr;
static Script* s_exprGetAnimAction   = nullptr;
static Script* s_exprIsAiming        = nullptr;  // ShowOff NVSE (optional)

// Detection — previous-tick state for direction computation
static float  g_prevPosX = 0, g_prevPosY = 0, g_prevPosZ = 0;
static float  g_prevRotZ = 0;
static bool   g_prevPosValid = false;

// Attack edge detection — per-tick polling, latched until consumed by 20Hz snapshot
static int    g_prevAnimAction = 0;   // previous tick's GetAnimAction value
static bool   g_attackDetected = false; // latched: true when a new attack starts

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
// Detection helpers
// --------------------------------------------------

static double PollNumber(Script* expr, TESObjectREFR* actor)
{
	if (!expr || !actor || !g_script) return 0.0;
	NVSEArrayVarInterface::Element result;
	result.Reset();
	bool ok = g_script->CallFunction(expr, actor, nullptr, &result, 0);
	if (!ok) return 0.0;
	return result.GetNumber();
}

static uint8_t ComputeMoveDirection(float curX, float curY, float curZ, float curRotZ)
{
	if (!g_prevPosValid) {
		g_prevPosX = curX;
		g_prevPosY = curY;
		g_prevPosZ = curZ;
		g_prevRotZ = curRotZ;
		g_prevPosValid = true;
		return DirNone;
	}

	float dx = curX - g_prevPosX;
	float dy = curY - g_prevPosY;
	float magnitude = sqrtf(dx * dx + dy * dy);

	g_prevPosX = curX;
	g_prevPosY = curY;
	g_prevPosZ = curZ;

	static constexpr float IDLE_THRESHOLD = 1.0f;    // units per snapshot interval
	static constexpr float TURN_THRESHOLD = 0.01f;    // radians per snapshot interval

	if (magnitude < IDLE_THRESHOLD) {
		// Check for turning in place
		float rotDelta = curRotZ - g_prevRotZ;
		g_prevRotZ = curRotZ;

		if (rotDelta > 3.14159f) rotDelta -= 6.28318f;
		if (rotDelta < -3.14159f) rotDelta += 6.28318f;

		if (rotDelta > TURN_THRESHOLD) return DirTurnLeft;
		if (rotDelta < -TURN_THRESHOLD) return DirTurnRight;
		return DirNone;
	}

	g_prevRotZ = curRotZ;

	// Compute angle between movement vector and facing direction
	// FNV: +X = east, +Y = north, rotZ is radians (0 = north, increasing = CCW)
	float moveAngle = atan2f(dx, dy);  // angle of movement vector (0 = north)
	float relAngle = moveAngle - curRotZ;

	// Normalize to [-pi, pi]
	if (relAngle > 3.14159f) relAngle -= 6.28318f;
	if (relAngle < -3.14159f) relAngle += 6.28318f;

	// Map to 8-direction bins (45-degree sectors)
	// relAngle ~0 = forward, ~pi = backward, ~pi/2 = left, ~-pi/2 = right
	static constexpr float PI_8 = 0.392699f;  // pi/8

	if (relAngle >= -PI_8 && relAngle < PI_8)
		return DirForward;
	if (relAngle >= PI_8 && relAngle < 3 * PI_8)
		return DirForwardLeft;
	if (relAngle >= 3 * PI_8 && relAngle < 5 * PI_8)
		return DirLeft;
	if (relAngle >= 5 * PI_8 && relAngle < 7 * PI_8)
		return DirBackwardLeft;
	if (relAngle >= -3 * PI_8 && relAngle < -PI_8)
		return DirForwardRight;
	if (relAngle >= -5 * PI_8 && relAngle < -3 * PI_8)
		return DirRight;
	if (relAngle >= -7 * PI_8 && relAngle < -5 * PI_8)
		return DirBackwardRight;

	return DirBackward;  // remaining sector: backward
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

		// Initialize entity manager (compiles spawn expression + animation expressions)
		EntityManager_Init(g_script, g_console);

		// Compile detection expressions for local player state
		s_exprIsRunning        = g_script->CompileExpression("IsRunning");
		s_exprIsSneaking       = g_script->CompileExpression("IsSneaking");
		s_exprIsWeaponOut      = g_script->CompileExpression("IsWeaponOut");
		s_exprGetAnimAction    = g_script->CompileExpression("GetAnimAction");

		// Optional plugins
		s_exprIsAiming    = g_script->CompileExpression("IsAiming");

		_MESSAGE("Detection expressions: IsRunning=%p IsSneaking=%p IsWeaponOut=%p GetAnimAction=%p",
				 s_exprIsRunning, s_exprIsSneaking, s_exprIsWeaponOut, s_exprGetAnimAction);
		_MESSAGE("Detection expressions (optional): IsAiming=%p", s_exprIsAiming);

		if (!s_exprIsAiming)
			_MESSAGE("WARNING: IsAiming not available (ShowOff NVSE not installed?) — aiming detection disabled");

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

	// Per-tick attack edge detection (runs every frame, not just at 20Hz)
	if (g_thePlayer && *g_thePlayer && s_exprGetAnimAction)
	{
		TESObjectREFR* player = *g_thePlayer;
		int animAction = (int)PollNumber(s_exprGetAnimAction, player);

		// Detect transition INTO attack actions:
		// 3=Attack, 4=AttackEject, 5=AttackFollowThrough, 6=AttackThrow
		bool wasAttacking = (g_prevAnimAction >= 3 && g_prevAnimAction <= 6);
		bool isAttacking  = (animAction >= 3 && animAction <= 6);

		if (isAttacking && !wasAttacking) {
			g_attackDetected = true;  // latch until consumed by next snapshot
		}

		g_prevAnimAction = animAction;
	}

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

			// Movement direction (computed from position delta)
			snap.moveDirection = ComputeMoveDirection(player->posX, player->posY, player->posZ, player->rotZ);
			snap.isRunning     = (uint8_t)PollNumber(s_exprIsRunning, player);
			snap.isSneaking    = (uint8_t)PollNumber(s_exprIsSneaking, player);
			snap.isWeaponOut   = (uint8_t)(PollNumber(s_exprIsWeaponOut, player) >= 1.0 ? 1 : 0);

			// Action state bitmask
			uint8_t action = ActionNone;
			double animAction = PollNumber(s_exprGetAnimAction, player);
			if ((int)animAction == 9) action |= ActionReloading;  // kAnimAction_Reload
			if (g_attackDetected) {
				action |= ActionFiring;
				g_attackDetected = false;  // consume the latched event
			}
			if (s_exprIsAiming && PollNumber(s_exprIsAiming, player) >= 1.0)
				action |= ActionAimingIS;
			snap.actionState = action;

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
