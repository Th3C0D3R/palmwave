//! What the memory windows and XCL's per-draw command patterns cost on this machine's GPU, for
//! choosing between them (ignored: `cargo test -p xcl --lib -- --ignored bench_ --nocapture
//! --test-threads=1`, on an idle GPU – another process's GPU work lands in the timestamps).
//!
//! * `bench_window_memory`: one memory window's loads, by the memory behind it (video memory;
//!   the coherent window's host memory as `OpenExistingHeapFromAddress` imports it; host memory
//!   of a custom heap, write-combined or write-back; a GPU upload heap – video memory the CPU
//!   maps over the resizable BAR) and by whether the view is `globallycoherent`
//!   (`emit::needs_coherent_memory`); what the CPU pays to write and read each mapping; whether
//!   tiles of a reserved buffer (a sparse window) can be memory of a GPU upload heap. (The 64
//!   MiB `stream` reads stay in the 64 MB L2 of an RTX 5080 between runs for video memory, the
//!   write-combined host memory and the GPU upload heap – L2 bandwidth, not memory's – and do
//!   not for the coherent window's or the write-back host memory: PCIe.)
//! * `bench_translated_scalar_loads`: the same through the translator – a console compute
//!   program of dependent scalar loads (a descriptor or constant walk), plain (the cached window
//!   it now gets) and with GLC (the `globallycoherent` one every compute program had).
//! * `bench_two_pass_commands`: the commands of a two-pass mesh draw (`d3d12x::bind`): a compute
//!   pass writing records, a UAV barrier, a draw reading them, a UAV barrier – direct or through
//!   ExecuteIndirect with a count of 1, with one barrier a draw (records alternating between two
//!   regions), or batched (every compute pass, one barrier, every draw).

use std::ffi::c_void;
use std::mem::{transmute_copy, ManuallyDrop};
use std::time::Instant;

use windows::core::Interface;
use windows::Win32::Graphics::Direct3D::*;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::*;
use windows::Win32::Graphics::Dxgi::*;
use windows_sys::Win32::System::Memory::{VirtualAlloc, VirtualFree, MEM_COMMIT, MEM_RELEASE, MEM_RESERVE, PAGE_READWRITE};

const MIB: usize = 1 << 20;
/// bytes of each window in `bench_window_memory`
const WINDOW: usize = 64 * MIB;

fn device() -> Option<(ID3D12Device, String)> {
    unsafe {
        let factory: IDXGIFactory6 = CreateDXGIFactory2(DXGI_CREATE_FACTORY_FLAGS(0)).ok()?;
        let sel = crate::gpu::adapter::Selector::from_env_or("auto");
        if sel == crate::gpu::adapter::Selector::Warp {
            return None;
        }
        let (a, desc) = crate::gpu::adapter::candidates(&factory, &sel).into_iter().next()?;
        let dev = crate::gpu::agility::create_device(&a, D3D_FEATURE_LEVEL_12_0).ok()?;
        Some((dev, crate::gpu::adapter::name(&desc)))
    }
}

fn buffer_desc(bytes: u64, flags: D3D12_RESOURCE_FLAGS) -> D3D12_RESOURCE_DESC {
    D3D12_RESOURCE_DESC {
        Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
        Width: bytes,
        Height: 1,
        DepthOrArraySize: 1,
        MipLevels: 1,
        SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
        Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        Flags: flags,
        ..Default::default()
    }
}

unsafe fn committed(dev: &ID3D12Device, props: D3D12_HEAP_PROPERTIES, bytes: u64, flags: D3D12_RESOURCE_FLAGS, state: D3D12_RESOURCE_STATES) -> windows::core::Result<ID3D12Resource> {
    let mut r: Option<ID3D12Resource> = None;
    dev.CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &buffer_desc(bytes, flags), state, None, &mut r)?;
    Ok(r.unwrap())
}

fn heap_type(t: D3D12_HEAP_TYPE) -> D3D12_HEAP_PROPERTIES {
    D3D12_HEAP_PROPERTIES { Type: t, ..Default::default() }
}

fn custom(page: D3D12_CPU_PAGE_PROPERTY, pool: D3D12_MEMORY_POOL) -> D3D12_HEAP_PROPERTIES {
    D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_CUSTOM, CPUPageProperty: page, MemoryPoolPreference: pool, CreationNodeMask: 0, VisibleNodeMask: 0 }
}

fn uav_barrier(r: &ID3D12Resource) -> D3D12_RESOURCE_BARRIER {
    D3D12_RESOURCE_BARRIER {
        Type: D3D12_RESOURCE_BARRIER_TYPE_UAV,
        Flags: D3D12_RESOURCE_BARRIER_FLAG_NONE,
        Anonymous: D3D12_RESOURCE_BARRIER_0 { UAV: ManuallyDrop::new(D3D12_RESOURCE_UAV_BARRIER { pResource: unsafe { transmute_copy(r) } }) },
    }
}

fn transition(r: &ID3D12Resource, before: D3D12_RESOURCE_STATES, after: D3D12_RESOURCE_STATES) -> D3D12_RESOURCE_BARRIER {
    D3D12_RESOURCE_BARRIER {
        Type: D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        Flags: D3D12_RESOURCE_BARRIER_FLAG_NONE,
        Anonymous: D3D12_RESOURCE_BARRIER_0 {
            Transition: ManuallyDrop::new(D3D12_RESOURCE_TRANSITION_BARRIER {
                pResource: unsafe { transmute_copy(r) },
                Subresource: D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                StateBefore: before,
                StateAfter: after,
            }),
        },
    }
}

/// A queue, an allocator and a list on `dev`, timestamps for `record`'s marks, and a wait.
struct Gpu {
    dev: ID3D12Device,
    queue: ID3D12CommandQueue,
    alloc: ID3D12CommandAllocator,
    list: ID3D12GraphicsCommandList,
    fence: ID3D12Fence,
    value: u64,
    stamps: ID3D12QueryHeap,
    readback: ID3D12Resource,
    hz: f64,
}

const STAMPS: u32 = 256;

