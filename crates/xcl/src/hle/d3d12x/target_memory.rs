//! Render targets of one texel, written to the memory they were placed at.
//!
//! On the console a render target is memory like any other, and a title may read what it drew
//! through the address it placed the target at: a retail game renders its exposure into a 1x1
//! R32G32B32A32_FLOAT target in the linear layout (XG layout 0x100), and its bright pass,
//! bilateral grid and final composite take the value with `s_buffer_load_dword` at offset 0 of a
//! buffer descriptor (the red channel) and scale the scene by it. Here a target placed at a title
//! address is a PC texture of its own (`device_x::placed_resource_x`, `vram`): the memory at the
//! address never held what was drawn, the exposure read 0, and every pixel of the presented
//! picture was the colour grade's black – one flat dark grey with the dithering noise on it.
//!
//! So a render target of one texel placed in unified memory gets a buffer over that memory when
//! it is made (`real::place_at_title_address`), and after each draw into it and each clear of it
//! its texel is copied there (`after_draw`, `after_clear`): out of RENDER_TARGET for the copy and
//! back, then an aliasing barrier – shaders read the memory through XCL's window buffers, other
//! resources over the same memory. One texel only: whatever a console texture's tiling, texel
//! (0, 0) of its first mip is at its address; a larger target would need the console's layout
//! (XG) to be written back. Draws through ExecuteIndirect or DispatchMesh are not followed.

use std::collections::HashMap;
use std::ffi::c_void;
use std::mem::ManuallyDrop;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::Mutex;

use windows::core::Interface;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::*;

/// PC command list slots: GetType, CopyTextureRegion, ResourceBarrier
const PC_GET_TYPE: usize = 8;
const PC_COPY_TEXTURE_REGION: usize = 16;
const PC_RESOURCE_BARRIER: usize = 26;

/// A one-texel target's way to its memory: a buffer over the memory, from the 64 KiB page the
/// target's address is in, and where that address is in the buffer.
struct Target {
    buffer: ID3D12Resource,
    offset: u64,
    format: DXGI_FORMAT,
}

/// target → the buffer its texel is copied to
static TARGETS: Mutex<Option<HashMap<usize, Target>>> = Mutex::new(None);
/// list → the one-texel targets it has bound
static BOUND: Mutex<Option<HashMap<usize, Vec<usize>>>> = Mutex::new(None);
/// targets in `TARGETS`, and lists in `BOUND` (0: nothing to look up)
static LIVE: AtomicUsize = AtomicUsize::new(0);
static BOUND_LISTS: AtomicUsize = AtomicUsize::new(0);

