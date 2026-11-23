# DirectStorage (`xcrdapi.dll!DStorageGetFactory`)

The retail executable imports one DirectStorage function, `XCRDAPI.dll!DStorageGetFactory`
([runtime_imports.md](runtime_imports.md#the-retail-executable-rt-5)). Its single call site passes the address of an
interface id and of a global, then tests the global for null ([audio_hal_exe_calls.md](audio_hal_exe_calls.md)).
The local offline boot stopped at this import. A null factory leads to a fatal assert in the title
([retail_boot_chain.md](retail_boot_chain.md)). `runtime/shims/dstorage.cpp` serves it. The shim is adapted from
upstream PR #4348 ([UPSTREAM.md](../UPSTREAM.md#directstorage-factory)).

## What is measured, sourced or assumed

| Fact | Status | Where it comes from |
|---|---|---|
| Module `XCRDAPI.dll` and the shape `HRESULT DStorageGetFactory(REFIID, void**)` | measured | the executable's import table and call site |
| Factory id `{62959d11-d45f-4884-a0a3-f7862ea243ae}` | measured | the local offline boot (the id the title passes). The public OpenScarlett header (MIT) names it `IDStorageFactoryX2` |
| Factory slot 8 `SetStagingBufferSize(UINT32)` and slot 10 `CreateQueue1(desc1, REFIID, void**)` | sourced, reviewed | the PR's declaration order. The native review matched it against FiveStar (GPL-3.0-or-later) and OpenScarlett. `dstorage_test` pins both slots through the raw vtable |
| Factory slots 3-7 and 9 | PR order, unverified | PC `dstorage.h` order plus the GDK overview. FiveStar agrees on 4 (`OpenFile`) and 5 (`CreateStatusArray`) |
| Queue and status-array interface ids | **not measured locally** | none is accepted yet; see below |
| Queue and file signatures | primary Xbox documentation | Microsoft documents `HANDLE GetHandle()` on FileX and adds `EnqueueSetEvent` in QueueX1 after base QueueX methods. Other projects disagree, so their alternate tables are not adopted. Exact ordinals remain source-derived. |
| Struct layouts, `E_DSTORAGE_*` values, capacity bounds | sourced | the GDK reference and the PC `dstorage.h`/`dstorageerr.h` 1.3.0, pinned with `static_assert` in `dstorage_xs_abi.h` |

## Interface ids: strict

The creating calls (`DStorageGetFactory`, `CreateQueue`, `CreateQueue1`, `OpenFile`, `CreateStatusArray`) and every
`QueryInterface` apply the same rule. They accept `IUnknown`, or an id that `kKnownInterfaceIds` in `dstorage_xs_abi.h`
lists for that kind of object. Today that list has only the retail factory id. Any other id gets `E_NOINTERFACE`,
with `*ppv` cleared and nothing created or opened. Refused ids include unknown ones, another kind's id, system COM
ids, and every PC DirectStorage id, including `IDStorageStatusArray`, whose shape happens to match. Each refusal is
logged once per caller as `[rt] dstorage: <caller> asked for {id}: <reason> (E_NOINTERFACE)`. Nothing is learned at
run time, so the same id is refused again. The PR's policy of accepting and remembering any new id was removed.

**Measured next blocker:** the offline probe reaches `OpenFile` and requests file id `{43704f9f-27c6-4a9f-8815-d16527894df4}`. This revision refuses it. The follow-up must pair that id with the documented Xbox `GetHandle` contract. Queue and status ids have not yet been observed locally.

## What the shim does

These parts are unchanged from the PR except where noted.

- **Factory:** one object per process that is never freed. The factory itself does not change between aliases:
  `xcrdapi.dll`, `dstorage_xs.dll` and `dstorage.dll` all resolve to the same `DStorageGetFactory`. Each call
  behaves like this:
  - `SetStagingBufferSize` accepts 0/16/20/24/28/32 MiB and is refused while a queue or file is live. It stores
    the size only. No staging buffer is allocated, because these direct reads do not use one.
  - `SetCpuAffinity` and `SetDebugFlags` store their values only.
  - `GetStats` reports the counters the shim keeps.
- **Files:** `OpenFile` calls the registry's `kernel32.dll!CreateFileW`, which is `file_system.cpp`. A `GAME:` path
  therefore gets the same mapping, overlay and `..`/drive sandbox as the title's own opens. A host path goes to the
  host, as the title's own `CreateFileW` does. **Palmwave change:** the PR fell back to the host `CreateFileW` when
  that shim was missing. This shim instead returns `E_NOTIMPL` and opens nothing. `GetHandle` returns a separate
  read-only handle from `ReOpenFile`. `Close` waits for the requests already submitted against the file.
- **Queues:** capacity must be `0x80`..`0x2000`, and a memory queue must be `REALTIME`. Auto-submit fires at half
  capacity unless it is disabled. An enqueue into a full queue waits. One CPU worker thread per queue runs the
  submitted entries in order:
  - A file request is a positioned `ReadFile` of exactly `SourceSize` bytes.
  - A memory request is a guarded `memmove`.
  - A status entry, `ID3D12Fence::Signal` (PC slot 10, which the `d3d12_xs` fence proxy forwards) and
    `EnqueueSetEvent` each complete only after every entry before them.
  - `CancelRequestsWithTag` marks requests that have not started. A cancelled request counts as a success with
    nothing read.
  - Failures are recorded in the status entry, the error record and the error event.
- **Unsupported requests fail honestly:**
  - zlib, BCPack (modes 1-7) and swizzle (modes 1-4) return `E_NOTIMPL`.
  - Physical source or destination pages return `E_NOTIMPL`.
  - An unpaged buffer over 32 MiB returns `E_DSTORAGE_REQUEST_TOO_LARGE`.
  - Each of these checks runs before the destination is touched, so such a request never completes as a success
    with empty or stale data.
  - Invalid modes, reserved bits and size mismatches return the matching `E_DSTORAGE_*` error.

Nothing here goes online, sends telemetry or touches the GPU. Logging goes to stderr only.

## Tests

`runtime/test/dstorage_test.cpp` (ctest `dstorage`) uses synthetic files in `%TEMP%`, with no game data and no GPU:

- **Inherited from the PR:** layout and option bits, `GAME:` and overlay reads, offsets, EOF, cancellation,
  auto-submit and a full queue, status arrays, fence ordering, events, error records, file and queue close with
  requests in flight, an unwritable destination, four concurrent producers, memory queues, and statistics.
- **Added for Palmwave:**
  - Raw-vtable calls to slot 8 and slot 10 on the factory that `xcrdapi.dll` returns for the retail id. The test
    checks size validation and the lock, and that the queue slot 10 creates really reads file bytes.
  - Strict-id rejection: each id is asked twice, every refused output is cleared, and a refused id creates no queue
    and opens no file.
  - Unsupported requests leave a sentinel destination unchanged and are recorded as failures.

`unimplemented_policy_test` checks that `xcrdapi.dll` / `XCRDAPI.dll!DStorageGetFactory` is classified as returning
an HRESULT, and that other `xcrdapi.dll` names are not.

## Not done

- The queue, file and status interface ids and their vtables, as described above.
- zlib/DEFLATE, then BCPack (`tools/bcpack.py` is the reference decoder), then swizzle, as stages between the read
  and the destination write.
- Physical pages (this needs `xmem` page-number translation).
- Routing writes into Vulkan-backed GPU memory.
- Parallel reads.

## Sources and licences

| Source | Use |
|---|---|
| [upstream PR #4348](https://github.com/ItsNotRexed/Leonida/pull/4348), mariuslozo-boop, commit `45c39cf6f4148aebbc8a88d476bea755887d35bd`, GPL-3.0 | Code adapted (`dstorage.cpp`, `dstorage_xs_abi.h`, `dstorage_test.cpp`) |
| ThatRecompDev/five-star-recomp `ff2a5dd95094fdceb5efdca9fd3236b7fa97076e`, GPL-3.0-or-later | Slot facts cited by the native review; no code copied |
| californiaAlmonds/OpenScarlett `0cbc742aa607bb19e3a580bfb81442cfcd99e39c`, MIT | Interface name and slot facts cited by the native review; no code copied |
| GDK DStorage reference and overview on Microsoft Learn; PC `dstorage.h`/`dstorageerr.h` 1.3.0 | Names, signatures, structures, error values (as recorded by the PR); facts only |

Xbox file signature reference: [Microsoft IDStorageFileX::GetHandle](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/interfaces/idstoragefilex/methods/idstoragefilex_gethandle?view=gdk-2510).
