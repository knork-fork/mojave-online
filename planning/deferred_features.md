# Deferred Features (Not in v1)

Features explicitly excluded from the initial version. These may be revisited in future versions.

---

## Networking & Protocol

- **Protocol version negotiation** — v1 requires matching client/server builds; no handshake or version check
- **Position quantization** — snapshots use raw floats; quantization is a future bandwidth optimization
- **Bandwidth optimization** — delta compression, interest management, etc.

## Chat & Communication

- **Text chat** — no in-game chat system; players use external tools (Discord, etc.)
- **Voice chat** — not planned

## World State Sync

- **Container contents sync** — each player sees independent container contents
- **Vendor inventory sync** — each player has independent vendor stock
- **Lock/door state sync** — locks reset when zone empties; each client handles independently
- **World object sync** — picked-up items, moved objects, traps are all local; item duplication is accepted
- **Cleared areas sync** — each client tracks independently

## Persistence

- **Quest state persistence** — quests are local to each client's save, not stored on server
- **Faction reputation persistence** — not tracked by server
- **World modification persistence** — opened doors, moved objects, etc. reset when zone empties

## Loot & Inventory

- **Corpse loot sync** — corpse inventory is populated locally from known player data, but removing items from a corpse is not synced between clients
- **Loot drop sync** — items dropped in the world are not synced

## Player Appearance

- **Face/body customization sync** — all player avatars use the same generic base NPC form; player's character creation choices (face, hair, body) are not transmitted or applied to their avatar on other clients
- **Player appearance persistence** — server does not store appearance data

## Animation

- **Swimming animations** — swimming state is not synced; avatars will not play swim animations when the source player is swimming. The engine exposes `IsSwimming` (returns 0/1) which could be used to detect and sync this state in a future version
- **Fine rotation** — only yaw (Z rotation) is synced; no pitch/roll
- **Lip sync / facial expressions** — not synced
- **Ragdoll sync** — each client handles ragdoll independently after Kill()

## Projectile & Placed Entity Sync

- **Thrown weapon projectiles** (grenades, spears, etc.) — the projectile entity itself is not synced between clients; only the throw animation plays on the remote avatar. The projectile exists only on the thrower's client. These weapons are more rarely used than standard ranged and melee weapons.
- **Placed mines** — mines placed in the world are not synced; they exist only on the placer's client

## Combat

- **Anti-cheat** — no validation beyond basic sanity checks (distance, rate); game is for co-op with friends
- **PvP** — no explicit PvP system; all players are in companion faction

## Server

- **Admin tools / RCON** — no remote admin interface; SQLite DB can be inspected directly
- **Server browser / matchmaking** — direct IP connect only
- **Multiple worldspace support** — not explicitly excluded but not tested or designed for DLC worldspaces

## Client

- **GUI launcher** — v1 uses CLI; GUI may be added later
- **In-game server browser** — not planned
- **Mod compatibility layer** — no special handling for other mods; may conflict
