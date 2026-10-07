# AcceleratorLocal API18 migration

Source: `ghostss2016/AcceleratorLocal`, branch `master`, original commit
`05149568304a05a3b90c252e805a2803bb78e933`. This is a modified fork of the
Phoenix/Asher Baker GPL v3 plugin; original attribution, upstream URL and
history are preserved. This document is new, not an overwritten instruction.

## Behavior

Post `IServerGameDLL::GameFrame` → inspect the original five fatal signals →
restore the captured Breakpad actions if another handler replaces them → no
write on an unchanged frame. Reads and writes are checked. The frequency and
maximum size remain one pass over five signals per frame; no worker or queue
is added. A failed signal read does not write an uninitialized action.

Post `INetworkServerService::StartupServer` → store the map name for the next
crash. Late load takes the current map when a game server exists. Null names
become empty metadata and long names/paths have bounded, terminated storage.
No fabricated `GameSessionConfiguration_t` object/layout is used.

Breakpad fatal-signal callback → retain the original `.dmp` → retain the
original `.dmp.txt` metadata, console history in reverse order, console stack
and appended process state. The writer itself is unchanged except for
lock-free callback bookkeeping; its normalized original SHA is guarded.

Both hooks are owned typed `SvarogHooks::Virtual` registrations from the shared
`SchemaEntity/metamod_virtual_hook.h`. Member pointers select the SDK virtual
methods; there are no engine signatures or numerical vtable offsets. Both
callbacks return `KHook::Action::Ignore`, so KHook invokes the original once.
Actual API18 headers are required; no plugin API version is redefined.

## Lifetime

Before installing Breakpad, record the previous signal actions. After
construction, require complete changed `SA_SIGINFO` actions for the monitored
signals. Hook rejection rolls back the partial pair before any signal handler
is installed. A later Breakpad setup failure synchronously rolls back the
hooks and deletes the handler before reporting Load failure.

Unload first refuses active engine/crash callbacks or an unavailable KHook
provider while hooks are still owned. Accepted unload removes the StartupServer
hook and then GameFrame synchronously, deletes Breakpad, clears its pointer,
and resets the signal/metadata/interface state. Breakpad owns restoration of
the previous signal handlers under its own handler-stack mutex. Unchanged
empty cleanup is idempotent. No asynchronous plugin callbacks or detached
workers are introduced.

## Central build inputs

Use only the existing panel `.238` → central `cs2-ci` `.100` route, target
`AcceleratorLocal`. Preserve the existing AMBuild recipe and package:

- `addons/accelerator_local/accelerator_local.so`
- `addons/metamod/accelerator_local.vdf`, alias `accelerator_local`, file
  `addons/accelerator_local/accelerator_local`

Required source-lock dependencies are the selected HL2SDK-CS2, genuine API18
MetaMod with its pinned KHook submodule, and `SchemaEntity` with the existing
owned virtual-hook helper. KHook's opaque-reference ABI header repair belongs
to the central trusted recipe, never to a fake configuration in this plugin.

The absent external dependency must be provisioned by central CI, with exact
source/tool hashes and offline upstream configure/make. The agreed official
pins are Google Breakpad `6598c9c33fc02da7805401f3b0f1a733e6a24071` and its
Linux syscall support `29164a80da4d41134950d76d55199ea33fbb9613`.
Preserve the upstream recipe's paths:

- sources/headers: `breakpad/src/src/`
- `breakpad/build/src/client/linux/libbreakpad_client.a`
- `breakpad/build/src/libbreakpad.a`
- `breakpad/build/src/third_party/libdisasm/libdisasm.a`

Static archives must use Linux x86_64, PIC and `_GLIBCXX_USE_CXX11_ABI=0`,
matching the plugin. pthread and zlib remain link dependencies. Additional
Breakpad common source files from the original AMBuilder are retained.
Do not build Breakpad manually, add another Makefile/dispatch script, or
activate a per-plugin GitHub workflow.

## Validation and limits

`tests/api18_hook_contract_test.py` runs without a compiler. It checks migration
policy, original writer/package/native declaration fingerprints and recipe
syntax. It does not demonstrate native ABI correctness.

`tests/accelerator_runtime_test.cpp` imports production `accelerator_runtime.h`.
Central `ci/plugin-checks.sh` must compile/run it with pthread and ASan/UBSan.
Success marker: `accelerator_runtime_test: all checks passed`.
It checks rejected/partial/repeated registration, cleanup order, active callback
and missing-provider unload refusal, thread-visible callback counts, later
initialization rollback, bounded/null map and command-line data, five-signal
repair, no unchanged writes, read/write failure, retained complete snapshots
and reset. Only hook registration and signal I/O are replaced by boundaries.

The existing opaque `CMiniDumpComment` size declaration (`0x28`) remains
unchanged. It is a pre-existing engine ABI assumption, not a declaration
verified against the new engine. The existing crash writer still invokes the
resolver, allocation and stdio in a compromised crash context. These facts
must be tested separately; source guards/native ownership tests do not prove
crash safety, signal-time deadlock freedom, dump validity or real game ABI.
The central shared KHook native regression validates the pinned helper at its
detour boundary separately from the real server.

This source change does not install game files or restart any server. Build,
catalog import, card synchronization and game/runtime verification are
separate outcomes and must be reported from their actual evidence.