impl Gpu {
    unsafe fn new(dev: &ID3D12Device) -> Gpu {
        let queue: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC::default()).unwrap();
        let alloc: ID3D12CommandAllocator = dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT).unwrap();
        let list: ID3D12GraphicsCommandList = dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &alloc, None).unwrap();
        list.Close().unwrap();
        let fence: ID3D12Fence = dev.CreateFence(0, D3D12_FENCE_FLAG_NONE).unwrap();
        let mut h: Option<ID3D12QueryHeap> = None;
        dev.CreateQueryHeap(&D3D12_QUERY_HEAP_DESC { Type: D3D12_QUERY_HEAP_TYPE_TIMESTAMP, Count: STAMPS, NodeMask: 0 }, &mut h).unwrap();
        let readback = committed(dev, heap_type(D3D12_HEAP_TYPE_READBACK), STAMPS as u64 * 8, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST).unwrap();
        let hz = queue.GetTimestampFrequency().unwrap() as f64;
        Gpu { dev: dev.clone(), queue, alloc, list, fence, value: 0, stamps: h.unwrap(), readback, hz }
    }

    /// Record with `f` (it marks with `mark(list, k)`), run, wait; the marks' times in ms from mark 0.
    unsafe fn run(&mut self, marks: u32, f: impl FnOnce(&ID3D12GraphicsCommandList, &dyn Fn(u32))) -> Vec<f64> {
        self.alloc.Reset().unwrap();
        self.list.Reset(&self.alloc, None).unwrap();
        let list = self.list.clone();
        let heap = self.stamps.clone();
        f(&list, &|k| list.EndQuery(&heap, D3D12_QUERY_TYPE_TIMESTAMP, k));
        list.ResolveQueryData(&self.stamps, D3D12_QUERY_TYPE_TIMESTAMP, 0, marks, &self.readback, 0);
        list.Close().unwrap();
        self.queue.ExecuteCommandLists(&[Some(list.cast().unwrap())]);
        self.value += 1;
        self.queue.Signal(&self.fence, self.value).unwrap();
        crate::gpu::fence::wait(&self.fence, self.value, &self.dev, None, 60_000).map_err(|e| format!("{e}")).unwrap();
        let mut p = std::ptr::null_mut();
        self.readback.Map(0, None, Some(&mut p)).unwrap();
        let t = std::slice::from_raw_parts(p as *const u64, marks as usize).to_vec();
        self.readback.Unmap(0, None);
        t.iter().map(|&x| x.saturating_sub(t[0]) as f64 * 1000.0 / self.hz).collect()
    }
}

/// The smallest of `n` runs of `f` (the GPU time of what it measures, in ms): with another
/// process on the GPU the smallest is the one least disturbed.
fn best(n: usize, mut f: impl FnMut() -> f64) -> f64 {
    (0..n).map(|_| f()).fold(f64::INFINITY, f64::min)
}

/// One window's loads: `chain` (each load's address from the one before, the same in every lane:
/// a descriptor or constant a wave waits for), with 1 group (latency) and with 4096 (many waves
/// at once); `stream` (each lane its own 16 bytes, every byte of the window once: bandwidth).
const BENCH_HLSL: &str = r#"
#define RS "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED), UAV(u0), RootConstants(num32BitConstants=4, b0)"
cbuffer C : register(b0) { uint window; uint iterations; uint mask; uint stride; };
RWByteAddressBuffer outb : register(u0);
[RootSignature(RS)]
[numthreads(64, 1, 1)]
void main(uint3 g : SV_GroupID, uint l : SV_GroupThreadID) {
    COHERENT RWByteAddressBuffer w = ResourceDescriptorHeap[NonUniformResourceIndex(window)];
    uint acc = 0u;
#ifdef CHAIN
    uint a = (g.x * 64u) & mask;
    for (uint i = 0u; i < iterations; ++i) {
        uint4 v = w.Load4(a);
        a = (v.x + 64u) & mask;
        acc += v.y;
    }
#else
    uint base = (g.x * 64u + l) * 16u;
    for (uint i = 0u; i < iterations; ++i) {
        uint4 v = w.Load4((base + i * stride) & mask);
        acc += v.x ^ v.w;
    }
#endif
    if (acc == 0x9E3779B9u) outb.Store(4u * l, acc);
}
"#;

/// The memory behind a window.
struct Placement {
    name: &'static str,
    res: ID3D12Resource,
    /// where the CPU sees it (null: it does not)
    cpu: *mut u8,
    /// VirtualAlloc'd here (freed at the end)
    owned: bool,
}

/// Every dword of the first `bytes` of `p` holds its own byte offset (the chain's next address).
unsafe fn identity(p: *mut u8, bytes: usize) {
    for k in 0..bytes / 4 {
        (p as *mut u32).add(k).write_volatile((4 * k) as u32);
    }
}

/// The CPU on a mapping: (GB/s writing 64 MiB, GB/s reading 16 MiB, ns a dependent 4-byte read).
unsafe fn cpu_costs(p: *mut u8) -> (f64, f64, f64) {
    let src = vec![0x5Au8; WINDOW];
    let t = Instant::now();
    std::ptr::copy_nonoverlapping(src.as_ptr(), p, WINDOW);
    let write = WINDOW as f64 / t.elapsed().as_secs_f64() / 1e9;
    let mut dst = vec![0u8; 16 * MIB];
    let t = Instant::now();
    std::ptr::copy_nonoverlapping(p, dst.as_mut_ptr(), 16 * MIB);
    let read = (16 * MIB) as f64 / t.elapsed().as_secs_f64() / 1e9;
    std::hint::black_box(&dst);
    identity(p, 16 * MIB);
    let (n, mut a) = (8192usize, 0usize);
    let t = Instant::now();
    for _ in 0..n {
        let v = (p.add(a) as *const u32).read_volatile() as usize;
        a = (v + 4096 + 64) & (16 * MIB - 1) & !3;
    }
    std::hint::black_box(a);
    (write, read, t.elapsed().as_secs_f64() * 1e9 / n as f64)
}

