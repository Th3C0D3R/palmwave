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

<p align="center">
  <img alt="Stage: in game" src="https://img.shields.io/badge/stage-in%20game-1f883d?style=for-the-badge">
  <img alt="World: drawn" src="https://img.shields.io/badge/world-drawn-1f883d?style=for-the-badge">
  <img alt="Lighting: in progress" src="https://img.shields.io/badge/lighting-in%20progress-bf8700?style=for-the-badge">
  <img alt="Speed: 0.2 to 0.5 fps" src="https://img.shields.io/badge/speed-0.2%E2%80%930.5%20fps-bf8700?style=for-the-badge">
  <img alt="Playable: not yet" src="https://img.shields.io/badge/playable-not%20yet-6e7781?style=for-the-badge">
</p>

### A retail Xbox Series game: on the road to playable

The title runs in its world: its first mission starts and its world is drawn. The street, the ground, palm trees, traffic lights, people and a car reach the title's G-buffer, and `XCL_SHOW_TARGET=lit` shows them in the window under one fixed light of Palmwave's. The picture the title presents itself is still almost one dark colour, because its own lighting does not work yet. A world frame takes 2–5 s, and the newest run's GPU faulted after about four minutes. Next come stability, the title's lighting and the frame time.

```mermaid
flowchart LR
  boot["Boots<br/>4 s"] --> menus["First screens<br/>and intro video<br/>30 fps"] --> load["World loads<br/>no crash"] --> ingame["In game<br/>first mission"] --> geo["World geometry<br/>drawn"] --> light["Lighting"] --> speed["30 fps"] --> play["Playable"]
  classDef done fill:#1f883d,stroke:#1a7f37,color:#ffffff
  classDef wip fill:#bf8700,stroke:#9a6700,color:#ffffff
  classDef todo fill:#6e7781,stroke:#57606a,color:#ffffff
  class boot,menus,load,ingame,geo done
  class light,speed wip
  class play todo
```

| | Milestone | Where it stands |
|---|---|---|
| ✅ | Boot | The title boots and presents its own frames after about 4 s, its first screen after 10–13 s. |
| ✅ | First screens and intro video | At 30 fps. Its Opus sound is decoded (not checked by ear yet). |
| ✅ | World loading | The first mission's world loads without a crash. |
| ✅ | In game | The first mission starts: its opening card and the card's subtitle, then its world frames. |
| ✅ | World geometry | Drawn: the street, the ground, palm trees, traffic lights, people and a car reach the depth buffer and the G-buffer, and `XCL_SHOW_TARGET=lit` shows them in the window under one fixed light (not the title's lighting). Some vegetation comes out flat green and some textures black. |
| ✅ | No crash in the world | With a 3 s GPU-wait watchdog, a run stayed in the world 3 minutes and presented frames until it was stopped, without a crash or a device removal. With a longer watchdog, runs with `6e68cf2` stopped presenting after a few world frames while the GPU stayed busy; with `9aafbcc` no GPU wait needed the watchdog, and world frames kept coming until the runs were stopped. |
| 🔄 | Stability | With `16bae47` the newest run reached its world at about 100 s and stopped at 245 s on a GPU fault (a read at address 0, "device removed"); the cause is being looked for. Builds from `52687c1` to `fb0b6a6` stopped at 6 s, before the intro video, on a lock cycle between Palmwave and D3D12, fixed in `c1fa050` and `0ec0c55`. |
| 🔄 | Lighting | The picture the title presents is still almost one dark colour. The lighting targets stay almost black: with `9aafbcc`, only small lit areas show (about 1.5 % of a lighting target). Why is not known yet. |
| 🔄 | Frame time | 2–5 s per world frame (0.2–0.5 fps) with `16bae47`, against 33 ms for 30 fps; the GPU is 92 % busy and the title uses 21 GB of video memory against a budget of 14.6 GB. Most of it is GPU time, with the GPU 99–100 % busy; the title's 4.4 GB of GPU heaps, which the GPU reads across PCIe, come first. |
| ⏳ | Controls in the world | Not tested yet. |
| ⏳ | Playable at 30 fps | Not yet. |

The newest figures (world drawn, frame time, stability) were measured with `16bae47` on the same machine, with the shader caches of earlier runs, `XCL_DEPTH_UAV_TWINS=1`, `XCL_SHOW_TARGET=lit` and `XCL_DRED=1`. Older figures: Measured with Palmwave `6e68cf2` (branch `world-loading`, not merged into `main` yet) on an RTX 5080 (driver 621.14), a Ryzen 9 9950X3D and 61 GB of RAM, offline, with the shader caches of earlier runs, A pressed at 30 s and 40 s, `XCL_PATCH` (below), `XCL_DSTORAGE_BCPACK_FILL=neutral`, `XCL_GPU_WAIT_WATCHDOG_MS=3000` (60000 and 600000 in the other runs), `XCL_PSO_PRELOAD_THREADS=4` and `XCL_DXC_THREADS=8`, with the frame, render-target and census dumps on (`XCL_FRAME_DUMP_FROM`, `XCL_RT_DUMP=1`, `XCL_FRAME_CENSUS_FROM`), which add about 0.5 s to each world frame. Newer runs with `9aafbcc` (same branch) are noted where they differ.

<details>
<summary><strong>A run, second by second</strong></summary>

| Time | What happens |
|---|---|
| 0–4 s | The title boots and presents its own frames after about 4 s. |
| 5–30 s | Black frames until 10–13 s, then its first screens, at 30 fps. |
| 32–42 s | The intro video, at 30 fps. |
| 43–58 s | A logo, then the first loading screen, at 30 fps. |
| 60–120 s | The world loads behind its loading screens, at 0–16 fps, then no frame for about 23 s while the first world frame is built. The compute pipelines of the world are built ahead of their use on background threads. |
| from about 120 s | **In game, first mission.** The title starts its first mission: it presents the mission's opening card (its text over the title's background colour, with cinematic bars), then the mission's world frames; its Opus sound is decoded (not checked by ear yet). Its first world frame records about 7,900 draws and 7,500 compute dispatches. **Part of the world's geometry is drawn:** its depth buffer and G-buffer hold utility poles, power lines, fences, an awning and bike racks (`XCL_RT_DUMP=1`); about 91 % of the depth buffer stays empty, without the ground or the rest of the street. The presented picture is still one flat colour, because the world is not lit yet (below). A world frame takes 9–20 s. With the 3 s watchdog the run stayed in the world 3 minutes, until it was stopped, without a crash or a device removal. |

