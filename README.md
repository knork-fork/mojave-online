# Mojave Online

A multiplayer mod for Fallout: New Vegas. Players connect to a dedicated server and see each other as NPCs in the game world, with position, rotation, and animation synced over the network.

This is a co-op mod for friends — anti-cheat is explicitly out of scope. The design prioritizes stability and performance over feature completeness.

## Status

Early development. Milestones v0.1–v0.3 are complete (networking, player snapshots, remote player avatars). See [planning/milestones.md](planning/milestones.md) for the full roadmap.

## How it works

- **`server.exe`** — standalone dedicated server that owns authoritative game state
- **`fnvmp.dll`** — NVSE plugin injected into the game client; sends/receives network data
- **`join_server.exe`** — launcher that bootstraps the game with connection info (planned, not yet implemented)

Remote players are represented as AI-disabled NPCs whose transforms and animations are driven entirely by network data — no engine pathfinding or AI packages involved.

## Prerequisites

- **Visual Studio 2022** (or 2019) with the "Desktop development with C++" workload
- **Fallout: New Vegas** with [xNVSE](https://github.com/xNVSE/NVSE/releases) installed

## Building

### From VS Code (recommended)

A build task is preconfigured. Press **Ctrl+Shift+B** to build the Release configuration.

This runs MSBuild against `fnvmp/fnvmp.sln` and produces both `fnvmp.dll` (client plugin) and `server.exe` (dedicated server).

### From command line

```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" fnvmp\fnvmp.sln -p:Configuration=Release -p:Platform=Win32 -m
```

### Post-build

The post-build step automatically copies:
- `fnvmp.dll` to `%FalloutNVPath%\Data\NVSE\Plugins\fnvmp.dll`
- `server.exe` to `%FalloutNVPath%\Server\server.exe`

Set the `FalloutNVPath` environment variable to your Fallout New Vegas installation directory.

## Project structure

```
fnvmp/              Solution root (fnvmp.sln)
  main.cpp          NVSE plugin entry points and game loop
  net/              Client networking (ENet wrapper)
  shared/           Protocol definitions (shared between client and server)
  server/           Dedicated server (server.vcxproj, server_main.cpp)
  enet/             Vendored ENet 1.3.18 (networking library)
nvse/nvse/          xNVSE source headers and cpp files compiled into the plugin
common/             Shared utility library (common_vc9)
planning/           Design documents and technical specification
geck_wiki_resource/ GECK wiki function/animation indexes for development reference
```

## Third-party code and data

This repository includes vendored source code and data from the following projects:

| Component | License | Location |
|-----------|---------|----------|
| [xNVSE](https://github.com/xNVSE/NVSE) | MIT | `nvse/` |
| common (NVSE utility lib) | zlib | `common/` — see [common/common_license.txt](common/common_license.txt) |
| [ENet](http://enet.besoun.net/) | MIT | `fnvmp/enet/` |
| [GECK Wiki](https://geckwiki.com/) | non-commercial | `geck_wiki_resource/` |

xNVSE is created and maintained by Ian Patterson, Stephen Abel, Paul Connelly, and Hugues LE PORS (original NVSE), with xNVSE development by korri123, jazzisparis, and Demorome.

`geck_wiki_resource/` contains an XML export and pre-built indexes from the [Community GECK Wiki](https://geckwiki.com/), used as a development reference for scripting commands and animation groups. The wiki content is [licensed for non-commercial use](https://geckwiki.com/index.php/GECK:General_disclaimer). The GECK and Fallout are trademarks of Bethesda Softworks LLC.
