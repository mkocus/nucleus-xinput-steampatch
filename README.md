# nucleus-xinput-steampatch

XInput proxy DLL that filters controllers per game instance, surviving Steam overlay function hooking.

Built for [Nucleus Co-op](https://github.com/SplitScreen-Me/splitscreenme-nucleus) split-screen scenarios where each instance should see only its assigned controller.

## Problem

When using XInputPlus-style proxy DLLs to separate controllers between game instances, **Steam overlay** (`gameoverlayrenderer.dll`) overwrites the proxy's `XInputGetState` export with a `JMP` detour to its own handler. Steam's handler calls the system `xinput1_4.dll` directly, completely bypassing the proxy's controller filtering. This causes all instances to respond to all controllers.

This happens even when the Steam overlay is disabled in Steam settings — the DLL is still injected and hooks are still installed.

### What doesn't work

| Approach | Why it fails |
|---|---|
| Restoring original function bytes | Steam re-hooks immediately (every frame) |
| Reading bytes from DLL on disk | The compiler emits JMP thunks — disk bytes are also JMPs |
| Goldberg Steam Emulator | Bundled version too old for modern Steamworks SDK |
| Stub `gameoverlayrenderer.dll` in game folder | Steam injects via full absolute path, ignores local DLL |
| Disabling overlay in Steam settings | DLL is still injected and hooks still installed |

## Solution

Instead of fighting Steam's hook, **take it over**:

1. A background thread sleeps 3 seconds (giving Steam time to install its hooks)
2. Reads the `JMP rel32` target address from our hooked `XInputGetState` — this is Steam's internal handler
3. Overwrites the `JMP` to point at our **filtering trampoline** instead
4. Our trampoline applies the controller map from `XInputPlus.ini`, then calls Steam's handler with the remapped index
5. Monitors for 60 seconds in case Steam re-hooks

### Call flow

```
Game calls XInputGetState(0)
  → [our xinput1_3.dll export]
    → [JMP — rewritten to point at our trampoline]
      → hooked_getstate(0)
        → g_map[0] == -1? → return ERROR_DEVICE_NOT_CONNECTED
        → g_map[0] == 2?  → call Steam_handler(2, pState)
          → Steam overlay internals
            → system xinput1_4.dll
```

## Building

Requires Visual Studio Build Tools (or full Visual Studio) with the C++ desktop workload.

```bat
build.bat
```

Produces 4 DLLs:

```
x86\xinput1_3.dll    x86\xinput1_4.dll
x64\xinput1_3.dll    x64\xinput1_4.dll
```

Choose the DLL matching your game's architecture (x86/x64) and XInput version (1_3/1_4).
Most older games (XNA, Unity 4) use `xinput1_3.dll`. Newer games often use `xinput1_4.dll`.

## Usage with Nucleus Co-op

1. Build or download the DLLs
2. Pick the correct variant for your game (e.g. `x86\xinput1_3.dll` for Terraria)
3. Replace `NucleusCoop\utils\XInputPlus\x86\xinput1_3.dl_` (or the x64 equivalent) with the built DLL  
   (Nucleus renames `.dl_` → `.dll` when copying to game instances)
4. Ensure the game handler has `Game.XInputPlusDll = ["xinput1_3.dll"]` (or `xinput1_4.dll`)
5. Launch via Nucleus as usual

The DLL writes a log file (`xinput_proxy.log`) next to itself for diagnostics.

## Troubleshooting

The proxy writes `xinput_proxy.log` in the same directory as the DLL. Nucleus cleans up instance folders after the game exits, so **copy the log while the game is still running**.

Instance folders are located at: `NucleusCoop\content\<GameName>\Instance0\`, `Instance1\`, etc.

### What to look for in the log

| Log line | Meaning |
|---|---|
| `XInput proxy loaded` | DLL is being loaded — basic setup works |
| `Counter-hooked XInputGetState: Steam(...) -> filter(...)` | Steam overlay was detected and counter-hooked — this is the fix working |
| `XInputGetState not hooked by Steam` | No Steam hook found — proxy filtering works directly without counter-hook |
| `Re-hooked XInputGetState (pass N)` | Steam re-applied its hook and we patched it again |
| `Failed to load xinput1_4.dll` | System XInput DLL not found — should not happen on any Windows 10/11 |

### Common issues

- **Log file is empty or missing** — the DLL isn't being loaded. Check that the `.dl_` file in `utils\XInputPlus\x86\` (or `x64\`) was replaced correctly and that `Game.XInputPlusDll` in the handler matches the DLL name.
- **Log shows `Controller1 = 0` for all controllers** — Nucleus assigned no controllers to this instance (keyboard+mouse instance). This is correct behavior.
- **Counter-hook installs but controller still controls both instances** — the game may load XInput from a different DLL name. Check which `xinput*.dll` the game imports (use a tool like Dependencies or `dumpbin /imports`).

## Files

| File | Purpose |
|---|---|
| `xinput1_3.c` | Proxy DLL source — filtering + counter-hook (x86 & x64) |
| `xinput1_3.def` | Export definitions for xinput1_3.dll |
| `xinput1_4.def` | Export definitions for xinput1_4.dll (adds `XInputGetAudioDeviceIds`) |
| `build.bat` | Build script — produces all 4 variants |

## Tested with

- Terraria (XNA Framework 4.0, x86, xinput1_3) via Nucleus Co-op v2.4.1
- Windows 11 Pro, Steam overlay enabled and disabled
- Single physical Xbox controller, one instance keyboard+mouse / one controller

### Architecture support

- **x86**: Steam overlay uses `E9` (JMP rel32) hooks — fully tested
- **x64**: Steam overlay uses `FF 25` (indirect JMP with 8-byte address) or `E9` — supported, not yet field-tested

## License

MIT