A first launch is much slower, because the world's compute programs are compiled then. Its three largest compute programs, of the indirect-diffuse passes that the byte patches turn off, took the NVIDIA driver up to about 30 minutes each at 12–25 MB of DXIL; they are 5.5–5.9 MB now, and their build time has not been measured since. The DXIL is kept on disk, and so is the driver's build of every compute pipeline that took 250 ms or more.

</details>

<details>
<summary><strong>Byte patches for the title</strong></summary>

`XCL_PATCH` writes byte patches into an image as it is loaded, each only where the bytes it expects are found, so another version of the file is left alone. For version 1.0.2.1 of the title's executable:

- The first three patches turn off its hang detector, which ends the title after 30 s without a heartbeat from its main thread.
- The next two turn off its indirect-diffuse and stochastic-reflection passes, whose compute programs are the largest (up to about 88,000 instructions).
- The next one sets its render-resolution mode, which gives world targets of 2560×1440.
- The next three turn off three more passes that are slow here: an alpha-resolve blur, the scattering applied in the deferred combine, and a ray-marched gather.
- The last one avoids a stop after about two minutes, where the title's audio code keeps a lock held on one path.

```powershell
$g = "game_scarlett_master.exe"
$env:XCL_PATCH = "$g+0x2215bdc:732e=9090;$g+0x2215c70:7404=9090;$g+0x2215d84:7404=9090;$g+0x22a3b5:e8a6f7ffff=31c0909090;$g+0x22aee6:e875ecffff=31c0909090;$g+0x7b4e6a:8b8944010000=b90100000090;$g+0x25d2fe2:e819a4ffff=0f1f440000;$g+0x327b92e:7408=9090;$g+0x125912d:e85ef50102=0f1f440000;$g+0x5267747:0f84ae050000=0f8432050000"
```

</details>

<details>
<summary><strong>What still separates it from playable, most urgent first</strong></summary>

