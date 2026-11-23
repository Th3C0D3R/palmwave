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
  <img src="https://img.shields.io/badge/diagnostic-53_compute_pipelines_created-08bdb5" alt="Opt-in diagnostic creates 53 compute pipelines">
  <img src="https://img.shields.io/badge/game_frame-not_observed-f57962" alt="No rendered game frame observed">
  <img src="https://img.shields.io/badge/backend-Vulkan-08bdb5" alt="Vulkan backend">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0-blue" alt="GPL-3.0 license"></a>
</p>

Palmwave is an experimental open-source Xbox Series compatibility layer and shader translation toolchain for PC, developed under **[Moon AI](https://realmoon.ai), the AI platform created by Stellarr Studio**. Its first target is a verified frame rendered by an Xbox Series executable on Windows.

**The tested Xbox Series executable starts on PC. With opt-in diagnostics enabled, it successfully creates 53 compute pipelines and 17 root signatures, and creates and closes 585 command lists.** It also imports a real 174 MiB buffer, writes a RAW buffer view, passes descriptor-heap initialization and creates two 4K images backed by its registered allocator's pages.

These are measured startup milestones on **NVIDIA RTX 5080**. No game `Draw`, `ExecuteCommandLists`, `Present`, menu or rendered frame has been observed; a stable launch is not yet established.

## Measured startup progress

Recorded on **24 November 2026**, with the signed48 shader-address contract and JSON loader updates: Windows 11, Ryzen 9 9950X3D, 64 GB RAM, **RTX 5080 16 GB / driver 621.14**. The results below describe local startup with explicitly enabled experiments, including a direct run inside a network-isolated AppContainer.

The executable maps and initializes, including four successful DLL initializations. It creates five queues and five fences; console command-list creation reaches **585 lists created and closed** through device slot 73, four more than the preceding capture. Closing a list does not establish GPU submission.

| Device slot | Measured progress | Scope and remaining limit |
|---|---|---|
| **26** | Heap-properties calls pass the console return-value convention. | The earlier access violation is cleared. |
| **133** | A real **174 MiB** caller-owned buffer is imported into Vulkan. | Requires the host-import experiment; import alone does not prove shader access. |
| **14** | The descriptor heap is created and the heap-address assertion is passed. | Subsequent SRV/UAV/CBV calls are observed; this does not mean every requested view is usable. |
| **70** | One address-based RAW buffer view is written successfully. | Two other views are still refused; follow-up tracing identifies typed R16_UINT views outside the supported RAW shape. This slot is not fully passed. |
| **25** | Allocation information returns successfully with the console argument order. | Size/alignment queries progress to resource creation. |
| **127** | Two **3840 × 2160** images are created using the actual pages returned by allocation callbacks. | The allocation-accounting assertion is cleared for the exact opt-in texture tuple. Unknown flags remain refused by default. |

The direct run records **25 startup events** and **2,341 D3D12 trace events**: 2,294 `ok`, 30 `called`, 14 `experimental-hypothesis`, one `experimental` and two `unsupported`. The 53 compute-pipeline and 17 root-signature results are successful returns, not just calls entered. Trace totals also depend on instrumentation coverage; their growth is not a progress metric. See the [latest startup result](STATUS.md#compute-pipeline-creation-and-the-next-startup-stop) and [allocation-backed image evidence](STATUS.md#committed-images-use-the-registered-allocators-real-pages).

## Current stop and run configurations

The direct run stopped on a guest breakpoint after pipeline creation. Follow-up tracing identified the third indirect command signature as the first call returning an error. The slot 41 adapter now accepts its measured 12-byte compute argument (type 105). On the RTX 5080 the next probe created four signatures and stopped after the fifth, a mesh-dispatch signature, returned unsupported. The two R16_UINT buffer views also remain unsupported. See the [parent fixes under review](docs/peer_compatibility.md).

| Configuration | Observed reach |
|---|---|
| **Default, last measured** | Graphics device, queues, fences, allocator and first console command list; recording stops at unimplemented list methods 132 and 131. |
| **Opt-in diagnostic** | 53 compute pipelines and 17 root signatures created successfully; startup later stops on a guest breakpoint. Follow-up tracing identifies the failed command-signature request above. |

The diagnostic combines `PALMWAVE_EXPERIMENTAL_XBOX_FLUSH`, `PALMWAVE_EXPERIMENTAL_XBOX_HOST_IMPORT`, `PALMWAVE_EXPERIMENTAL_XBOX_COMMITTED_TEXTURE` and `PALMWAVE_EXPERIMENTAL_XBOX_TEXTURE_HOOK_BACKING`, each set to `1`. These options test bounded compatibility hypotheses; they do not establish the general console contract. Their evidence and limits are recorded in [STATUS.md](STATUS.md) and [the blockers](docs/BLOCKERS.md).

## Shader preparation and independent GPU checks

| Check | Measured result |
|---|---|
| Package check | 108 / 108 files present; executable found |
| Main shader corpus | 26,503 / 26,503 extracted, unpacked, disassembled, translated and SPIR-V valid |
| Embedded BVH programs | 52 / 52 translated and SPIR-V valid; a bounded signed48 compute-pipeline cache build also passed 52 / 52 with no failures or skips in 32.3 s, producing 5,453,962 bytes |
| Embedded FSPX programs | 29 / 29 translated and SPIR-V valid; GPU semantics not yet verified |
| Bulk driver pipeline creation | 18,500 completed with zero failures/skips before the run was stopped for memory pressure; no full-pass claim |
| Presentation bridge | 25 targeted CTests passed; the RTX 5080 bridge test presented six synthetic frames, verified five distinct images by readback and checked 17 refusal cases, with zero Vulkan/synchronization-validation errors |
| Allocation-backed images | Three focused CTests passed; the RTX 5080 GPU test verified CPU/RTV/UAV/SRV access to the original callback pages and resource retirement, with zero Vulkan/synchronization-validation errors |

The signed48 contract supports imported RAW-buffer addresses on either side of the shader arena; the translated index must carry its matching marker. The JSON loader now moves parsed trees without deep copies. These updates and the bounded BVH cache were present in the new startup configuration; no single change is credited with causing the new result.

SPIR-V validation checks shader structure, not rendering correctness. The presentation test uses handcrafted images in **Palmwave Game Launcher**; the Xbox executable has not called the presenter. These checks are documented separately from executable progress in [STATUS.md](STATUS.md). Historical measurements in inherited documentation remain attributed to their original authors and hardware.

## What it does

- Loads x86-64 executable code on Windows and supplies compatibility shims for console APIs.
- Translates precompiled RDNA2 shader programs into SPIR-V for Vulkan.
- Implements the console-shaped D3D12 interfaces over a Vulkan device.
- Captures startup and graphics traces so the next blocking call can be measured and fixed.

All game input stays on the user's machine. This repository contains **no game package, game executable, generated game shaders, keys or decryption tools**. Online Xbox Live and Rockstar services are out of scope; guest execution requires network isolation.

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