/// What a window's loads cost on this GPU by the memory behind it and its view, and what the
/// CPU pays on the memory it maps. Prints a table; asserts only that each form ran.
#[test]
#[ignore]
fn bench_window_memory() {
    let Some((dev, name)) = device() else {
        super::runner::skip_test("no GPU");
        return;
    };
    if super::dxc::location().is_err() {
        super::runner::skip_test("no DXC");
        return;
    }
    unsafe {
        let mut o16 = D3D12_FEATURE_DATA_D3D12_OPTIONS16::default();
        let o16_ok = dev.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &mut o16 as *mut _ as *mut c_void, std::mem::size_of_val(&o16) as u32).is_ok();
        let gpu_upload = o16_ok && o16.GPUUploadHeapSupported.as_bool();
        let mut arch = D3D12_FEATURE_DATA_ARCHITECTURE1::default();
        let _ = dev.CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &mut arch as *mut _ as *mut c_void, std::mem::size_of_val(&arch) as u32);
        println!("GPU: {name}; OPTIONS16 {}: GPUUploadHeapSupported {}; UMA {}, cache-coherent UMA {}", if o16_ok { "read" } else { "not read" }, gpu_upload, arch.UMA.as_bool(), arch.CacheCoherentUMA.as_bool());

        let uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        let mut places: Vec<Placement> = Vec::new();
        let vram = committed(&dev, heap_type(D3D12_HEAP_TYPE_DEFAULT), WINDOW as u64, uav, D3D12_RESOURCE_STATE_COMMON).unwrap();
        places.push(Placement { name: "video memory (DEFAULT heap)", res: vram, cpu: std::ptr::null_mut(), owned: false });
        // the coherent window's form (`d3d12x::real::open_uma`)
        let host = VirtualAlloc(std::ptr::null(), WINDOW, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) as *mut u8;
        assert!(!host.is_null());
        let dev3: ID3D12Device3 = dev.cast().unwrap();
        let heap: ID3D12Heap = dev3.OpenExistingHeapFromAddress(host as *const c_void).unwrap();
        let mut r: Option<ID3D12Resource> = None;
        dev.CreatePlacedResource(&heap, 0, &buffer_desc(WINDOW as u64, uav | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER), D3D12_RESOURCE_STATE_COMMON, None, &mut r).unwrap();
        places.push(Placement { name: "host memory, coherent window (OpenExistingHeapFromAddress)", res: r.unwrap(), cpu: host, owned: true });
        for (what, page) in [("host memory, write-combined (CUSTOM L0)", D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE), ("host memory, write-back (CUSTOM L0)", D3D12_CPU_PAGE_PROPERTY_WRITE_BACK)] {
            match committed(&dev, custom(page, D3D12_MEMORY_POOL_L0), WINDOW as u64, uav, D3D12_RESOURCE_STATE_COMMON) {
                Ok(res) => {
                    let mut p = std::ptr::null_mut();
                    res.Map(0, None, Some(&mut p)).unwrap();
                    places.push(Placement { name: what, res, cpu: p as *mut u8, owned: false });
                }
                Err(e) => println!("{what}: not made: {e}"),
            }
        }
        if gpu_upload {
            match committed(&dev, heap_type(D3D12_HEAP_TYPE_GPU_UPLOAD), WINDOW as u64, uav, D3D12_RESOURCE_STATE_COMMON) {
                Ok(res) => {
                    let mut p = std::ptr::null_mut();
                    res.Map(0, None, Some(&mut p)).unwrap();
                    places.push(Placement { name: "video memory mapped over the BAR (GPU_UPLOAD heap)", res, cpu: p as *mut u8, owned: false });
                }
                Err(e) => println!("GPU upload heap: not made: {e}"),
            }
        }
        // the chain's addresses: video memory gets them through a copy
        let upload = committed(&dev, heap_type(D3D12_HEAP_TYPE_UPLOAD), 64 << 10, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ).unwrap();
        let mut p = std::ptr::null_mut();
        upload.Map(0, None, Some(&mut p)).unwrap();
        identity(p as *mut u8, 64 << 10);
        for pl in places.iter().filter(|p| !p.cpu.is_null()) {
            identity(pl.cpu, 64 << 10);
        }
        let mut gpu = Gpu::new(&dev);
        let vram_res = places[0].res.clone();
        gpu.run(1, |list, mark| {
            mark(0);
            list.CopyBufferRegion(&vram_res, 0, &upload, 0, 64 << 10);
        });

        let dh: ID3D12DescriptorHeap = dev
            .CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC { Type: D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, NumDescriptors: places.len() as u32, Flags: D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, NodeMask: 0 })
            .unwrap();
        let inc = dev.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) as usize;
        for (k, pl) in places.iter().enumerate() {
            let mut h = dh.GetCPUDescriptorHandleForHeapStart();
            h.ptr += k * inc;
            dev.CreateUnorderedAccessView(&pl.res, None::<&ID3D12Resource>, Some(&super::runner::raw_window_uav(WINDOW as u64)), h);
        }
        let out = committed(&dev, heap_type(D3D12_HEAP_TYPE_DEFAULT), 4096, uav, D3D12_RESOURCE_STATE_COMMON).unwrap();
        // (pattern, coherent) → pipeline
        let mut rs: Option<ID3D12RootSignature> = None;
        let mut psos = Vec::new();
        for chain in [true, false] {
            for coherent in [false, true] {
                let src = BENCH_HLSL.replace("COHERENT ", if coherent { "globallycoherent " } else { "" });
                let src = if chain { format!("#define CHAIN 1\n{src}") } else { src };
                let dxil = super::dxc::compile(&src, "main", "cs_6_6", &[]).unwrap();
                let r = rs.get_or_insert_with(|| dev.CreateRootSignature(0, &dxil).unwrap()).clone();
                let pso: ID3D12PipelineState = dev
                    .CreateComputePipelineState(&D3D12_COMPUTE_PIPELINE_STATE_DESC {
                        pRootSignature: transmute_copy(&r),
                        CS: D3D12_SHADER_BYTECODE { pShaderBytecode: dxil.as_ptr() as _, BytecodeLength: dxil.len() },
                        ..Default::default()
                    })
                    .unwrap();
                psos.push(((chain, coherent), pso));
            }
        }
        let rs = rs.unwrap();
        // GPU time of one dispatch (ms), the least of 5
        let mut time = |pso: &ID3D12PipelineState, window: u32, groups: u32, consts: [u32; 4]| {
            best(5, || {
                let t = gpu.run(2, |list, mark| {
                    list.SetDescriptorHeaps(&[Some(dh.clone())]);
                    list.SetComputeRootSignature(&rs);
                    list.SetPipelineState(pso);
                    list.SetComputeRootUnorderedAccessView(0, out.GetGPUVirtualAddress());
                    let c = [window, consts[1], consts[2], consts[3]];
                    list.SetComputeRoot32BitConstants(1, 4, c.as_ptr() as *const c_void, 0);
                    mark(0);
                    list.Dispatch(groups, 1, 1);
                    mark(1);
                });
                t[1]
            })
        };
        const CHAIN_STEPS: u32 = 2048;
        const MANY: u32 = 4096;
        const MANY_STEPS: u32 = 256;
        let stride = MANY * 64 * 16;
        println!("window loads: latency = one wave's dependent uniform Load4 (64 lines); many waves = {MANY} groups x {MANY_STEPS} dependent uniform loads, wave loads per µs; stream = {MANY} groups reading all 64 MiB coalesced");
        println!("{:<58} {:>13} {:>13} {:>13} {:>13} {:>11} {:>11}", "memory", "latency ns", "coh. lat. ns", "many /µs", "coh. many/µs", "stream GB/s", "coh. GB/s");
        for (k, pl) in places.iter().enumerate() {
            let pso = |chain: bool, coherent: bool| psos.iter().find(|(key, _)| *key == (chain, coherent)).map(|(_, p)| p.clone()).unwrap();
            let mut lat = |coherent| time(&pso(true, coherent), k as u32, 1, [0, CHAIN_STEPS, 4095, 0]) * 1e6 / CHAIN_STEPS as f64;
            let (l0, l1) = (lat(false), lat(true));
            let waves = (MANY * 2 * MANY_STEPS) as f64; // two 32-lane waves a group
            let mut many = |coherent| waves / (time(&pso(true, coherent), k as u32, MANY, [0, MANY_STEPS, 4095, 0]) * 1e3);
            let (m0, m1) = (many(false), many(true));
            let mut stream = |coherent| WINDOW as f64 / (time(&pso(false, coherent), k as u32, MANY, [0, 16, (WINDOW - 1) as u32, stride]) * 1e-3) / 1e9;
            let (s0, s1) = (stream(false), stream(true));
            println!("{:<58} {l0:>13.0} {l1:>13.0} {m0:>13.0} {m1:>13.0} {s0:>11.1} {s1:>11.1}", pl.name);
            assert!(l0 > 0.0 && l1 > 0.0 && s0 > 0.0 && s1 > 0.0, "{}: every form ran", pl.name);
        }
        let removed = dev.GetDeviceRemovedReason();
        assert!(removed.is_ok(), "device removed: {removed:?}");

        println!("CPU on each mapping: write 64 MiB (GB/s), read 16 MiB (GB/s), one dependent 4-byte read (ns)");
        for pl in places.iter().filter(|p| !p.cpu.is_null()) {
            let (w, r, ns) = cpu_costs(pl.cpu);
            println!("  {:<58} write {w:>6.2} GB/s   read {r:>7.3} GB/s   dependent read {ns:>7.0} ns", pl.name);
        }

        // a sparse window's tile as GPU upload heap memory: the CPU writes it through a placed
        // buffer on that heap, the GPU reads it through the reserved buffer
        if gpu_upload && std::env::var_os("XCL_BENCH_UPLOAD_TILES").is_some() {
            let tile = 0x1_0000u64;
            let mut h: Option<ID3D12Heap> = None;
            let made = dev.CreateHeap(
                &D3D12_HEAP_DESC { SizeInBytes: tile, Properties: heap_type(D3D12_HEAP_TYPE_GPU_UPLOAD), Alignment: 0, Flags: D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS },
                &mut h,
            );
            println!("reserved tiles on a GPU upload heap: CreateHeap {made:?}");
            if let (Ok(()), Some(h)) = (made, h) {
                let mut placed: Option<ID3D12Resource> = None;
                dev.CreatePlacedResource(&h, 0, &buffer_desc(tile, D3D12_RESOURCE_FLAG_NONE), D3D12_RESOURCE_STATE_COMMON, None, &mut placed).unwrap();
                let placed = placed.unwrap();
                let mut q = std::ptr::null_mut();
                placed.Map(0, None, Some(&mut q)).unwrap();
                for k in 0..64usize {
                    (q as *mut u32).add(k).write_volatile(0xC0DE_0000 | k as u32);
                }
                let mut win: Option<ID3D12Resource> = None;
                dev.CreateReservedResource(&buffer_desc(16 * tile, uav), D3D12_RESOURCE_STATE_COMMON, None, &mut win).unwrap();
                let win = win.unwrap();
                let coord = D3D12_TILED_RESOURCE_COORDINATE { X: 3, ..Default::default() };
                let region = D3D12_TILE_REGION_SIZE { NumTiles: 1, ..Default::default() };
                gpu.queue.UpdateTileMappings(&win, 1, Some(&coord), Some(&region), &h, 1, Some(&D3D12_TILE_RANGE_FLAG_NONE), Some(&0u32), Some(&1u32), D3D12_TILE_MAPPING_FLAG_NONE);
                let rb = committed(&dev, heap_type(D3D12_HEAP_TYPE_READBACK), 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST).unwrap();
                gpu.run(1, |list, mark| {
                    mark(0);
                    list.CopyBufferRegion(&rb, 0, &win, 3 * tile, 256);
                });
                let mut p = std::ptr::null_mut();
                rb.Map(0, None, Some(&mut p)).unwrap();
                let got = std::slice::from_raw_parts(p as *const u32, 64).to_vec();
                rb.Unmap(0, None);
                let same = got.iter().enumerate().all(|(k, &v)| v == 0xC0DE_0000 | k as u32);
                println!("  the GPU read through the reserved buffer {} the CPU wrote (first words {:x?}); device {:?}", if same { "what" } else { "NOT what" }, &got[..4], dev.GetDeviceRemovedReason());
            }
        }
        // the GPU's views of the host memory go before the memory
        let owned: Vec<*mut u8> = places.iter().filter(|p| p.owned).map(|p| p.cpu).collect();
        drop(places);
        drop(heap);
        for p in owned {
            VirtualFree(p as *mut c_void, 0, MEM_RELEASE);
        }
    }
}