1. **GPU time of a world frame.**
   - The title's 4.4 GB of GPU heaps are in host memory, which the GPU reads across PCIe. One of its world compute passes takes 431 ms there, against 0.45 ms with the same memory in video memory.
   - `XCL_HEAPS_VRAM_MB=64` moves the heaps of 64 MB or more to video memory, which the title already overfills (item 3); with the page budget the switch also turns on, this is not measured yet.
   - With that switch, the heap memory under the title's render targets takes no video memory of its own (`XCL_SPARSE_UNDER_TARGETS=0` turns this off, `=1` turns it on without the switch). Its sampled textures of 32 KiB or less are packed in heaps of their own instead of taking a 64 KiB page each (`XCL_SMALL_TEXTURE_HEAPS`, same values). What the two save on the title is not measured yet.
   - With `6e68cf2`, some GPU waits between the title's queues did not complete on their own and `XCL_GPU_WAIT_WATCHDOG_MS` released them, about 3 s of most world frames with a 3 s watchdog. With `9aafbcc` none needs it.
   - The title's queues are created without Windows' GPU timeout, so long GPU work no longer resets the GPU; with `XCL_GPU_TIMEOUT=1` it does, and the title stops on "device removed".
2. **The title's own lighting.** Its geometry now reaches the G-buffer and the window can show it lit by one fixed light (`XCL_SHOW_TARGET=lit`), but the picture the title presents stays almost one dark colour: its lighting targets are still nearly black, and why is not known yet.
   - The title keeps its streamed data in a 16–24 GB "GPU view" of its memory, whose pages it maps with queue method `CopyPageMappingsBatchX`. Textures, buffers and `CopyMemoryToMemoryX` copies there now read the pages mapped under them; before, buffers there read nothing or another memory window's bytes. Whether the geometry is right with it is not measured yet. `XCL_GPU_VIEW_BUFFERS=0` turns the buffer part off; `XCL_GPU_VIEW_BUFFERS_MB=n` is the address space it may take (4096). With `XCL_GPU_VIEW_WRITEBACK=1`, what the GPU writes into such a buffer is copied back to the title's pages before the title's fence signal after that work; it is off by default, since it copies whole 64 KiB pages and can overwrite the title's data next to a buffer (one crash after four minutes in the world) ( `XCL_GPU_VIEW_WRITEBACK_MB=n` is the most one submission copies back, 64).
3. **Video memory.** 15–21 GB on a 16 GB card (21.3 GB in the world in the newest run, against a budget of 14.6 GB). The title's textures on 64 KiB pages of video memory (render targets and streamed sampled textures) take about 10 GB of it.
4. **BCPack textures.** BC1, BC3, BC4, BC5, BC6H and BC7 streams are decoded; the title's BC1, BC5, BC6H and BC7 requests unpack in its runs. BC2 is refused. With `XCL_DSTORAGE_BCPACK_FILL=neutral`, a request that is refused or whose stream the decoder does not take completes with neutral grey blocks instead of failing.

Sampled textures in a GPU view read each page from its own backing, including textures with only some pages mapped directly. A view by address can also reactivate a cached texture after an overlapping placement, remapping and uploading its tiles again. Synthetic regressions cover both paths; their effect on the retail title's flat materials is not measured yet.

</details>

<details>
<summary><strong>Diagnostic tools used for these measurements</strong></summary>

- `XCL_GPU_PROFILE=1` times each draw and dispatch on the GPU.
- `XCL_FRAME_DUMP_FROM=n` writes every presented frame from frame n.
- `XCL_RT_DUMP=1` writes the frame's render targets and depth.
- `XCL_TRACE_FRAME=1 XCL_TRACE_COMPUTE_FROM=n` logs compute bindings from present count n, including indirect dispatches, once per pipeline. `XCL_TRACE_MANY=m` caps the number of captures (12); `XCL_TRACE_BUFFER_WORDS=w` sets the CPU words read from each root buffer (64, at most 4096). The log includes CS hashes, root arguments and inferred descriptor candidates with their T#/V# words and tracked resources. These are CPU snapshots and candidate bindings, not GPU light-list readbacks or proof of shader reads. `XCL_TRACE_SHADERS=hash,...` selects CS hashes and permits repeated captures of them.
- `XCL_SHOW_TARGET=albedo`, `lit`, `depth` or a number shows in the window, in place of the title's picture, what its G-buffer pass drew: the albedo, the albedo under one fixed light with the pass's normals, the depth, or the frame's n-th target as `XCL_RT_DUMP` numbers them. It is a debug view, not the title's lighting.
- `XCL_FRAME_CENSUS=n` lists frame n's draws and dispatches (pipeline, targets, viewport, depth state, the counts the GPU ran of each ExecuteIndirect) in the log and in `census_<n>.csv`; `XCL_FRAME_CENSUS_FROM=n` does so for frames n to n+2. With `XCL_FRAME_CENSUS_MIN_OPS=m`, frames with fewer than m operations are passed over and the first three from n that have m are kept (the world's first frame is not the same from run to run).

</details>

<details>
<summary><strong>More notes: sound, shader compilation, wave64</strong></summary>

