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

### A retail Xbox Series game: in game, its world not visible yet, not playable

Measured with Palmwave `b3bef8b` on an RTX 5080 (driver 621.14), a Ryzen 9 9950X3D and 61 GB of RAM, offline, with the shader caches of earlier runs, A pressed at 30 s and 40 s, `XCL_PATCH` (below), `XCL_DSTORAGE_BCPACK_FILL=neutral`, `XCL_GPU_WAIT_WATCHDOG_MS=3000`, `XCL_PSO_PRELOAD_THREADS=2` and `XCL_DXC_THREADS=6`.

| Time | What happens |
|---|---|
| 0–4 s | The title boots and presents its own frames after about 4 s. |
| 5–30 s | Its first screens, at 30 fps. |
| 32–58 s | The intro video, at 30 fps. |
| 60–84 s | The world loads behind its loading screens, at 5–30 fps. The compute pipelines of the world are built ahead of their use on background threads, so the title no longer stops to build them. |
| from 84 s | **In game.** The title runs in its world and its sound plays. Its first world frame records about 7,000 draws and 7,000 compute dispatches. It presents its world frames, but nothing of the world is visible yet: the image is the title's own uniform background colour, and one world frame takes from 18 s to 2 minutes of GPU work. |

A first launch is much slower, because the world's compute programs are compiled then. A few of them are giants of 12–25 MB of DXIL that take the NVIDIA driver up to about 30 minutes each. Everything built is kept on disk, both the DXIL and the driver's result.

**Byte patches for the title.** `XCL_PATCH` writes byte patches into an image as it is loaded, each only where the bytes it expects are found, so another version of the file is left alone. For version 1.0.2.1 of the title's executable:

- The first three patches turn off its hang detector, which ends the title after 30 s without a heartbeat from its main thread.
- The next two turn off its indirect-diffuse and stochastic-reflection passes, whose compute programs are the largest (up to about 88,000 instructions).
- The last one sets its render-resolution mode, which gives world targets of 2560×1440.

```powershell
$g = "game_scarlett_master.exe"
$env:XCL_PATCH = "$g+0x2215bdc:732e=9090;$g+0x2215c70:7404=9090;$g+0x2215d84:7404=9090;$g+0x22a3b5:e8a6f7ffff=31c0909090;$g+0x22aee6:e875ecffff=31c0909090;$g+0x7b4e6a:8b8944010000=b90100000090"
```

What still separates it from playable, most urgent first:

1. **GPU time of a world frame.**
   - The title's 4.4 GB of GPU heaps are in host memory, which the GPU reads across PCIe. One of its world compute passes takes 431 ms there, against 0.45 ms with the same memory in video memory.
   - `XCL_HEAPS_VRAM_MB=64` moves the heaps of 64 MB or more to video memory, but the title then needs about 20 GB of the 16 GB card.
   - GPU waits between the title's queues complete only when `XCL_GPU_WAIT_WATCHDOG_MS` releases them.
   - When GPU work runs too long, Windows resets the GPU and the title stops on "device removed".
2. **Nothing of the world is visible.** In the first world frames its render targets keep their cleared values (`XCL_RT_DUMP=1` writes them as images).
3. **Video memory.** 15–17 GB on a 16 GB card. The title's render targets, kept on 64 KiB pages of video memory, take 9.8 GB of it.
4. **BCPack textures.** BC1, BC3, BC4, BC5, BC6H and BC7 streams are decoded; this has not been run on the title yet. BC2 is refused. With `XCL_DSTORAGE_BCPACK_FILL=neutral`, a request that is refused or whose stream the decoder does not take completes with neutral grey blocks instead of failing.

Tools used for these measurements:
- `XCL_GPU_PROFILE=1` times each draw and dispatch on the GPU.
- `XCL_FRAME_DUMP_FROM=n` writes every presented frame from frame n.
- `XCL_RT_DUMP=1` writes the frame's render targets and depth.

Sound: the title's Opus sound is decoded. It has not been checked by ear on the test machine yet.

Shader compilation does not block the start. Before it loads its world, the title creates 58,759 pipelines and uses 12 or 13 of them, which are built at first use. On a second launch the DXIL cache cuts that build time from 2.9 s to 0.2 s. The window title's "pipelines queued" counts the pipelines that were created and never used.

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