/// A console compute program as the translator makes it (one window, `Options::mem_windows` 1),
/// timed with its window in video memory and in the coherent window's host memory: 256
/// dependent `s_load_dwordx4` a wave (each at the offset the one before read, + 64, in a 4 KiB
/// table), without GLC – a cached window – and with GLC – `globallycoherent`, the form every
/// compute program had. The sum goes to memory (and is checked).
#[test]
#[ignore]
fn bench_translated_scalar_loads() {
    use crate::gpu::rdna2::encode;
    let Some((dev, name)) = device() else {
        super::runner::skip_test("no GPU");
        return;
    };
    if super::dxc::location().is_err() {
        super::runner::skip_test("no DXC");
        return;
    }
    const STEPS: u32 = 256;
    let one = |n: &str, e: &str, f: &[(&str, u64)], l: Option<u32>| encode(n, e, f, l).unwrap_or_else(|x| panic!("{n}: {x}"));
    let program = |glc: u64| -> Vec<u8> {
        [
            one("S_MOV_B32", "ENC_SOP1", &[("SDST", 12), ("SSRC0", 128)], None),
            one("S_MOV_B32", "ENC_SOP1", &[("SDST", 13), ("SSRC0", 128)], None),
            one("S_MOVK_I32", "ENC_SOPK", &[("SDST", 14), ("SIMM16", STEPS as u64)], None),
            // loop: s[8:11] = table[s12]; s12 = (s8 + 64) & 4095; s13 += s9
            one("S_LOAD_DWORDX4", "ENC_SMEM", &[("SDATA", 8), ("SBASE", 0), ("OFFSET", 0), ("SOFFSET", 12), ("GLC", glc)], None),
            one("S_ADD_U32", "ENC_SOP2", &[("SDST", 12), ("SSRC0", 8), ("SSRC1", 192)], None),
            one("S_AND_B32", "ENC_SOP2", &[("SDST", 12), ("SSRC0", 12), ("SSRC1", 255)], Some(4095)),
            one("S_ADD_U32", "ENC_SOP2", &[("SDST", 13), ("SSRC0", 13), ("SSRC1", 9)], None),
            one("S_SUB_U32", "ENC_SOP2", &[("SDST", 14), ("SSRC0", 14), ("SSRC1", 129)], None),
            one("S_CMP_LG_U32", "ENC_SOPC", &[("SSRC0", 14), ("SSRC1", 128)], None),
            // back to the s_load: 8 + 4 + 8 + 4 + 4 + 4 bytes before this branch
            one("S_CBRANCH_SCC1", "ENC_SOPP", &[("SIMM16", 0xFFF7)], None),
            // out[tid] = s13 at s2
            one("V_MOV_B32", "ENC_VOP1", &[("VDST", 1), ("SRC0", 13)], None),
            one("V_LSHLREV_B32", "ENC_VOP2", &[("VDST", 6), ("SRC0", 130), ("VSRC1", 0)], None),
            one("V_ADD_NC_U32", "ENC_VOP2", &[("VDST", 6), ("SRC0", 2), ("VSRC1", 6)], None),
            one("V_MOV_B32", "ENC_VOP1", &[("VDST", 7), ("SRC0", 128)], None),
            one("GLOBAL_STORE_DWORD", "ENC_FLAT_GLBL", &[("ADDR", 6), ("DATA", 1), ("SADDR", 0x7D)], None),
            one("S_ENDPGM", "ENC_SOPP", &[], None),
        ]
        .concat()
    };
    unsafe {
        let rs = super::pso::abi_root_signature(&dev, false).unwrap();
        let cs = super::CsInterface { threads: [64, 1, 1], user_sgprs: 4, ..Default::default() };
        // (heap windows, GLC) → pipeline: one window as root UAV u2, or the runtime's form – four
        // windows from the descriptor heap (`Options::heap_windows`), here all the same buffer
        let mut forms = Vec::new();
        for (windows, glc) in [(1u32, 0u64), (1, 1), (4 | super::HEAP_WINDOWS, 0), (4 | super::HEAP_WINDOWS, 1)] {
            let o = super::Options { mode: super::Mode::Compute(cs.clone()), wave64: false, mem_windows: windows & 7, heap_windows: windows & super::HEAP_WINDOWS != 0, ..Default::default() };
            let t = super::translate(&program(glc), &o).unwrap();
            assert!(t.complete(), "{:?}", t.unsupported_why);
            assert_eq!(t.hlsl.contains("globallycoherent"), glc == 1);
            let dxil = super::dxc::compile(&t.hlsl, "main", "cs_6_6", &[]).unwrap_or_else(|e| panic!("{e}\n{}", t.hlsl));
            let pso: ID3D12PipelineState = dev
                .CreateComputePipelineState(&D3D12_COMPUTE_PIPELINE_STATE_DESC {
                    pRootSignature: transmute_copy(&rs),
                    CS: D3D12_SHADER_BYTECODE { pShaderBytecode: dxil.as_ptr() as _, BytecodeLength: dxil.len() },
                    ..Default::default()
                })
                .unwrap();
            forms.push((windows != 1, glc == 1, pso));
        }
        let uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const BYTES: usize = 1 << 20;
        // io: s[0:1] = 0 (the table), s2 = 64 KiB (the sums)
        let mut io = vec![0u32; crate::gpu::recomp::KERNEL_SGPRS];
        io[2] = 64 << 10;
        let io_buf = committed(&dev, heap_type(D3D12_HEAP_TYPE_UPLOAD), (io.len() * 4) as u64, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ).unwrap();
        let mut p = std::ptr::null_mut();
        io_buf.Map(0, None, Some(&mut p)).unwrap();
        std::ptr::copy_nonoverlapping(io.as_ptr(), p as *mut u32, io.len());
        let io_dev = committed(&dev, heap_type(D3D12_HEAP_TYPE_DEFAULT), (io.len() * 4) as u64, uav, D3D12_RESOURCE_STATE_COMMON).unwrap();
        let res = committed(&dev, heap_type(D3D12_HEAP_TYPE_DEFAULT), 4096, uav, D3D12_RESOURCE_STATE_COMMON).unwrap();
        let vram = committed(&dev, heap_type(D3D12_HEAP_TYPE_DEFAULT), BYTES as u64, uav, D3D12_RESOURCE_STATE_COMMON).unwrap();
        let host = VirtualAlloc(std::ptr::null(), BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) as *mut u8;
        assert!(!host.is_null());
        identity(host, 4096);
        let dev3: ID3D12Device3 = dev.cast().unwrap();
        let heap: ID3D12Heap = dev3.OpenExistingHeapFromAddress(host as *const c_void).unwrap();
        let mut r: Option<ID3D12Resource> = None;
        dev.CreatePlacedResource(&heap, 0, &buffer_desc(BYTES as u64, uav | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER), D3D12_RESOURCE_STATE_COMMON, None, &mut r).unwrap();
        let coherent_window = r.unwrap();
        let table = committed(&dev, heap_type(D3D12_HEAP_TYPE_UPLOAD), 4096, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ).unwrap();
        let mut q = std::ptr::null_mut();
        table.Map(0, None, Some(&mut q)).unwrap();
        identity(q as *mut u8, 4096);
        let mut gpu = Gpu::new(&dev);
        gpu.run(1, |list, mark| {
            mark(0);
            list.CopyBufferRegion(&io_dev, 0, &io_buf, 0, (io.len() * 4) as u64);
            list.CopyBufferRegion(&vram, 0, &table, 0, 4096);
        });
        // the sum a wave makes: the table's dwords 1, 17, 33, .. (offsets 4, 68, ..) for STEPS steps
        let want: u32 = (0..STEPS).map(|k| ((k * 64) % 4096) + 4).fold(0u32, u32::wrapping_add);
        let readback = committed(&dev, heap_type(D3D12_HEAP_TYPE_READBACK), 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST).unwrap();
        // the heap form's windows: descriptors MEM_HEAP_BASE.. of a heap for each memory (each a raw
        // view of the same buffer)
        let inc = dev.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) as usize;
        let heaps: Vec<ID3D12DescriptorHeap> = [&vram, &coherent_window]
            .into_iter()
            .map(|mem| {
                let dh: ID3D12DescriptorHeap = dev
                    .CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC { Type: D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, NumDescriptors: super::MEM_HEAP_BASE + 4, Flags: D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, NodeMask: 0 })
                    .unwrap();
                for k in 0..4usize {
                    let mut h = dh.GetCPUDescriptorHandleForHeapStart();
                    h.ptr += (super::MEM_HEAP_BASE as usize + k) * inc;
                    dev.CreateUnorderedAccessView(mem, None::<&ID3D12Resource>, Some(&super::runner::raw_window_uav(BYTES as u64)), h);
                }
                dh
            })
            .collect();
        println!("GPU: {name}; a translated compute program of {STEPS} dependent s_load_dwordx4 a wave, 64-thread groups; GPU ms, the least of 5");
        for (m, (what, mem)) in [("video memory", &vram), ("host memory, coherent window", &coherent_window)].into_iter().enumerate() {
            for groups in [1u32, 4096] {
                let mut row = Vec::new();
                for (heap_form, glc, pso) in &forms {
                    let ms = best(5, || {
                        let t = gpu.run(2, |list, mark| {
                            list.SetComputeRootSignature(&rs);
                            list.SetPipelineState(pso);
                            if *heap_form {
                                list.SetDescriptorHeaps(&[Some(heaps[m].clone())]);
                            }
                            list.SetComputeRootUnorderedAccessView(0, io_dev.GetGPUVirtualAddress());
                            list.SetComputeRootUnorderedAccessView(1, res.GetGPUVirtualAddress());
                            list.SetComputeRootUnorderedAccessView(2, mem.GetGPUVirtualAddress());
                            mark(0);
                            list.Dispatch(groups, 1, 1);
                            mark(1);
                            list.ResourceBarrier(&[transition(mem, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)]);
                            list.CopyBufferRegion(&readback, 0, mem, 64 << 10, 256);
                        });
                        t[1]
                    });
                    let mut s = std::ptr::null_mut();
                    readback.Map(0, None, Some(&mut s)).unwrap();
                    let got = (s as *const u32).read();
                    readback.Unmap(0, None);
                    assert_eq!(got, want, "{what}, {groups} groups, heap form {heap_form}, glc {glc}: the program's sum");
                    row.push(format!("{} {}: {ms:>7.3} ms", if *heap_form { "heap windows" } else { "root window" }, if *glc { "GLC (coherent)" } else { "plain (cached)" }));
                }
                println!("  {what:<30} {groups:>4} groups   {}", row.join("   "));
            }
        }
        let removed = dev.GetDeviceRemovedReason();
        assert!(removed.is_ok(), "device removed: {removed:?}");
        drop(coherent_window);
        drop(heap);
        VirtualFree(host as *mut c_void, 0, MEM_RELEASE);
    }
}

