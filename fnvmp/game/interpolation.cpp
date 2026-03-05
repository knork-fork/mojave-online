#include "interpolation.h"

#include <map>
#include <cmath>

// --------------------------------------------------
// Internal types
// --------------------------------------------------

static constexpr float PI = 3.14159265f;
static constexpr int BUFFER_SIZE = 5;

struct SnapshotSample {
    float  posX, posY, posZ;
    float  rotZ;
    double timestamp;
};

struct EntityInterpState {
    SnapshotSample samples[BUFFER_SIZE];
    int   count    = 0;  // valid samples (0..BUFFER_SIZE)
    int   writeIdx = 0;  // next write slot

    float velX = 0, velY = 0, velZ = 0;  // estimated velocity for extrapolation
};

static std::map<uint32_t, EntityInterpState> g_interpStates;

// --------------------------------------------------
// Helpers
// --------------------------------------------------

// Get sample at logical index (0 = oldest, count-1 = newest) from ring buffer.
static const SnapshotSample& GetSample(const EntityInterpState& state, int logicalIdx)
{
    // writeIdx points to the next write slot.
    // oldest = (writeIdx - count + BUFFER_SIZE) % BUFFER_SIZE
    int oldest = (state.writeIdx - state.count + BUFFER_SIZE) % BUFFER_SIZE;
    int actual = (oldest + logicalIdx) % BUFFER_SIZE;
    return state.samples[actual];
}

static float LerpAngle(float a, float b, float t)
{
    float diff = b - a;
    while (diff > PI) diff -= 2.0f * PI;
    while (diff < -PI) diff += 2.0f * PI;
    return a + t * diff;
}

// --------------------------------------------------
// Public API
// --------------------------------------------------

void Interp_PushSnapshot(uint32_t entityId, float x, float y, float z, float rotZ, double localTime)
{
    EntityInterpState& state = g_interpStates[entityId];

    // Compute velocity from previous newest sample (if we have at least one)
    if (state.count > 0) {
        const SnapshotSample& prev = GetSample(state, state.count - 1);
        double dt = localTime - prev.timestamp;
        if (dt > 0.001) {
            state.velX = (float)((x - prev.posX) / dt);
            state.velY = (float)((y - prev.posY) / dt);
            state.velZ = (float)((z - prev.posZ) / dt);
        }
    }

    // Write into ring buffer
    SnapshotSample& s = state.samples[state.writeIdx];
    s.posX = x;
    s.posY = y;
    s.posZ = z;
    s.rotZ = rotZ;
    s.timestamp = localTime;

    state.writeIdx = (state.writeIdx + 1) % BUFFER_SIZE;
    if (state.count < BUFFER_SIZE)
        state.count++;
}

bool Interp_GetState(uint32_t entityId, double renderTime, double currentTime,
                     float& outX, float& outY, float& outZ, float& outRotZ)
{
    auto it = g_interpStates.find(entityId);
    if (it == g_interpStates.end()) return false;

    const EntityInterpState& state = it->second;
    if (state.count == 0) return false;

    const SnapshotSample& oldest = GetSample(state, 0);
    const SnapshotSample& newest = GetSample(state, state.count - 1);

    // Case 1: Only one sample or renderTime is before the oldest — snap to oldest
    if (state.count == 1 || renderTime <= oldest.timestamp) {
        outX = oldest.posX;
        outY = oldest.posY;
        outZ = oldest.posZ;
        outRotZ = oldest.rotZ;
        return true;
    }

    // Case 2: renderTime is between two samples — interpolate
    if (renderTime <= newest.timestamp) {
        // Find the two samples bracketing renderTime
        for (int i = 0; i < state.count - 1; ++i) {
            const SnapshotSample& a = GetSample(state, i);
            const SnapshotSample& b = GetSample(state, i + 1);

            if (renderTime >= a.timestamp && renderTime <= b.timestamp) {
                double dt = b.timestamp - a.timestamp;
                float t = (dt > 0.0001) ? (float)((renderTime - a.timestamp) / dt) : 0.0f;

                outX = a.posX + t * (b.posX - a.posX);
                outY = a.posY + t * (b.posY - a.posY);
                outZ = a.posZ + t * (b.posZ - a.posZ);
                outRotZ = LerpAngle(a.rotZ, b.rotZ, t);
                return true;
            }
        }

        // Shouldn't reach here, but fall through to newest
    }

    // Case 3: renderTime is beyond the newest sample — extrapolate or freeze
    double timeSinceNewest = currentTime - newest.timestamp;

    if (timeSinceNewest > EXTRAP_TIMEOUT) {
        // Freeze at newest position
        outX = newest.posX;
        outY = newest.posY;
        outZ = newest.posZ;
        outRotZ = newest.rotZ;
        return true;
    }

    // Extrapolate using stored velocity
    float dt = (float)(renderTime - newest.timestamp);
    outX = newest.posX + state.velX * dt;
    outY = newest.posY + state.velY * dt;
    outZ = newest.posZ + state.velZ * dt;
    outRotZ = newest.rotZ;  // don't extrapolate rotation
    return true;
}

void Interp_RemoveEntity(uint32_t entityId)
{
    g_interpStates.erase(entityId);
}

void Interp_Clear()
{
    g_interpStates.clear();
}
