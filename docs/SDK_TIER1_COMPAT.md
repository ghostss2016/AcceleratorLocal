# Current CS2 SDK library compatibility

New prerelease migration note, 2026-10-07. This follows AcceleratorLocal source
commit `aa3bd927` without replacing either existing migration Markdown file.
The SDK was inspected read-only on the central `.100` source-lock checkout:
`hl2sdk-cs2` commit `c0faf64a733f692e4316fff7b47b22eb94aa2bf9`.

That SDK has `lib/linux64/interfaces.a` and `mathlib.a` but no `tier1.a`.
The old `tier1/tier1.cpp` and `tier1/interface.cpp` are also absent. Its
`public/interfaces/interfaces.h` declares InterfaceReg and the exposure macro;
`public/tier0/interface.h` contains the lower-level interface declarations.
Read-only `nm` of its interfaces archive confirms definitions of InterfaceReg,
CreateInterface and ConnectInterfaces. Its tier0 library exports CommandLine.

The original AcceleratorLocal recipe unconditionally postlinked `tier1.a`,
which would fail before producing a usable binary with the current SDK.
The existing AMBuild recipe now uses `ResolveTier1Library`: link an archive
when present, allow its documented absence for CS2, and report an explicit
error for unexpected missing archives in the other legacy SDKs. Keep the
existing interfaces archive, memoverride source and tier0 link. This plugin
does not call ConnectTier1Libraries, use CTier1AppSystem, or instantiate
non-inline tier1 container/string helpers, so no replacement tier1 source
is required for its current calls. No old SDK source or substitute archive
is fabricated.

The compiler-free sourceguard executes the actual recipe resolver with
temporary SDK file fixtures: missing current CS2 archive, present archive,
missing legacy archive, legacy SDK names and the original x86 filename rule.
This proves the library-selection policy, not successful native linking.
The central builder still needs to validate all plugin and dependency symbols.
No compilation, game-file installation or restart is implied by this note.
