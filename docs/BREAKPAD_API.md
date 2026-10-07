# Pinned Breakpad API compatibility

New migration note, 2026-10-07. The original plugin source is
`ghostss2016/AcceleratorLocal` commit
`05149568304a05a3b90c252e805a2803bb78e933`; no existing Markdown was replaced.

The agreed official Breakpad pin
`6598c9c33fc02da7805401f3b0f1a733e6a24071` exposes a six-argument
`PrintProcessState`, while AcceleratorLocal's original writer calls four.
The local four-argument compatibility overload routes through production
`accelerator::PrintOriginalProcessState`: preserve the original state, stack
contents, requesting-thread selection and resolver; disable the added
stack-pointer dump option; use thread index `-1` for all threads.
The original writer body and metadata format remain fingerprint-identical.
The native harness tests the exact production argument mapping and one print
per call. This is not a proof of valid output during an engine crash.

The original `google_breakpad::scoped_ptr` usage has an explicit
`common/scoped_ptr.h` include; it no longer depends on obsolete transitive
includes from ExceptionHandler. The pinned `libbreakpad.a` already includes
`processor/stackwalk_common.cc`; do not add a second printer source object.

Primary source evidence:

- [pinned stackwalk_common.h](https://github.com/google/breakpad/blob/6598c9c33fc02da7805401f3b0f1a733e6a24071/src/processor/stackwalk_common.h)
- [pinned Makefile.am](https://github.com/google/breakpad/blob/6598c9c33fc02da7805401f3b0f1a733e6a24071/Makefile.am)

The same central dependency layout and PIC/ABI0 requirements from
`API18_MIGRATION.md` apply. No manual dependency build, game installation or
server restart is part of this source change.
