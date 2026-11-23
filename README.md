<p align="center">
  <img src="assets/palmwave-logo.png" alt="Palmwave: a palm and a wave forming the letter P" width="360">
</p>

<h1 align="center">Palmwave</h1>
<p align="center"><strong>From console code to a PC frame.</strong></p>
<p align="center">Xbox Series compatibility research · Windows x64 · Vulkan · RDNA2 → SPIR-V</p>
<p align="center">
  <a href="https://realmoon.ai">Moon AI</a> ·
  <a href="STATUS.md">Measured status</a> ·
  <a href="docs/boot.md">Boot runbook</a> ·
  <a href="docs/testing.md">Testing</a>
</p>
<p align="center">
  <img src="https://img.shields.io/badge/boot-graphics_device_reached-08bdb5" alt="Retail startup reaches a graphics device">
  <img src="https://img.shields.io/badge/status-first_frame_unverified-f57962" alt="First frame unverified">
  <img src="https://img.shields.io/badge/backend-Vulkan-08bdb5" alt="Vulkan backend">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0-blue" alt="GPL-3.0 license"></a>
</p>

Palmwave is an experimental open-source Xbox Series compatibility layer and shader translation toolchain for PC, developed under **[Moon AI](https://realmoon.ai), the AI platform created by Stellarr Studio**. Its first target is a **verified frame rendered by an Xbox Series executable on Windows**.

**The tested Xbox Series executable starts on PC and reaches Vulkan graphics initialization on NVIDIA RTX 5080. It then stops during startup. No menu or rendered game frame has been observed.**

**Local compatibility milestone: graphics device reached.** The default run creates five queues, five fences, an allocator and the first Xbox command list, then stops at unimplemented list methods 132 and 131. With the explicitly opt-in synchronization and host-memory experiments, the run reaches **581 command lists created and closed, a real 174 MiB buffer import and the first descriptor heap**. It now passes that heap-address assertion and reaches SRV/UAV/CBV calls, including Xbox device slot 70. Imported RAW views now use checked Vulkan device addresses; the latest run refuses a negative relative address that still needs shader support. Allocation information at slot 25 now succeeds. A separate opt-in committed-texture experiment now creates two real 4K images backed by pages returned through the registered allocation callbacks. The run continues to root-signature and compute-pipeline creation calls, then stops in a secondary thread; the earlier allocation-accounting assertion is no longer observed. The default texture path still refuses unknown console flags. The Vulkan presentation bridge now opens Palmwave Game Launcher and passes synthetic image tests; the executable has not reached a presentation call. This experimental result does not establish a stable launch or a first frame. Measurements and limitations are recorded in [STATUS.md](STATUS.md); a successful presentation call alone would not establish a rendered image.

## What it does

- Loads x86-64 executable code on Windows and supplies compatibility shims for console APIs.
- Translates precompiled RDNA2 shader programs into SPIR-V for Vulkan.
- Implements the console-shaped D3D12 interfaces over a Vulkan device.
- Captures startup and graphics traces so the next blocking call can be measured and fixed.

All game input stays on the user's machine. This repository contains **no game package, game executable, generated game shaders, keys or decryption tools**. Online Xbox Live and Rockstar services are out of scope; guest execution requires network isolation.

## Current local milestone

Measured on **Ryzen 9 9950X3D / NVIDIA RTX 5080 16 GB / driver 621.14 / Windows 11**, 23 November 2026:

| Check | Result |
|---|---|
| Package file check | 108 / 108 files present; executable found |
| Shader extraction | 26,503 shaders extracted |
| Native shader unpacking | 26,503 FSSM blobs; no unpacking problems |
| LLVM disassembly | 26,503 / 26,503 clean |
| Windows runtime build | 684 / 684 build steps completed |
| Main-corpus SPIR-V validation | 26,503 / 26,503 passed |
| Embedded BVH shaders | 52 / 52 translated and SPIR-V valid |
| Embedded FSPX shaders | 29 / 29 translated and SPIR-V valid with the native decoder; GPU semantics not yet verified |
| GPU pipeline creation | 18,500 completed with zero failures/skips; bulk build stopped to release memory; full-pass result pending |
| Retail dry-run / initialization | Both exit 0; four DLL initializations succeed |
| Retail execution | Vulkan device, five queues, five fences, an allocator and the first Xbox command list created on RTX 5080; recording stops at methods 132 and 131 |
| Opt-in startup diagnostics | 581 lists created and closed; real 174 MiB buffer imported; heap-address assertion passed; SRV/UAV/CBV calls reached; imported RAW views refuse unsupported negative relative addresses; allocation-info succeeds; a separate texture experiment backs two 4K images with callback pages and reaches pipeline creation before a secondary-thread stop; experimental ABI hypotheses |
| First rendered frame | Not verified |

These counts are preparation results, not gameplay or rendering results. Historical measurements in the inherited `docs/` remain attributed to their original authors and hardware.

## Build a local copy

Use your own readable Xbox Series package. Prerequisites include Python 3.12+, Visual Studio C++ x64 tools, Windows SDK 10.0.26100+, Vulkan SDK and LLVM 21+ with AMDGPU `llvm-mc`. See [the build and test notes](docs/testing.md). The .NET 8 SDK is needed for the optional asset exporter.

Keep an authoring checkout separate from a disposable build checkout. The inherited runtime helper writes into the build checkout's `runtime/build`; do not point it at your clean authoring tree if all generated outputs must stay outside that tree.

```powershell
git clone --recursive https://github.com/fablinoo/palmwave C:\palmwave-work\source
cd C:\palmwave-work\source
$env:VULKAN_SDK = 'C:\VulkanSDK\YOUR_VERSION'
$env:PATH = "$env:VULKAN_SDK\Bin;$env:PATH"
$env:LLVM_MC = 'C:\tools\llvm-mc\llvm-mc.exe'

python -I make_game.py --game 'D:\Games\YOUR_READABLE_PACKAGE' --out C:\palmwave-run --work C:\palmwave-work --stages check,shaders,runtime,pipelines --wave64-emulate on --jobs 2
```

This omits the large `assets` stage. Re-run the same command to resume. Wave64 emulation is required for the RTX 5080; cross-half wave operations in vertex, pixel and geometry shaders still have known approximations.

## First-frame workflow

Follow [docs/boot.md](docs/boot.md): `--dry-run`, then `--init`, then `--run`, with the Vulkan backend, startup JSONL and D3D12 stderr traces. **Isolate the game process and its children from the network before running guest code.** Keep traces, the working directory, saves and `--overlay` outside both the game package and the authoring repository.

Triage mapping, the first unimplemented import, TLS/DllMain, exceptions, exit status, then the first unsupported or failed graphics operation. Fix one observed cause at a time; use `PALMWAVE_UNIMPLEMENTED=stop` on the next diagnostic run. Do not skip shaders or draws to manufacture a progress claim.

## Development

Read [AGENTS.md](AGENTS.md), [CONTRIBUTING.md](CONTRIBUTING.md), [docs/design.md](docs/design.md) and the boot runbook. Changes should include a focused test and the hardware/driver used for any GPU result. Public reports contain counts and error classes, never proprietary traces or shader listings.

Project discussions and issues: [fablinoo/palmwave](https://github.com/fablinoo/palmwave/issues). Upstream contributions remain coordinated with the original project. New work uses small, reviewable commits.

## License

**Project:** [Moon AI](https://realmoon.ai), created by **Stellarr Studio**. **Maintainer:** [fablinoo](https://github.com/fablinoo).

Source is distributed under [GPL-3.0](LICENSE), with third-party components under their stated licenses. Palmwave is an independent community project and is not affiliated with Microsoft or game publishers.
