<p align="center">
  <img src="assets/palmwave-logo.png" alt="Palmwave logo, a palm tree and a wave forming the letter P" width="360">
</p>

<h1 align="center">Palmwave</h1>
<p align="center"><strong>From console code to a PC frame.</strong></p>
<p align="center">Xbox Series X|S games on Windows x64 · Direct3D 12 · RDNA2 → HLSL → DXIL</p>
<p align="center">
  <a href="https://realmoon.ai">Made with Moon AI</a> ·
  <a href="#status">Status</a> ·
  <a href="#build-and-run">Build and run</a> ·
  <a href="https://github.com/fablinoo/palmwave/issues">Issues</a>
</p>

Palmwave runs Xbox Series X|S games on Windows. It is made with **Moon AI**, the AI platform created by **Stellarr Studio**. Palmwave runs a title’s x64 code natively, implements the console APIs that code calls, and translates the console’s RDNA2 shaders through HLSL and DXIL for Direct3D 12. The launcher and its settings keep their current names (`xcl.exe`, `XCL_*` variables) until they are renamed.

## Status

### A retail Xbox Series game: it reaches world loading, not playable yet

Measured with Palmwave `0e0bbef` on an RTX 5080 (driver 621.14), a Ryzen 9 9950X3D and 61 GB of RAM, offline, with A pressed every 10 s from 30 s to pass the menus and `XCL_DSTORAGE_BCPACK_FILL=neutral`.

| Time | What happens |
|---|---|
| 0–4 s | The title boots and presents its own frames after about 4 s. |
| 5–30 s | Its first screens, at 30 fps. |
| 32–58 s | The intro video, at 30 fps. It ran at 3.5 fps while its video planes were copied a texel at a time and each texture upload waited on the CPU. |
| 60–80 s | The world loads behind its loading screens, at 5–30 fps. Process commit levels off at about 29 GB. A per-texture layout table took 9 GB more before. DirectStorage requests no longer fail. |
| from 82 s | The title stops presenting: its render jobs wait for pipelines that are still compiling. After 30 s without a frame, the title's own watchdog ends it, at about 110 s. |

What still separates it from playable, most urgent first:

1. **Pipeline waits while the world loads.** The title's render jobs wait for graphics and compute pipelines still being compiled. A draw now waits at most 4 s. Next: one budget for all of a frame's pipeline waits, and the title's compute shaders compiled ahead of time.
2. **Video memory.** About 15 GB of video memory on a 16 GB card, in page pools that never shrink. Next: a video-memory budget that spills to system memory.
3. **BCPack textures.** No BCPack mode is decoded yet. With `XCL_DSTORAGE_BCPACK_FILL=neutral`, the 768 such requests of a run complete with neutral grey blocks instead of failing.
4. **Mesh pipelines.** 25,479 pipeline creations per run put the console's mesh shader in the amplification-shader slot and are refused, so whatever the title draws with them is missing.

Sound: the title's Opus sound is now decoded. It has not been checked by ear on the test machine yet.

Shader compilation does not block the start. The title creates 58,759 pipelines in 180 s and uses 12 or 13 of them, which are built at first use. On a second launch the DXIL cache cuts that build time from 2.9 s to 0.2 s. The window title's "pipelines queued" counts the pipelines that were created and never used.

On a GPU with 32-lane waves, including the RTX 5080, the recompiler implements wave64 compute and mesh operations with two cooperating halves. Vertex and pixel shaders that read lanes 32–63 still have accuracy limits. [Wave64 implementation notes](docs/wave64-on-wave32.md)

## Build and run

Use Windows 10/11 x64, stable Rust with the MSVC toolchain, Visual Studio C++ build tools, the Windows SDK with DXC, and Windows Gaming Services. The graphics path targets Direct3D 12, Shader Model 6.6 and mesh shaders. Windows 10 may also need the D3D12 Agility SDK.

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

## License

Palmwave is distributed under **GPL-2.0-only**. It is derived from the XCL project by ivall and its contributors, whose copyright it keeps. Third-party notices accompany the source. The Palmwave logo keeps the license under which it was previously published. [License](LICENSE) · [Third-party notices](THIRD_PARTY.md) · [Logo license](assets/LICENSE.palmwave-logo)

**AI used:** [realmoon.ai](https://realmoon.ai) (Moon AI)
