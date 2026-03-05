#pragma once

#include <cstdint>

// Interpolation constants
// Render delay = 2 packet intervals — enough to absorb one dropped/late packet.
static constexpr double INTERP_DELAY    = 2.0 / 30.0;  // ~67ms at 30Hz send rate
static constexpr double EXTRAP_TIMEOUT  = 0.500;        // 500ms before freezing

// Push a new position/rotation sample for an entity.
// localTime = client-local monotonic clock at time of receipt.
void Interp_PushSnapshot(uint32_t entityId, float x, float y, float z, float rotZ, double localTime);

// Get interpolated (or extrapolated) state for an entity at the given render time.
// renderTime should be currentTime - INTERP_DELAY.
// currentTime is the raw current time (used for extrapolation timeout check).
// Returns false if entity has no samples.
bool Interp_GetState(uint32_t entityId, double renderTime, double currentTime,
                     float& outX, float& outY, float& outZ, float& outRotZ);

// Remove all interpolation data for an entity (on despawn).
void Interp_RemoveEntity(uint32_t entityId);

// Clear all interpolation data (on shutdown).
void Interp_Clear();
