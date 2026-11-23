# Source provenance

Palmwave is published with an independent Git history. Its initial source snapshot is derived from [ItsNotRexed/Leonida](https://github.com/ItsNotRexed/Leonida) at commit [`dde82f27e994337094afc476ded0b1426f6e87e2`](https://github.com/ItsNotRexed/Leonida/commit/dde82f27e994337094afc476ded0b1426f6e87e2), under GPL-3.0.

Original source copyright headers, LICENSE, technical documentation and bundled third-party notices are retained. The initial import does not claim original authorship of the inherited implementation. Historical measurements in the documentation are not new Palmwave test results.

The `third_party/rpf9-explorer` submodule remains pinned to `e2939175c36a02d6ed25fa2c217b9d6ddcb8b516` from [breadbbbb/rpf9-explorer](https://github.com/breadbbbb/rpf9-explorer).

The initial Palmwave changes are branding, project documentation and a separate local measurement record. No runtime code from XCL, Five Star, OpenScarlett or xbox-emulator has been imported in this initial commit. Future imports must record their source revision, license, authorship, rationale and tests here or in a component notice.

Public implementation notes from other projects can help identify a question to test. They do not substitute for a local trace. Do not copy GPL-2.0-only code into this GPL-3.0 project. Preserve MIT and other applicable notices when incorporating compatible components.

## XErrorReport export

The flat `XErrorReport` export and its synthetic runtime/loader tests are adapted from dracbat's implementation, rebased by Kam471 in [Leonida PR #6582](https://github.com/ItsNotRexed/Leonida/pull/6582), commit [`4224c53b145ea9e68eb579817d772fe7276b943a`](https://github.com/Kam471/Leonida/commit/4224c53b145ea9e68eb579817d772fe7276b943a), under GPL-3.0. A local offline boot stopped when `GetProcAddress` could not resolve this name. The export delegates to the existing offline `IXErrorImpl` report handler; `xgameruntime` and `run_tiny_gdk_load` cover lookup and callback delivery, with an additional message/context check in Palmwave.

## Executable TLS index zero

The early TLS-index-zero reservation, host-template clone and synthetic `/GA` loader tests are adapted from mariuslozo-boop's [Leonida PR #4313](https://github.com/ItsNotRexed/Leonida/pull/4313), final revision [`a9085b661c3637ce4d154dab3c0d5b4a436d90ec`](https://github.com/mariuslozo-boop/Leonida/commit/a9085b661c3637ce4d154dab3c0d5b4a436d90ec), under GPL-3.0, including Roberto0953's rebase and existing-thread correction. The upstream commits credit Claude Opus 5.5 as co-author. Local offline debugging established direct slot-zero access while the loader had assigned the executable another TLS index. The runtime now reserves zero before its C++ initializers and keeps its own TLS at a separate, OS-backed home index; the imported tests cover host/guest isolation, existing and new threads, reservation refusal, repeated loads and temporary-clone cleanup. Their local execution results are recorded separately in `STATUS.md`.

## DirectStorage factory

`runtime/shims/dstorage.cpp`, `runtime/shims/dstorage_xs_abi.h` and `runtime/test/dstorage_test.cpp` are adapted from mariuslozo-boop's [Leonida PR #4348](https://github.com/ItsNotRexed/Leonida/pull/4348), final revision `45c39cf6f4148aebbc8a88d476bea755887d35bd`, under GPL-3.0. The PR was closed unmerged, and its author permitted it to be taken over. The upstream commits credit Claude Opus 5.5 as co-author.

A local offline boot stopped at `xcrdapi.dll!DStorageGetFactory`, with factory id `62959d11-d45f-4884-a0a3-f7862ea243ae`. The PR's CPU file and memory queues, status arrays, fences, events, cancellation and error records are kept as written.

Palmwave changed the following:
- The PR learned unknown interface ids at run time and accepted the PC `IDStorageStatusArray` id. Both are replaced by a strict table that holds only the measured retail factory id.
- `OpenFile` no longer falls back to the host `CreateFileW` when the file-system shim is absent.
- `xcrdapi.dll` is registered as the primary name.
- Tests were added for raw factory slots 8 and 10, strict-id rejection and unsupported-request sentinels.

FiveStar (GPL-3.0-or-later) and OpenScarlett (MIT) were consulted for ABI facts only, through the native review; no code was copied from either. The adaptation was written with Claude Code (Claude Opus 5.5). Test results are recorded where root runs them, not here. See [docs/directstorage.md](docs/directstorage.md).

## Project identity and tools

Palmwave is developed under [Moon AI](https://realmoon.ai), the AI platform created by Stellarr Studio. The maintainer account is [fablinoo](https://github.com/fablinoo). The initial setup was AI-assisted with OpenAI Codex and OpenAI image generation; Claude Code was assigned a read-only source review. This records the actual tool provenance.
