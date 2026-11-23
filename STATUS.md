# Measured status

Updated: 23 November 2026. Target: a first verified frame from the retail executable, not gameplay.

## Local preparation

Source revision: original upstream `dde82f27e994337094afc476ded0b1426f6e87e2`, matching the inherited runtime code in the initial Palmwave import. Runtime built with MSVC 19.51, Windows SDK 10.0.26100 and Vulkan SDK 1.4.363.0. Shader disassembler: LLVM 21.1.8 with AMDGPU. Host: Windows 11, Ryzen 9 9950X3D, 64 GB RAM, NVIDIA RTX 5080 16 GB, driver 621.14.

| Measurement | Result |
|---|---|
| Requested package check | Version 1.0.2.1, 108 files present, executable found |
| Carve | 26,503 shaders; one scanner candidate rejected |
| FSSM unpack | 26,503 blobs, 412 MiB total, no reported problems |
| Disassembly | 26,503 clean results |
| Runtime compilation | 684 build steps completed |
| Vulkan in the intended offline sandbox | Initialization and enumeration succeed; no rendering test implied |
| Full-corpus translation/validation | Running; final counts pending |
| Driver pipeline creation | Pending |
| Retail execution and frame | Pending; no rendered frame claimed |

The scanner candidate is not counted as a shader translation failure. Generated data and full logs are private and stored outside the authoring repository. No game assets have been added to Git.

## Isolation

The planned retail run uses a Windows AppContainer token with no Internet or private-network capability, no loopback exemption, and inherited confinement for ordinary child processes. Overlay, saves, cache writes and startup/D3D12 traces use a writable temporary directory. This is the local runner's measured configuration, not a network guarantee supplied by palmwave itself.

## Reporting rules

Record the code revision, mode, exit code, event counts, first failure class and GPU/driver. Inspect the actual presented image before claiming a first rendered frame. Keep raw game traces, addresses paired with proprietary dumps, shader listings and screenshots containing game assets out of this repository.

Project: [Palmwave / Moon AI](https://realmoon.ai). Source provenance: [UPSTREAM.md](UPSTREAM.md).