/// Bytes of the one texel of the render target `d` describes, in `console_format` on the
/// console (None: not a render target of one texel, or one whose PC texel is not the console's).
fn one_texel(d: &D3D12_RESOURCE_DESC, console_format: u32) -> Option<u32> {
    let one = matches!(d.Dimension, D3D12_RESOURCE_DIMENSION_TEXTURE1D | D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        && (d.Width, d.Height, d.DepthOrArraySize) == (1, 1, 1)
        && d.MipLevels <= 1
        && d.SampleDesc.Count <= 1;
    let target = d.Flags.0 & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET.0 != 0;
    if !one || !target || d.Format.0 as u32 != console_format {
        return None;
    }
    crate::gpu::desc::gfx10_format(d.Format).map(crate::gpu::desc::format_bytes)
}

/// A D3D12_RESOURCE_DESC (the bytes `real::place_at_title_address` takes) of a buffer of `bytes`.
fn buffer_desc(bytes: u32) -> [u8; 0x38] {
    let mut d = [0u8; 0x38];
    d[0..4].copy_from_slice(&(D3D12_RESOURCE_DIMENSION_BUFFER.0 as u32).to_le_bytes());
    d[0x10..0x18].copy_from_slice(&(bytes as u64).to_le_bytes());
    // height, depth or array size, mips, samples: 1; layout: row major
    d[0x18..0x1C].copy_from_slice(&1u32.to_le_bytes());
    d[0x1C..0x20].copy_from_slice(&(1u32 | 1 << 16).to_le_bytes());
    d[0x24..0x28].copy_from_slice(&1u32.to_le_bytes());
    d[0x2C..0x30].copy_from_slice(&(D3D12_TEXTURE_LAYOUT_ROW_MAJOR.0 as u32).to_le_bytes());
    d
}

/// The title placed `res`, a texture with the PC description `d` (`console_format` on the
/// console), at `va`: a render target of one texel there gets the buffer its copies go to.
pub unsafe fn note_target(device: *mut c_void, res: usize, va: usize, d: &D3D12_RESOURCE_DESC, console_format: u32) {
    let Some(bytes) = one_texel(d, console_format) else { return };
    if crate::gpu::uma::get().is_none_or(|u| !u.contains(va) || !u.contains(va + bytes as usize - 1)) {
        return;
    }
    static FAILED: AtomicU64 = AtomicU64::new(0);
    static N: AtomicU64 = AtomicU64::new(0);
    let failed = |why: &str| {
        if FAILED.fetch_add(1, Ordering::Relaxed) < 4 {
            crate::xlog_warn!("D3D12.X: a render target of one texel at {va:#x}: {why} – what is drawn into it does not reach that memory");
        }
    };
    let mut p: *mut c_void = std::ptr::null_mut();
    let iid = &ID3D12Resource::IID as *const windows::core::GUID as *const windows_sys::core::GUID;
    let placed = super::real::place_at_title_address(device, va, &buffer_desc(bytes), D3D12_RESOURCE_STATE_COPY_DEST.0 as u32, std::ptr::null(), iid, &mut p);
    if placed.is_none_or(|hr| hr < 0) || p.is_null() {
        return failed("no buffer over its memory");
    }
    let buffer = ID3D12Resource::from_raw(p);
    // where the target's address is in the buffer (it starts at the address's 64 KiB page); a
    // copy's footprint starts at a multiple of 512 bytes
    let offset = super::real::buffer_delta(p as usize);
    if offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT as u64 != 0 {
        return failed("not at a multiple of 512 bytes");
    }
    let old = TARGETS.lock().unwrap().get_or_insert_with(HashMap::new).insert(res, Target { buffer, offset, format: d.Format });
    if old.is_none() {
        LIVE.fetch_add(1, Ordering::Relaxed);
    }
    let n = N.fetch_add(1, Ordering::Relaxed) + 1;
    if n <= 8 || n.is_power_of_two() {
        crate::xlog_info!("D3D12.X: a render target of one texel at {va:#x} (format {}): copied to that memory after each draw into it ({n} so far)", d.Format.0);
    }
}

/// `res` is gone.
pub fn forget(res: usize) {
    if LIVE.load(Ordering::Relaxed) == 0 {
        return;
    }
    let gone = TARGETS.lock().unwrap().as_mut().and_then(|m| m.remove(&res));
    if gone.is_some() {
        LIVE.fetch_sub(1, Ordering::Relaxed);
    }
    drop(gone);
}

/// Which of `resources` (bound as render targets) are one-texel targets with a buffer.
fn one_texel_targets(resources: impl Iterator<Item = usize>) -> Vec<usize> {
    let g = TARGETS.lock().unwrap();
    let Some(m) = g.as_ref() else { return Vec::new() };
    resources.filter(|r| m.contains_key(r)).collect()
}

/// `list` has `targets` bound now (none: forgotten).
fn set_bound(list: usize, targets: Vec<usize>) {
    let mut g = BOUND.lock().unwrap();
    let m = g.get_or_insert_with(HashMap::new);
    let had = if targets.is_empty() { m.remove(&list).is_some() } else { m.insert(list, targets).is_some() };
    match (had, m.contains_key(&list)) {
        (false, true) => {
            BOUND_LISTS.fetch_add(1, Ordering::Relaxed);
        }
        (true, false) => {
            BOUND_LISTS.fetch_sub(1, Ordering::Relaxed);
        }
        _ => {}
    }
}

/// The one-texel targets `list` has bound.
fn bound(list: usize) -> Vec<usize> {
    if BOUND_LISTS.load(Ordering::Relaxed) == 0 {
        return Vec::new();
    }
    BOUND.lock().unwrap().as_ref().and_then(|m| m.get(&list).cloned()).unwrap_or_default()
}

/// OMSetRenderTargets(n, rtvs, single, …) on `list`.
pub unsafe fn note_bound(list: usize, n: u32, rtvs: *const usize, single: i32) {
    if LIVE.load(Ordering::Relaxed) == 0 && BOUND_LISTS.load(Ordering::Relaxed) == 0 {
        return;
    }
    let n = if rtvs.is_null() { 0 } else { n.min(D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) as usize };
    let handles = (0..n).map(|k| if single != 0 { *rtvs + k * 32 } else { *rtvs.add(k) });
    let resources: Vec<usize> = handles.filter_map(super::present::rtv_resource).collect();
    set_bound(list, one_texel_targets(resources.into_iter()));
}

/// `list` starts a new recording: nothing is bound.
pub fn list_reset(list: usize) {
    if BOUND_LISTS.load(Ordering::Relaxed) != 0 {
        set_bound(list, Vec::new());
    }
}

/// A draw was recorded on `list`: the one-texel targets it drew into go to their memory.
pub unsafe fn after_draw(list: *mut c_void) {
    for res in bound(list as usize) {
        copy_out(list, res);
    }
}

/// ClearRenderTargetView of `rtv` was recorded on `list`.
pub unsafe fn after_clear(list: *mut c_void, rtv: usize) {
    if LIVE.load(Ordering::Relaxed) == 0 {
        return;
    }
    if let Some(res) = super::present::rtv_resource(rtv) {
        copy_out(list, res);
    }
}

unsafe fn copy_out(list: *mut c_void, res: usize) {
    let Some((buffer, offset, format)) = TARGETS.lock().unwrap().as_ref().and_then(|m| m.get(&res)).map(|t| (t.buffer.clone(), t.offset, t.format)) else { return };
    let r = res as *mut c_void;
    let Some(target) = ID3D12Resource::from_raw_borrowed(&r) else { return };
    // (a bundle has no barriers and no copies)
    let list_type: unsafe extern "system" fn(*mut c_void) -> D3D12_COMMAND_LIST_TYPE = std::mem::transmute(super::real::orig_fn(list, PC_GET_TYPE));
    if list_type(list) != D3D12_COMMAND_LIST_TYPE_DIRECT {
        return;
    }
    type Barrier = unsafe extern "system" fn(*mut c_void, u32, *const D3D12_RESOURCE_BARRIER);
    type CopyRegion = unsafe extern "system" fn(*mut c_void, *const D3D12_TEXTURE_COPY_LOCATION, u32, u32, u32, *const D3D12_TEXTURE_COPY_LOCATION, *const D3D12_BOX);
    let barrier: Barrier = std::mem::transmute(super::real::orig_fn(list, PC_RESOURCE_BARRIER));
    let copy: CopyRegion = std::mem::transmute(super::real::orig_fn(list, PC_COPY_TEXTURE_REGION));
    record(target, &buffer, offset, format, &|b| barrier(list, b.len() as u32, b.as_ptr()), &|dst, src| copy(list, dst, 0, 0, 0, src, std::ptr::null()));
    static N: AtomicU64 = AtomicU64::new(0);
    let n = N.fetch_add(1, Ordering::Relaxed) + 1;
    if n <= 2 || n.is_power_of_two() {
        crate::xlog_info!("D3D12.X: a render target of one texel copied to its memory ({n} copies so far)");
    }
}

fn borrowed(res: &ID3D12Resource) -> ManuallyDrop<Option<ID3D12Resource>> {
    // the barrier / copy location holds no reference of its own
    unsafe { std::mem::transmute_copy(res) }
}

fn transition(res: &ID3D12Resource, before: D3D12_RESOURCE_STATES, after: D3D12_RESOURCE_STATES) -> D3D12_RESOURCE_BARRIER {
    D3D12_RESOURCE_BARRIER {
        Type: D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        Flags: D3D12_RESOURCE_BARRIER_FLAG_NONE,
        Anonymous: D3D12_RESOURCE_BARRIER_0 {
            Transition: ManuallyDrop::new(D3D12_RESOURCE_TRANSITION_BARRIER { pResource: borrowed(res), Subresource: D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, StateBefore: before, StateAfter: after }),
        },
    }
}

/// The copy of the texel of `target` – in RENDER_TARGET, as a draw or a clear leaves it – to
/// `offset` of `buffer` (in COPY_DEST, or COMMON to be promoted), with the barriers around it
/// (`barrier`, `copy`: the list's ResourceBarrier and CopyTextureRegion(destination, source)).
fn record(target: &ID3D12Resource, buffer: &ID3D12Resource, offset: u64, format: DXGI_FORMAT, barrier: &dyn Fn(&[D3D12_RESOURCE_BARRIER]), copy: &dyn Fn(*const D3D12_TEXTURE_COPY_LOCATION, *const D3D12_TEXTURE_COPY_LOCATION)) {
    let (rt, src) = (D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(&[transition(target, rt, src)]);
    let footprint = D3D12_PLACED_SUBRESOURCE_FOOTPRINT {
        Offset: offset,
        Footprint: D3D12_SUBRESOURCE_FOOTPRINT { Format: format, Width: 1, Height: 1, Depth: 1, RowPitch: D3D12_TEXTURE_DATA_PITCH_ALIGNMENT },
    };
    let to = D3D12_TEXTURE_COPY_LOCATION { pResource: borrowed(buffer), Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { PlacedFootprint: footprint } };
    let from = D3D12_TEXTURE_COPY_LOCATION { pResource: borrowed(target), Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: 0 } };
    copy(&to, &from);
    // what reads the memory next reads it through another resource over it
    let aliasing = D3D12_RESOURCE_BARRIER {
        Type: D3D12_RESOURCE_BARRIER_TYPE_ALIASING,
        Flags: D3D12_RESOURCE_BARRIER_FLAG_NONE,
        Anonymous: D3D12_RESOURCE_BARRIER_0 { Aliasing: ManuallyDrop::new(D3D12_RESOURCE_ALIASING_BARRIER { pResourceBefore: ManuallyDrop::new(None), pResourceAfter: ManuallyDrop::new(None) }) },
    };
    barrier(&[transition(target, src, rt), aliasing]);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn target_desc(w: u64, h: u32, format: DXGI_FORMAT, flags: D3D12_RESOURCE_FLAGS) -> D3D12_RESOURCE_DESC {
        D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            Width: w,
            Height: h,
            DepthOrArraySize: 1,
            MipLevels: 1,
            Format: format,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Layout: D3D12_TEXTURE_LAYOUT_UNKNOWN,
            Flags: flags,
            ..Default::default()
        }
    }

    /// The exposure target of a retail game – 1x1 R32G32B32A32_FLOAT, a render target – has a
    /// texel of 16 bytes; a larger target, an array, MSAA, a texture that is not a render target
    /// and one the PC makes in another format than the console's do not count.
    #[test]
    fn only_a_render_target_of_one_texel_counts() {
        let rt = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        let f = DXGI_FORMAT_R32G32B32A32_FLOAT;
        let exposure = target_desc(1, 1, f, rt);
        assert_eq!(one_texel(&exposure, f.0 as u32), Some(16));
        assert_eq!(one_texel(&target_desc(1, 1, DXGI_FORMAT_R32_UINT, rt | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), DXGI_FORMAT_R32_UINT.0 as u32), Some(4));
        assert_eq!(one_texel(&target_desc(2, 1, f, rt), f.0 as u32), None, "two texels");
        assert_eq!(one_texel(&target_desc(1, 2, f, rt), f.0 as u32), None, "two rows");
        assert_eq!(one_texel(&target_desc(1, 1, f, D3D12_RESOURCE_FLAG_NONE), f.0 as u32), None, "not a render target");
        assert_eq!(one_texel(&exposure, DXGI_FORMAT_R9G9B9E5_SHAREDEXP.0 as u32), None, "another format on the PC");
        let mut array = exposure;
        array.DepthOrArraySize = 2;
        assert_eq!(one_texel(&array, f.0 as u32), None, "two slices");
        let mut msaa = exposure;
        msaa.SampleDesc.Count = 4;
        assert_eq!(one_texel(&msaa, f.0 as u32), None, "four samples");
        let mut buffer = exposure;
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        assert_eq!(one_texel(&buffer, f.0 as u32), None, "a buffer");
    }

    /// The buffer a target's texel is copied to, as `real::place_at_title_address` reads it.
    #[test]
    fn the_buffer_over_a_targets_memory_is_a_row_major_buffer_of_its_texel() {
        let d: D3D12_RESOURCE_DESC = unsafe { std::ptr::read_unaligned(buffer_desc(16).as_ptr() as *const D3D12_RESOURCE_DESC) };
        assert_eq!((d.Dimension, d.Width, d.Height, d.DepthOrArraySize, d.MipLevels), (D3D12_RESOURCE_DIMENSION_BUFFER, 16, 1, 1, 1));
        assert_eq!((d.Format, d.SampleDesc.Count, d.SampleDesc.Quality, d.Layout, d.Flags), (DXGI_FORMAT_UNKNOWN, 1, 0, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE));
    }

    /// A list that binds a one-texel target copies it after its draws until it binds other
    /// targets or starts a new recording; with no such list a draw looks nothing up.
    #[test]
    fn a_list_copies_the_one_texel_targets_it_has_bound() {
        let (list, other) = (0x7a11_0001usize, 0x7a11_0002usize);
        let lists = BOUND_LISTS.load(Ordering::Relaxed);
        set_bound(list, vec![0x5000]);
        set_bound(other, vec![0x6000, 0x7000]);
        set_bound(list, vec![0x5000]);
        assert_eq!((bound(list), bound(other)), (vec![0x5000], vec![0x6000, 0x7000]));
        assert_eq!(BOUND_LISTS.load(Ordering::Relaxed), lists + 2, "a list bound twice counts once");
        // other targets bound on `list`: nothing to copy after its draws, `other` keeps its own
        set_bound(list, Vec::new());
        assert!(bound(list).is_empty());
        assert_eq!(bound(other), vec![0x6000, 0x7000]);
        // a new recording of `other`
        list_reset(other);
        assert!(bound(other).is_empty());
        assert_eq!(BOUND_LISTS.load(Ordering::Relaxed), lists, "no list left with one bound");
    }

    /// A 1x1 R32G32B32A32_FLOAT target drawn (here: cleared) twice, its texel copied after each
    /// to its place in memory: each copy lands at its offset with the value of that moment, and
    /// the target is back in RENDER_TARGET in between (the second clear needs it there).
    #[test]
    fn gpu_the_texel_of_a_one_texel_target_lands_at_its_offset() {
        use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
        unsafe {
            let mut dev: Option<ID3D12Device> = None;
            if D3D12CreateDevice(None, D3D_FEATURE_LEVEL_11_0, &mut dev).is_err() {
                crate::gpu::recomp::runner::skip_test("no GPU");
                return;
            }
            let dev = dev.unwrap();
            let f = DXGI_FORMAT_R32G32B32A32_FLOAT;
            let default = D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() };
            let mut target: Option<ID3D12Resource> = None;
            dev.CreateCommittedResource(&default, D3D12_HEAP_FLAG_NONE, &target_desc(1, 1, f, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_RESOURCE_STATE_RENDER_TARGET, None, &mut target).unwrap();
            let target = target.unwrap();
            let rtv_heap: ID3D12DescriptorHeap = dev.CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC { Type: D3D12_DESCRIPTOR_HEAP_TYPE_RTV, NumDescriptors: 1, ..Default::default() }).unwrap();
            let rtv = rtv_heap.GetCPUDescriptorHandleForHeapStart();
            dev.CreateRenderTargetView(&target, None, rtv);
            // "the memory": a readback buffer, its 64 KiB page with the target at 0xC000
            let (first, second) = (0xC000u64, 0xC200u64);
            let mut memory: Option<ID3D12Resource> = None;
            let readback = D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_READBACK, ..Default::default() };
            let mut memory_desc = target_desc(0x1_0000, 1, DXGI_FORMAT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE);
            (memory_desc.Dimension, memory_desc.Layout) = (D3D12_RESOURCE_DIMENSION_BUFFER, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
            dev.CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &memory_desc, D3D12_RESOURCE_STATE_COPY_DEST, None, &mut memory).unwrap();
            let memory = memory.unwrap();

            let alloc: ID3D12CommandAllocator = dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT).unwrap();
            let list: ID3D12GraphicsCommandList = dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &alloc, None).unwrap();
            let barrier = |b: &[D3D12_RESOURCE_BARRIER]| list.ResourceBarrier(b);
            let copy = |dst: *const D3D12_TEXTURE_COPY_LOCATION, src: *const D3D12_TEXTURE_COPY_LOCATION| list.CopyTextureRegion(dst, 0, 0, 0, src, None);
            list.ClearRenderTargetView(rtv, &[2.5, 0.5, 0.25, 1.0], None);
            record(&target, &memory, first, f, &barrier, &copy);
            list.ClearRenderTargetView(rtv, &[-1.0, 8.0, 0.125, 0.0], None);
            record(&target, &memory, second, f, &barrier, &copy);
            list.Close().unwrap();
            let q: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC { Type: D3D12_COMMAND_LIST_TYPE_DIRECT, ..Default::default() }).unwrap();
            let fence: ID3D12Fence = dev.CreateFence(0, D3D12_FENCE_FLAG_NONE).unwrap();
            q.ExecuteCommandLists(&[Some(list.cast().unwrap())]);
            q.Signal(&fence, 1).unwrap();
            let t0 = std::time::Instant::now();
            while fence.GetCompletedValue() < 1 {
                assert!(t0.elapsed().as_secs() < 10, "GPU timeout");
                std::thread::yield_now();
            }
            assert_eq!(dev.GetDeviceRemovedReason(), Ok(()), "the device is still there");
            let mut p: *mut c_void = std::ptr::null_mut();
            memory.Map(0, None, Some(&mut p)).unwrap();
            let at = |o: u64| -> [f32; 4] { std::array::from_fn(|k| (p as *const f32).add(o as usize / 4 + k).read()) };
            let (a, b) = (at(first), at(second));
            memory.Unmap(0, None);
            assert_eq!(a, [2.5, 0.5, 0.25, 1.0], "the first draw's texel at its address");
            assert_eq!(b, [-1.0, 8.0, 0.125, 0.0], "the second's");
        }
    }
}
