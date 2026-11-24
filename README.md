<p align="center">
  <img src="assets/palmwave-logo.png" alt="Palmwave logo, a palm tree and a wave forming the letter P" width="360">
</p>

<h1 align="center">Palmwave</h1>
<p align="center"><strong>From console code to a PC frame.</strong></p>
<p align="center">Xbox Series X|S compatibility for Windows x64 · Direct3D 12 · RDNA2 → HLSL → DXIL</p>
<p align="center">
  <a href="https://realmoon.ai">Moon AI</a> ·
  <a href="#compatibility">Compatibility</a> ·
  <a href="#build-and-run">Build and run</a> ·
  <a href="https://github.com/fablinoo/palmwave/issues">Issues</a>
</p>

Palmwave is an Xbox Series X|S compatibility project from **Moon AI**, the AI platform created by **Stellarr Studio**. This edition is based on XCL. It runs a title’s x64 code natively on Windows, implements the console APIs that code calls, and translates RDNA2 shaders through HLSL and DXIL for Direct3D 12. The launcher, Cargo package and configuration variables use their upstream XCL names. [XCL source at the revision used here](https://github.com/ivall/xcl/tree/db1ddb25f3ff0c1686eed8200d238ad35f895926)

## Compatibility

At the pinned revision, XCL reports playable worlds in **Minecraft Preview** and playable **Unicorn Overlord**. For another Xbox Series executable, its maintainers report reaching the settings menu. Gameplay beyond those menus remains unverified. Palmwave's own runs on an RTX 5080 are below. [Upstream compatibility table](https://github.com/ivall/xcl/blob/db1ddb25f3ff0c1686eed8200d238ad35f895926/README.md#games)

### A retail Xbox Series game: it starts, not playable yet

Measured with Palmwave `00a7ca7` (the runtime code of the pinned XCL revision) on an RTX 5080 (driver 621.14), a Ryzen 9 9950X3D and 61 GB of RAM: offline, no input, two 180 s runs (with a measurement-only patch) and one 300 s run.

| Time | What happens |
|---|---|
| 0–4 s | The title boots and presents its own frames after about 4 s. |
| 5–30 s | Its first screens, at up to 30 fps. |
| 30–160 s | About 3 fps (6 or 7 frames every 2 s, 6 draws a frame). Upstream testers report the intro video here. |
| from 160 s | The world loads: about 23,000 more pipeline creations and DirectStorage reads. In the 300 s run the title showed almost no frame after 200 s (0 to 3 every 2 s), while local video memory rose from 6.8 GB to 22.5 GB and process commit from 16 GB to 42 GB. Longer runs crash in this phase. |

What still separates it from playable, most urgent first:

1. **World streaming memory.** Every texture the title places gets its own PC copy that is never released, so memory grows until the crash ([upstream issue #248](https://github.com/ivall/xcl/issues/248)). Upstream fixed it in [PR #288](https://github.com/ivall/xcl/pull/288); Palmwave does not have that fix yet.
2. **BCPack textures.** DirectStorage requests in some BCPack modes fail with `E_DECOMPRESSION`.
3. **Mesh pipelines.** 25,479 pipeline creations per run put the console's mesh shader in the amplification-shader slot and are refused, so whatever the title draws with them is missing.
4. **Frame rate.** The cause of the 3 fps phase is not known yet. It is not shader compilation: no pipeline is built during it.
5. **Sound.** The title decodes its sound through the console's Opus decoder. Upstream reports the game silent without [PR #294](https://github.com/ivall/xcl/pull/294), which Palmwave does not have yet.

Shader compilation does not block the start. The title creates 58,759 pipelines in 180 s and uses 12 or 13 of them, which are built at first use. On a second launch the DXIL cache cuts that build time from 2.9 s to 0.2 s. The window title's "pipelines queued" counts the pipelines that were created and never used.

On a GPU with 32-lane waves, including the RTX 5080, the recompiler implements wave64 compute and mesh operations with two cooperating halves. Vertex and pixel shaders that read lanes 32–63 still have accuracy limits. [Wave64 implementation notes](docs/wave64-on-wave32.md)

## Build and run

Use Windows 10/11 x64, stable Rust with the MSVC toolchain, Visual Studio C++ build tools, the Windows SDK with DXC, and Windows Gaming Services. The graphics path targets Direct3D 12, Shader Model 6.6 and mesh shaders. Windows 10 may also require the D3D12 Agility SDK. [DXC and Agility SDK setup](https://github.com/ivall/xcl/blob/db1ddb25f3ff0c1686eed8200d238ad35f895926/README.md#building-and-usage)

```powershell
git clone https://github.com/fablinoo/palmwave.git
cd palmwave
cargo build --release --locked --workspace
.\target\release\xcl.exe --version
```

To inspect an executable or launch your own unpacked title in offline mode, replace the example paths with your local paths. Opening `xcl.exe` from Explorer also brings up the title picker.

```powershell
$env:XCL_NETWORK = "0"
.\target\release\xcl.exe inspect "D:\Xbox\Title\title.exe"
.\target\release\xcl.exe run "D:\Xbox\Title"
```

The runtime directs title writes to an overlay. Each run records GPU activity, unsupported calls and crash information in `%LOCALAPPDATA%\XCL\<title>\logs\report.json`. For a rendering report, include the GPU, driver, commit and what appeared on screen. A present count alone cannot tell a black frame from a menu.

## Development

Run the workspace tests with `.\scripts\test.ps1`. The script reports GPU tests skipped on the selected adapter. The repository includes a synthetic title for end-to-end checks. [Test title](tests/testtitle) · [Contributor guide](CONTRIBUTING.md) · [Runtime design](docs/specs/2026-11-20-xcl-design.md)

## License and credits

The runtime comes from **ivall/xcl** and its contributors and is distributed under **GPL-2.0-only**. Third-party notices accompany the source. The Palmwave logo retains the license under which it was previously published. [Runtime license](LICENSE) · [Third-party notices](THIRD_PARTY.md) · [Logo license](assets/LICENSE.palmwave-logo)

**AI used:** [realmoon.ai](https://realmoon.ai) (Moon AI)