/// The two passes of a two-pass mesh draw, as `d3d12x::bind` records them: a compute pass writing
/// records (here 64 dwords, in region `region` of the record buffer), a UAV barrier, a draw whose
/// vertex shader reads them (and stores what it read for draw `draw`), a UAV barrier.
const TWO_PASS_HLSL: &str = r#"
#define RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), UAV(u0), UAV(u1), RootConstants(num32BitConstants=2, b0)"
cbuffer C : register(b0) { uint region; uint draw; };
RWByteAddressBuffer rec : register(u0);
RWByteAddressBuffer outb : register(u1);
[RootSignature(RS)]
[numthreads(64, 1, 1)]
void cs(uint l : SV_GroupThreadID) { rec.Store(region * 256u + 4u * l, l + 64u * draw); }
[RootSignature(RS)]
float4 vs(uint v : SV_VertexID) : SV_Position {
    uint x = rec.Load(region * 256u + 4u * v);
    outb.Store(4u * (3u * draw + v), x);
    return float4(v == 1u ? 3.0f : -1.0f, v == 2u ? -3.0f : 1.0f, (x & 1u) * 0.25f, 1.0f);
}
[RootSignature(RS)]
float4 ps() : SV_Target { return float4(1.0f, 0.5f, 0.0f, 1.0f); }
"#;

