# FNV Modding - NVSE Plugin

## Building

Run MSBuild against the solution:

```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" fnvmp\fnvmp.sln -p:Configuration=Release -p:Platform=Win32 -m
```

Post-build step copies the DLL to `C:\Games\Fallout New Vegas\Data\NVSE\Plugins\fnvmp.dll`.

## Project structure

- `fnvmp/` - main plugin source (main.cpp, .vcxproj, .sln)
- `nvse_plugin_example/` - original NVSE example plugin (kept as working prototype reference)
- `nvse/nvse/` - NVSE source headers and cpp files compiled into the plugin
- `common/` - shared utility lib (common_vc9)
- `planning/` - design documents and technical specifications
  - `initial_plan.txt` - original design notes and decisions
  - `technical_spec.md` - detailed technical specification (authoritative reference for implementation)
  - `deferred_features.md` - features explicitly excluded from v1

## What this project is

This is a **multiplayer mod** for Fallout: New Vegas. Spawned "NPCs" are actually **avatars representing other players** connected to a server. Their position, rotation, and animation state are received from the network and applied locally. Key implications:

- **No AI packages or engine pathfinding.** Avatar transforms are driven entirely by server data — the engine must not autonomously decide where to move them.
- **Per-tick position updates.** Avatar positions must be updated every game tick for smooth movement, interpolating between network updates.
- **Hardcoded positions are temporary.** Current constants are placeholders; real data will come from the server.

## NVSE API gotchas

- **`RunScriptLine2` bSuppressConsoleOutput is broken.** The `ToggleConsoleOutput` calls in `GameScript.cpp:349,366` are commented out. The bool parameter does nothing.
- **`ToggleConsoleOutput` is not exported to plugins.** It lives in NVSE's DLL (`Hooks_Gameplay.cpp`). To suppress console output from a plugin, patch `Console::Print` directly using `SafeWrite8` on `s_Console__Print` (`0x0071D0A0`, defined in `GameAPI.h`):
  - Disable: write `0xC2 0x08 0x00` (retn 8)
  - Restore: write `0x55 0x8B 0xEC` (push ebp; mov ebp, esp)
  - Note: NVSE's own source has a typo (`0x88` instead of `0x8B`) in the restore path.
- **`SafeWrite.h` / `SafeWrite.cpp`** are compiled into the plugin and available for runtime patching.
- **Don't trust NVSE API parameters blindly.** Read the NVSE source to verify what's actually implemented vs stubbed/commented out.

## GECK wiki reference (scripting commands & anim groups)

`geck_wiki_resource/` contains a full GECK wiki XML export and pre-built index files for fast lookup.

### Index files (search these, don't read the raw XML)

- **`geck_wiki_resource/geck_function_index.txt`** — 2500+ functions. One line per function: `Name | Origin | Signature | Summary | Categories`. Grep this for keywords when the user asks about scripting commands.
- **`geck_wiki_resource/geck_anim_groups.txt`** — All animation group IDs (hex, decimal, name). Grep for animation-related questions.
- **`geck_wiki_resource/geck_other_pages_index.txt`** — Non-function wiki pages (guides, category listings).

### Lookup workflow

1. Grep the index file(s) for keywords related to the user's question (e.g. `weapon|holster|draw` for "how to unholster weapon").
2. Identify the relevant function(s) from the results.
3. If full details are needed (arguments, notes, caveats), grep the original XML: `geck_wiki_resource/GECK-20260301214412.xml` for `<title>FunctionName</title>` and read that page's `<text>` block.

### Rebuilding the index

If the XML is updated, re-run: `powershell -ExecutionPolicy Bypass -File geck_wiki_resource/build_index.ps1`
