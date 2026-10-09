# EFML — Endfield Mod Loader

A mod loader and manager for *Arknights: Endfield* (Windows x64), built from the core of
[Better-Endfield](https://github.com/Dr-hydra/Better-Endfield): its loaders, IL2CPP resolver, shared
hook chain and third-party module host, without the built-in feature modules.

- **Compatible with Better-Endfield modules.** Same package format (`module.json` format 1 / ABI 1),
  same index and RPC, same page bridge. Modules built for Better Endfield 3.x
  (`BetterEndfield_GetThirdPartyModuleV1`) or Next 4.x (`BetterEndfieldNext_GetThirdPartyModuleV1`)
  load without recompiling.
- **More for module authors:** game-thread tasks and per-frame callbacks, an IL2CPP-attached module
  worker, services shared between modules, host events, runtime-gated loading with retries.
- **Two ways to load:** inject when the manager starts the game (nothing written to the game
  folder), or an `xinput1_4.dll` proxy in the game folder (works however the game is started).

> Work in progress. The native runtime is built and tested; the manager is being written.

## Layout

| Path | Contents |
|---|---|
| `native/include/EFML/` | Public SDK headers (`Module.h`, `ModuleApi.h`, `HookChain.h`) |
| `native/include/BetterEndfield*/` | Compatibility headers for Better-Endfield sources |
| `native/host/` | `EFML.Host.dll`: module host, RPC, hook broker, IL2CPP resolver, game thread |
| `native/loader/` | `xinput1_4.dll` proxy, `EFML.Bootstrap.dll`, `EFML.Injector.exe` |
| `native/tests/` | Module host tests (no game needed) |
| `sdk/examples/echo/` | Example module with a page |

## Build

GitHub Actions builds every push (`.github/workflows/build.yml`). Locally, either:

```bat
native\build.cmd
```

(MSVC only, no CMake), or

```bat
cmake -S native -B build\native-cmake -A x64
cmake --build build\native-cmake --config Release
ctest --test-dir build\native-cmake -C Release
```

## License

AGPL-3.0, like Better-Endfield. See [LICENSE](LICENSE) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