/// What a two-pass draw's commands cost on the GPU (µs a draw) by how they are issued: per draw
/// compute pass + barrier + draw + barrier (now), the same through ExecuteIndirect with a count
/// of 1 (the title's mesh draws), without the barrier after the draw (records alternating
/// between two regions), all compute passes first and one barrier (batched), and the compute
/// passes or draws alone. Also the CPU time to record a draw. Every form with both passes is
/// checked: each draw's vertex shader read the records of its own compute pass.
#[test]
#[ignore]
fn bench_two_pass_commands() {
    let Some((dev, name)) = device() else {
        super::runner::skip_test("no GPU");
        return;
    };
    if super::dxc::location().is_err() {
        super::runner::skip_test("no DXC");
        return;
    }
    unsafe {
        let cs = super::dxc::compile(TWO_PASS_HLSL, "cs", "cs_6_6", &[]).unwrap();
        let vs = super::dxc::compile(TWO_PASS_HLSL, "vs", "vs_6_6", &[]).unwrap();
        let ps = super::dxc::compile(TWO_PASS_HLSL, "ps", "ps_6_6", &[]).unwrap();
        let rs: ID3D12RootSignature = dev.CreateRootSignature(0, &cs).unwrap();
        let cpso: ID3D12PipelineState = dev
            .CreateComputePipelineState(&D3D12_COMPUTE_PIPELINE_STATE_DESC {
                pRootSignature: transmute_copy(&rs),
                CS: D3D12_SHADER_BYTECODE { pShaderBytecode: cs.as_ptr() as _, BytecodeLength: cs.len() },
                ..Default::default()
            })
            .unwrap();
        let mut g = D3D12_GRAPHICS_PIPELINE_STATE_DESC {
            pRootSignature: transmute_copy(&rs),
            VS: D3D12_SHADER_BYTECODE { pShaderBytecode: vs.as_ptr() as _, BytecodeLength: vs.len() },
            PS: D3D12_SHADER_BYTECODE { pShaderBytecode: ps.as_ptr() as _, BytecodeLength: ps.len() },
            SampleMask: u32::MAX,
            PrimitiveTopologyType: D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
            NumRenderTargets: 1,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            ..Default::default()
        };
        g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        g.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL.0 as u8;
        g.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        let gpso: ID3D12PipelineState = dev.CreateGraphicsPipelineState(&g).unwrap();
        let rt_desc = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            Width: 64,
            Height: 64,
            DepthOrArraySize: 1,
            MipLevels: 1,
            Format: DXGI_FORMAT_R8G8B8A8_UNORM,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Layout: D3D12_TEXTURE_LAYOUT_UNKNOWN,
            Flags: D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
            ..Default::default()
        };
        let mut rt: Option<ID3D12Resource> = None;
        dev.CreateCommittedResource(&heap_type(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, None, &mut rt).unwrap();
        let rt = rt.unwrap();
        let rtv_heap: ID3D12DescriptorHeap = dev.CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC { Type: D3D12_DESCRIPTOR_HEAP_TYPE_RTV, NumDescriptors: 1, ..Default::default() }).unwrap();
        let rtv = rtv_heap.GetCPUDescriptorHandleForHeapStart();
        dev.CreateRenderTargetView(&rt, None, rtv);
        let uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const DRAWS: u32 = 2000;
        let rec = committed(&dev, heap_type(D3D12_HEAP_TYPE_DEFAULT), 1 << 20, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS).unwrap();
        let out_bytes = (DRAWS * 3 * 4) as u64;
        let out = committed(&dev, heap_type(D3D12_HEAP_TYPE_DEFAULT), out_bytes, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS).unwrap();
        let out_rb = committed(&dev, heap_type(D3D12_HEAP_TYPE_READBACK), out_bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST).unwrap();
        // indirect arguments: a dispatch (1, 1, 1) at 0, a draw (3 vertices, 1 instance) at 16
        let args = committed(&dev, heap_type(D3D12_HEAP_TYPE_UPLOAD), 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ).unwrap();
        let mut p = std::ptr::null_mut();
        args.Map(0, None, Some(&mut p)).unwrap();
        std::ptr::copy_nonoverlapping([1u32, 1, 1, 0, 3, 1, 0, 0].as_ptr(), p as *mut u32, 8);
        let signature = |ty: D3D12_INDIRECT_ARGUMENT_TYPE, stride: u32| -> ID3D12CommandSignature {
            let arg = D3D12_INDIRECT_ARGUMENT_DESC { Type: ty, ..Default::default() };
            let mut s: Option<ID3D12CommandSignature> = None;
            dev.CreateCommandSignature(&D3D12_COMMAND_SIGNATURE_DESC { ByteStride: stride, NumArgumentDescs: 1, pArgumentDescs: &arg, NodeMask: 0 }, None::<&ID3D12RootSignature>, &mut s).unwrap();
            s.unwrap()
        };
        let dispatch_sig = signature(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH, 12);
        let draw_sig = signature(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, 16);
        let mut gpu = Gpu::new(&dev);
        #[derive(Clone, Copy, PartialEq, Debug)]
        enum Form {
            /// compute, barrier, draw, barrier (`bind::dispatch_mesh` / `execute_indirect`)
            Now,
            /// the same through ExecuteIndirect, a count of 1 each
            Indirect,
            /// root signature and root views again before each pass (`bind_two_pass_stage`)
            Rebind,
            /// no barrier after the draw: the records alternate between two regions
            OneBarrier,
            /// every compute pass, one barrier, every draw
            Batched,
            /// compute passes alone, no barriers
            ComputeOnly,
            /// draws alone, no barriers
            DrawsOnly,
        }
        let record = |list: &ID3D12GraphicsCommandList, form: Form| {
            let vp = D3D12_VIEWPORT { TopLeftX: 0.0, TopLeftY: 0.0, Width: 64.0, Height: 64.0, MinDepth: 0.0, MaxDepth: 1.0 };
            let sc = windows::Win32::Foundation::RECT { left: 0, top: 0, right: 64, bottom: 64 };
            let bind_compute = |list: &ID3D12GraphicsCommandList| {
                list.SetComputeRootSignature(&rs);
                list.SetPipelineState(&cpso);
                list.SetComputeRootUnorderedAccessView(0, rec.GetGPUVirtualAddress());
                list.SetComputeRootUnorderedAccessView(1, out.GetGPUVirtualAddress());
            };
            let bind_graphics = |list: &ID3D12GraphicsCommandList| {
                list.SetGraphicsRootSignature(&rs);
                list.SetPipelineState(&gpso);
                list.SetGraphicsRootUnorderedAccessView(0, rec.GetGPUVirtualAddress());
                list.SetGraphicsRootUnorderedAccessView(1, out.GetGPUVirtualAddress());
            };
            list.OMSetRenderTargets(1, Some(&rtv), false, None);
            list.RSSetViewports(&[vp]);
            list.RSSetScissorRects(&[sc]);
            list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            bind_compute(list);
            bind_graphics(list);
            // draw k's records in region k % regions
            let compute = |list: &ID3D12GraphicsCommandList, k: u32, regions: u32, indirect: bool| {
                if form == Form::Rebind {
                    bind_compute(list);
                } else {
                    list.SetPipelineState(&cpso);
                }
                list.SetComputeRoot32BitConstants(2, 2, [k % regions, k].as_ptr() as *const c_void, 0);
                if indirect {
                    list.ExecuteIndirect(&dispatch_sig, 1, &args, 0, None::<&ID3D12Resource>, 0);
                } else {
                    list.Dispatch(1, 1, 1);
                }
            };
            let draw = |list: &ID3D12GraphicsCommandList, k: u32, regions: u32, indirect: bool| {
                if form == Form::Rebind {
                    bind_graphics(list);
                } else {
                    list.SetPipelineState(&gpso);
                }
                list.SetGraphicsRoot32BitConstants(2, 2, [k % regions, k].as_ptr() as *const c_void, 0);
                if indirect {
                    list.ExecuteIndirect(&draw_sig, 1, &args, 16, None::<&ID3D12Resource>, 0);
                } else {
                    list.DrawInstanced(3, 1, 0, 0);
                }
            };
            match form {
                Form::Now | Form::Indirect | Form::Rebind => {
                    for k in 0..DRAWS {
                        compute(list, k, 1, form == Form::Indirect);
                        list.ResourceBarrier(&[uav_barrier(&rec)]);
                        draw(list, k, 1, form == Form::Indirect);
                        list.ResourceBarrier(&[uav_barrier(&rec)]);
                    }
                }
                Form::OneBarrier => {
                    for k in 0..DRAWS {
                        compute(list, k, 2, false);
                        list.ResourceBarrier(&[uav_barrier(&rec)]);
                        draw(list, k, 2, false);
                    }
                }
                Form::Batched => {
                    for k in 0..DRAWS {
                        compute(list, k, DRAWS, false);
                    }
                    list.ResourceBarrier(&[uav_barrier(&rec)]);
                    for k in 0..DRAWS {
                        draw(list, k, DRAWS, false);
                    }
                }
                Form::ComputeOnly => (0..DRAWS).for_each(|k| compute(list, k, DRAWS, false)),
                Form::DrawsOnly => (0..DRAWS).for_each(|k| draw(list, k, DRAWS, false)),
            }
        };
        println!("GPU: {name}; {DRAWS} two-pass draws of 1 group + 3 vertices (64x64 target), GPU µs a draw (least of 5) and CPU µs to record one");
        for form in [Form::Now, Form::Indirect, Form::Rebind, Form::OneBarrier, Form::Batched, Form::ComputeOnly, Form::DrawsOnly] {
            let mut cpu = f64::INFINITY;
            let t = best(5, || {
                let t = gpu.run(2, |list, mark| {
                    mark(0);
                    let c = Instant::now();
                    record(list, form);
                    cpu = cpu.min(c.elapsed().as_secs_f64() * 1e6 / DRAWS as f64);
                    mark(1);
                    list.ResourceBarrier(&[transition(&out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)]);
                    list.CopyBufferRegion(&out_rb, 0, &out, 0, out_bytes);
                    list.ResourceBarrier(&[transition(&out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)]);
                });
                t[1]
            });
            println!("  {:<12} {:>8.2} µs GPU   {:>6.2} µs CPU", format!("{form:?}"), t * 1e3 / DRAWS as f64, cpu);
            assert!(t > 0.0);
            if !matches!(form, Form::ComputeOnly | Form::DrawsOnly) {
                let mut q = std::ptr::null_mut();
                out_rb.Map(0, None, Some(&mut q)).unwrap();
                let got = std::slice::from_raw_parts(q as *const u32, (DRAWS * 3) as usize).to_vec();
                out_rb.Unmap(0, None);
                for k in 0..DRAWS {
                    for v in 0..3 {
                        assert_eq!(got[(3 * k + v) as usize], v + 64 * k, "{form:?}: draw {k} vertex {v} read another draw's records");
                    }
                }
            }
        }
        let removed = dev.GetDeviceRemovedReason();
        assert!(removed.is_ok(), "device removed: {removed:?}");
    }
}