Sound: xapu decodes the title's Opus packets into its buffers. The title mixes them in software and sends its mix through native Windows Spatial Audio on the default endpoint (the path established in upstream [#294](https://github.com/ivall/xcl/pull/294) and [#300](https://github.com/ivall/xcl/pull/300)). XAudio2 voices also use the Windows output. SHAPE hardware mixing remains unimplemented; its allocation message does not mean this software mix is silent. Sound has not been checked by ear on the test machine yet.

`XCL_AUDIO_PEAK=1` logs sampled Windows output peaks every 10 s, independently of rendering: the maximum of this process's session meters across active render devices, and the default device's meter (which includes other applications). It also reports the default device's volume/mute and whether sessions use another device. These are sampled meters, before endpoint volume, not an exact maximum over every submitted sample or proof that speakers are audible. Missing sessions, unavailable meters and errors are reported separately from silence. Compare non-zero process peaks with the xapu decode counters while listening to the intro, menus and world; no extra decoder-playback voice is needed.

Stencil shader views preserve unsigned integer reads and map a console `R8_UINT` view to the PC stencil channel. The change includes synthetic regression tests; its effect on the title's dark lighting has not been measured.

Shader compilation does not block the start. Before it loads its world, the title creates 58,759 pipelines and uses 12 or 13 of them, which are built at first use. On a second launch the DXIL cache cuts that build time from 2.9 s to 0.2 s. The window title's "pipelines queued" counts the pipelines that were created and never used.

On a GPU with 32-lane waves, including the RTX 5080, the recompiler implements wave64 compute and mesh operations with two cooperating halves. Vertex and pixel shaders that read lanes 32–63 still have accuracy limits. [Wave64 implementation notes](docs/wave64-on-wave32.md)

With `XCL_HEAPS_VRAM_MB` set (or `XCL_VRAM_BUDGET=1`; `XCL_VRAM_BUDGET=0` turns it off), the 64 KiB pages of video memory that hold the title's textures keep to a budget: what is left of 90 % of the video memory Windows grants the process, or of `XCL_VRAM_BUDGET_MB=<MiB>` of local video memory. At the budget:

- textures the CPU fills that no view heap names give their pages back, and are mapped and uploaded again when the title uses them;
- block-compressed textures of 1024 texels and more are placed without their top mip, of 2048 and more without their top two, and their views are clamped to the mips they have (`XCL_VRAM_TRIM=0`: placed whole). They stay so until the title releases them;
- page heaps whose textures nothing used for 10 s get a low residency priority, so that Windows moves those out of video memory first;
- further pages are in system memory, which the GPU reads more slowly, and page heaps left without a texture are released.

`XCL_VRAM_POOL_MB=<MiB>` gives the page heaps a fixed size of video memory instead, with the last point only, and `XCL_PAGE_BUDGET_MB=<MiB>` caps the page heaps of both kinds by giving pages back alone. The `memory: page budget:` line of `XCL_ACTIVITY=1` says what was given back, trimmed and demoted. This is tested on an RTX 5080 with the unit tests only, not yet with the retail title.

</details>

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

`XCL_OVERLAY=1` draws performance statistics over the game image; F10 hides or shows them. It shows last-frame FPS/ms and the mean of frame intervals ending in the last second, CPU use (100% means all logical processors), process RAM commit, local video memory use/budget, and the busiest GPU engine on XCL's adapter. GPU timings cover XCL's frame copy and overlay pass, not the title's whole frame. Unsupported counters show `N/A`; the display updates when the title presents. Off by default.

`XCL_CONSOLE=1` opens or reuses a live, level-coloured log console and places it beside the game window where Windows allows. Its bounded queue drops and counts lines when output cannot keep up; file logging is unaffected. Off by default.

## Development

Run the workspace tests with `.\scripts\test.ps1`. The script reports GPU tests skipped on the selected adapter. The repository includes a synthetic title for end-to-end checks. [Test title](tests/testtitle) · [Contributor guide](CONTRIBUTING.md) · [Runtime design](docs/specs/2026-11-20-xcl-design.md)

## License

Palmwave is distributed under **GPL-2.0-only**. It is derived from the XCL project by ivall and its contributors, whose copyright it keeps. Third-party notices accompany the source. The Palmwave logo keeps the license under which it was previously published. [License](LICENSE) · [Third-party notices](THIRD_PARTY.md) · [Logo license](assets/LICENSE.palmwave-logo)

**AI used:** [realmoon.ai](https://realmoon.ai) (Moon AI)
