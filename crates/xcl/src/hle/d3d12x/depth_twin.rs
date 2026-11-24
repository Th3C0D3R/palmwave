//! Twins of the console depth buffers that shaders write too (#970), with `XCL_DEPTH_UAV_TWINS=1`.
//!
//! The PC refuses a resource that is both a depth buffer and a UAV texture, so such a console
//! depth buffer loses ALLOW_UNORDERED_ACCESS (`device_x::pc_resource_desc`) and its UAVs were null
//! views – what the title's shaders wrote there was lost (a retail title clears or fills its D16
//! shadow maps that way before it renders depth into them). A twin is a texture of the same
//! typeless format and size with unordered access and no depth: the depth buffer's UAVs are views
//! of the twin (`real::dev_create_uav_x`), and at the title's barriers into the UNORDERED_ACCESS
//! layout the depth buffer is copied to the twin, at those out of it the twin back to the depth
//! buffer (`sync`). The twin is the depth buffer's private data, so it goes when the depth
//! buffer does.
//!
//! The title's enhanced barriers (#146) are applied (`enhanced_barrier`), except those into
//! UNORDERED_ACCESS for such a depth buffer, which stays where it is: the copies' transitions
//! start from the state its barriers left it in (`enhanced_barrier::tracked_state`), else the
//! one it was made in (`note_created`; not known: DEPTH_WRITE, what a depth buffer placed for
//! unordered access is made in).
//!
//! A depth buffer with a stencil plane (R24G8 – the console's D16S8 ones too – and R32G8X24) has
//! no UAV format on the PC: its twin holds the depth plane as R32 words, which its UAVs of that
//! plane write as floats (those of the stencil plane stay null views). A retail title fills such
//! a buffer with material IDs in a compute pass, then lights each material with tiles drawn at its
//! ID's depth and a depth test EQUAL: with the writes lost, the scene stayed unlit. The PC copies
//! a plane only to and from a buffer, so the depth plane and the twin go through a scratch buffer
//! of the depth buffer's, and a pass of XCL's converts the words on the way: a D24 plane holds
//! 24-bit unorm depths (rounded to nearest, as the GPU stores a depth), a D32 one the floats
//! themselves. One subresource only: such a depth buffer with mips or slices gets no twin.

use std::collections::HashMap;
use std::ffi::c_void;
use std::mem::ManuallyDrop;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock};

use windows::core::{Interface, GUID};
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::*;

/// The private data key under which a depth buffer holds its twin.
const TWIN: GUID = GUID::from_u128(0x5c1b_7a4e_9f02_4d36_b0a8_3e6d_1f29_c970);
/// The one under which a depth buffer with a stencil plane holds its scratch buffer.
const SCRATCH: GUID = GUID::from_u128(0x5c1b_7a4e_9f02_4d36_b0a8_3e6d_1f29_c971);

/// `XCL_DEPTH_UAV_TWINS=1`: depth buffers that shaders write get twins (off by default).
pub fn enabled() -> bool {
    static E: OnceLock<bool> = OnceLock::new();
    *E.get_or_init(|| std::env::var("XCL_DEPTH_UAV_TWINS").is_ok_and(|v| v.trim() == "1"))
}

/// The PC state each console depth buffer that is also a UAV was made in.
static MADE_IN: Mutex<Option<HashMap<usize, i32>>> = Mutex::new(None);

/// A console depth buffer that shaders write too was made (`device_x::create_placed_resource_x`)
/// in PC state `state`.
pub fn note_created(res: usize, state: u32) {
    if enabled() {
        MADE_IN.lock().unwrap().get_or_insert_with(HashMap::new).insert(res, state as i32);
    }
}

/// `res` is gone (`real::res_release`): a later resource at its address was not made in its state.
pub fn forget(res: usize) {
    if let Some(m) = MADE_IN.lock().unwrap().as_mut() {
        m.remove(&res);
    }
}

/// The PC state `res` is in: where the title's barriers left it, else where it was made
/// (UNORDERED_ACCESS is never it: the PC has the depth buffer without unordered access).
fn state_of(res: usize) -> D3D12_RESOURCE_STATES {
    let uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS.0;
    let tracked = super::enhanced_barrier::tracked_state(res).map(|s| s as i32).filter(|&s| s != uav);
    let s = tracked.or_else(|| MADE_IN.lock().unwrap().as_ref().and_then(|m| m.get(&res).copied()));
    D3D12_RESOURCE_STATES(s.unwrap_or(D3D12_RESOURCE_STATE_DEPTH_WRITE.0))
}

/// The view a UAV of twin `d` is when the title gives no description: the twin is typeless, and
/// the PC wants the view's format then (R16_UNORM / R32_FLOAT), all of its slices.
pub fn default_uav_desc(d: &D3D12_RESOURCE_DESC) -> D3D12_UNORDERED_ACCESS_VIEW_DESC {
    let format = DXGI_FORMAT(super::device_x::pc_shader_view_format(d.Format.0 as u32, 0) as i32);
    if d.DepthOrArraySize > 1 {
        let slices = D3D12_TEX2D_ARRAY_UAV { MipSlice: 0, FirstArraySlice: 0, ArraySize: d.DepthOrArraySize as u32, PlaneSlice: 0 };
        D3D12_UNORDERED_ACCESS_VIEW_DESC { Format: format, ViewDimension: D3D12_UAV_DIMENSION_TEXTURE2DARRAY, Anonymous: D3D12_UNORDERED_ACCESS_VIEW_DESC_0 { Texture2DArray: slices } }
    } else {
        D3D12_UNORDERED_ACCESS_VIEW_DESC { Format: format, ViewDimension: D3D12_UAV_DIMENSION_TEXTURE2D, ..Default::default() }
    }
}

/// Whether a depth buffer in `format` (typeless, `device_x::pc_resource_desc`) has a stencil plane.
fn stencil_plane(format: DXGI_FORMAT) -> bool {
    matches!(format, DXGI_FORMAT_R24G8_TYPELESS | DXGI_FORMAT_R32G8X24_TYPELESS)
}

/// The view a UAV of a depth buffer in `depth` format is on its twin `twin`, for the title's
/// `view` (in the PC's format, `device_x::pc_shader_view_format`; None: no description). The
/// depth plane of a depth buffer with a stencil plane is the twin's only one, of floats. None: a
/// view of the stencil plane, which the twin does not hold – it stays a null view.
pub fn twin_view(depth: DXGI_FORMAT, twin: &D3D12_RESOURCE_DESC, view: Option<&D3D12_UNORDERED_ACCESS_VIEW_DESC>) -> Option<D3D12_UNORDERED_ACCESS_VIEW_DESC> {
    let Some(&v) = view else { return Some(default_uav_desc(twin)) };
    if !stencil_plane(depth) {
        return Some(v);
    }
    if super::device_x::stencil_view(depth.0 as u32, v.Format.0 as u32) {
        return None;
    }
    let mut v = v;
    v.Format = DXGI_FORMAT_R32_FLOAT;
    match v.ViewDimension {
        D3D12_UAV_DIMENSION_TEXTURE2D => v.Anonymous.Texture2D.PlaneSlice = 0,
        D3D12_UAV_DIMENSION_TEXTURE2DARRAY => v.Anonymous.Texture2DArray.PlaneSlice = 0,
        _ => {}
    }
    Some(v)
}

/// The twin's description for a depth buffer's (None: no twin for it – several samples, a
/// resource that already has unordered access, a stencil plane with more than one subresource).
/// With a stencil plane: one of R32 words for the depth plane.
pub fn twin_desc(d: &D3D12_RESOURCE_DESC) -> Option<D3D12_RESOURCE_DESC> {
    let depth = d.Flags.0 & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL.0 != 0 && d.Flags.0 & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS.0 == 0;
    if !depth || d.SampleDesc.Count != 1 {
        return None;
    }
    let format = match d.Format {
        DXGI_FORMAT_R16_TYPELESS | DXGI_FORMAT_R32_TYPELESS => d.Format,
        f if stencil_plane(f) && d.MipLevels == 1 && d.DepthOrArraySize == 1 => DXGI_FORMAT_R32_TYPELESS,
        _ => return None,
    };
    let mut t = *d;
    t.Format = format;
    t.Flags = D3D12_RESOURCE_FLAGS((d.Flags.0 & !(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL.0 | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE.0)) | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS.0);
    t.Alignment = 0;
    Some(t)
}

/// The twin `res` has (None: none made).
pub unsafe fn twin_of(res: &ID3D12Resource) -> Option<ID3D12Resource> {
    let mut p: *mut c_void = std::ptr::null_mut();
    let mut size = std::mem::size_of::<*mut c_void>() as u32;
    res.GetPrivateData(&TWIN, &mut size, Some(&mut p as *mut _ as *mut c_void)).ok()?;
    // an interface kept with SetPrivateDataInterface comes back with a reference of its own
    (!p.is_null()).then(|| ID3D12Resource::from_raw(p))
}

/// The twin a UAV of `res` is made on – made now if `res` has none yet (None: twins are off, or
/// `res` gets none: `twin_desc`, or it could not be made).
pub unsafe fn twin_for_uav(res: &ID3D12Resource) -> Option<ID3D12Resource> {
    if !enabled() {
        return None;
    }
    make_twin(res)
}

/// `twin_for_uav` whether twins are on or not.
unsafe fn make_twin(res: &ID3D12Resource) -> Option<ID3D12Resource> {
    if let Some(t) = twin_of(res) {
        return Some(t);
    }
    let d = res.GetDesc();
    let td = twin_desc(&d)?;
    let mut dev: Option<ID3D12Device> = None;
    res.GetDevice(&mut dev).ok()?;
    let dev = dev?;
    let props = D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() };
    let mut twin: Option<ID3D12Resource> = None;
    static N: AtomicU64 = AtomicU64::new(0);
    // a depth plane also needs its scratch buffer and XCL's pass
    let created = dev.CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, None, &mut twin).map_err(|e| e.to_string());
    if let Err(e) = created.and_then(|()| if stencil_plane(d.Format) { prepare_planes(&dev, res, &d, &td) } else { Ok(()) }) {
        if N.fetch_add(1, Ordering::Relaxed) < 4 {
            crate::xlog_warn!("D3D12.X: no twin for a depth buffer that shaders write ({}x{}x{}, format {}): {e} – its UAVs are null views", d.Width, d.Height, d.DepthOrArraySize, d.Format.0);
        }
        return None;
    }
    let twin = twin?;
    res.SetPrivateDataInterface(&TWIN, &twin).ok()?;
    let n = N.fetch_add(1, Ordering::Relaxed) + 1;
    if n <= 4 || n.is_power_of_two() {
        crate::xlog_info!("D3D12.X: a depth buffer that shaders write ({}x{}x{}, format {}) has a twin for its UAVs ({n} so far)", d.Width, d.Height, d.DepthOrArraySize, d.Format.0);
    }
    Some(twin)
}

/// The PC list `sync` records into.
pub trait Recorder {
    unsafe fn barrier(&self, barriers: &[D3D12_RESOURCE_BARRIER]);
    unsafe fn copy(&self, dst: &ID3D12Resource, src: &ID3D12Resource);
    unsafe fn copy_region(&self, dst: &D3D12_TEXTURE_COPY_LOCATION, src: &D3D12_TEXTURE_COPY_LOCATION);
    /// XCL's pass between a depth plane and its twin: its pipeline, root constants, root UAV
    unsafe fn pass(&self, rs: &ID3D12RootSignature, pso: &ID3D12PipelineState, constants: &[u32; 8], buffer: u64, groups: [u32; 3]);
}

impl Recorder for ID3D12GraphicsCommandList {
    unsafe fn barrier(&self, barriers: &[D3D12_RESOURCE_BARRIER]) {
        self.ResourceBarrier(barriers);
    }
    unsafe fn copy(&self, dst: &ID3D12Resource, src: &ID3D12Resource) {
        self.CopyResource(dst, src);
    }
    unsafe fn copy_region(&self, dst: &D3D12_TEXTURE_COPY_LOCATION, src: &D3D12_TEXTURE_COPY_LOCATION) {
        self.CopyTextureRegion(dst, 0, 0, 0, src, None);
    }
    unsafe fn pass(&self, rs: &ID3D12RootSignature, pso: &ID3D12PipelineState, constants: &[u32; 8], buffer: u64, groups: [u32; 3]) {
        self.SetComputeRootSignature(rs);
        self.SetPipelineState(pso);
        self.SetComputeRoot32BitConstants(0, 8, constants.as_ptr() as *const c_void, 0);
        self.SetComputeRootUnorderedAccessView(1, buffer);
        self.Dispatch(groups[0], groups[1], groups[2]);
    }
}

/// XCL's pass over the scratch buffer's words (`RWByteAddressBuffer` at u0; b0: width, height,
/// conversion, –, the words from (offset, row pitch), the words to (offset, row pitch)), in
/// integers, exact both ways: 1 a float a shader wrote (saturated, NaN 0) → the 24-bit unorm of
/// a D24 plane, rounded to nearest; 2 a 24-bit unorm → the float nearest to it; 0 the bits as
/// they are.
const PASS_HLSL: &str = r"cbuffer C : register(b0) { uint width; uint height; uint conversion; uint unused; uint src; uint src_pitch; uint dst; uint dst_pitch; };
RWByteAddressBuffer words : register(u0);
static const uint D = 0xFFFFFFu;
uint unorm24(uint b) {
    uint e = (b >> 23) & 0xFFu;
    if ((b >> 31) != 0u || e == 0u) return 0u;
    if (e == 255u) return (b & 0x7FFFFFu) != 0u ? 0u : D;
    if (e >= 127u) return D;
    uint64_t x = (uint64_t)((b & 0x7FFFFFu) | 0x800000u) * D;
    uint s = 150u - e;
    if (s >= 64u) return 0u;
    uint64_t one = 1;
    uint64_t q = x >> s, r = x & ((one << s) - one), mid = one << (s - 1u);
    if (r > mid || (r == mid && (q & one) != 0)) q += one;
    return (uint)q;
}
uint float_of(uint w) {
    uint k = w & D;
    if (k == 0u) return 0u;
    if (k == D) return 0x3F800000u;
    uint s = 24u - firstbithigh(k);
    uint64_t one = 1;
    uint64_t n = (uint64_t)(k << s) << 23;
    uint64_t q = n / D, r = n % D;
    if (2 * r > D) q += one;
    uint e = 127u - s;
    if (q == (one << 24)) { q = one << 23; e += 1u; }
    return (e << 23) | ((uint)q & 0x7FFFFFu);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    uint w = words.Load(src + id.y * src_pitch + 4u * id.x);
    if (conversion == 1u) w = unorm24(w);
    else if (conversion == 2u) w = float_of(w);
    words.Store(dst + id.y * dst_pitch + 4u * id.x, w);
}
";

/// XCL's pass on `dev`, made once.
unsafe fn plane_pass(dev: &ID3D12Device) -> Result<(ID3D12RootSignature, ID3D12PipelineState), String> {
    static PASSES: Mutex<Option<HashMap<usize, (ID3D12RootSignature, ID3D12PipelineState)>>> = Mutex::new(None);
    let key = dev.as_raw() as usize;
    if let Some(p) = PASSES.lock().unwrap().as_ref().and_then(|m| m.get(&key).cloned()) {
        return Ok(p);
    }
    let dxil = crate::gpu::recomp::dxc::compile_cached(PASS_HLSL, "main", "cs_6_6")?.0;
    let params = [
        D3D12_ROOT_PARAMETER {
            ParameterType: D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
            Anonymous: D3D12_ROOT_PARAMETER_0 { Constants: D3D12_ROOT_CONSTANTS { ShaderRegister: 0, RegisterSpace: 0, Num32BitValues: 8 } },
            ShaderVisibility: D3D12_SHADER_VISIBILITY_ALL,
        },
        D3D12_ROOT_PARAMETER {
            ParameterType: D3D12_ROOT_PARAMETER_TYPE_UAV,
            Anonymous: D3D12_ROOT_PARAMETER_0 { Descriptor: D3D12_ROOT_DESCRIPTOR { ShaderRegister: 0, RegisterSpace: 0 } },
            ShaderVisibility: D3D12_SHADER_VISIBILITY_ALL,
        },
    ];
    let desc = D3D12_ROOT_SIGNATURE_DESC { NumParameters: params.len() as u32, pParameters: params.as_ptr(), ..Default::default() };
    let mut blob = None;
    D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &mut blob, None).map_err(|e| format!("root signature: {e}"))?;
    let blob = blob.ok_or("root signature")?;
    let rs: ID3D12RootSignature = dev.CreateRootSignature(0, std::slice::from_raw_parts(blob.GetBufferPointer() as *const u8, blob.GetBufferSize())).map_err(|e| format!("root signature: {e}"))?;
    let mut pd = D3D12_COMPUTE_PIPELINE_STATE_DESC {
        pRootSignature: ManuallyDrop::new(Some(rs.clone())),
        CS: D3D12_SHADER_BYTECODE { pShaderBytecode: dxil.as_ptr() as *const c_void, BytecodeLength: dxil.len() },
        ..Default::default()
    };
    let pso = dev.CreateComputePipelineState(&pd);
    ManuallyDrop::drop(&mut pd.pRootSignature);
    let pso: ID3D12PipelineState = pso.map_err(|e| format!("pipeline: {e}"))?;
    PASSES.lock().unwrap().get_or_insert_with(HashMap::new).insert(key, (rs.clone(), pso.clone()));
    Ok((rs, pso))
}

/// Where the depth plane's words and the twin's are in the scratch buffer (their footprints, at
/// their offsets), and the buffer's size: in the same place when their rows are as long.
unsafe fn plane_layout(dev: &ID3D12Device, depth: &D3D12_RESOURCE_DESC, twin: &D3D12_RESOURCE_DESC) -> (D3D12_PLACED_SUBRESOURCE_FOOTPRINT, D3D12_PLACED_SUBRESOURCE_FOOTPRINT, u64) {
    let footprint = |d: &D3D12_RESOURCE_DESC, at: u64| {
        let (mut fp, mut bytes) = (D3D12_PLACED_SUBRESOURCE_FOOTPRINT::default(), 0u64);
        dev.GetCopyableFootprints(d, 0, 1, at, Some(&mut fp), None, None, Some(&mut bytes));
        (fp, bytes)
    };
    let (t, t_bytes) = footprint(twin, 0);
    let at = if footprint(depth, 0).0.Footprint.RowPitch == t.Footprint.RowPitch { 0 } else { t_bytes.next_multiple_of(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT as u64) };
    let (d, d_bytes) = footprint(depth, at);
    (d, t, (at + d_bytes).max(t_bytes))
}

/// A depth buffer with a stencil plane gets its twin: its scratch buffer (its private data), XCL's
/// pass on its device.
unsafe fn prepare_planes(dev: &ID3D12Device, res: &ID3D12Resource, d: &D3D12_RESOURCE_DESC, td: &D3D12_RESOURCE_DESC) -> Result<(), String> {
    plane_pass(dev)?;
    let bytes = plane_layout(dev, d, td).2;
    let desc = D3D12_RESOURCE_DESC {
        Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
        Width: bytes,
        Height: 1,
        DepthOrArraySize: 1,
        MipLevels: 1,
        SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
        Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        Flags: D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        ..Default::default()
    };
    let props = D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() };
    let mut scratch: Option<ID3D12Resource> = None;
    dev.CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, None, &mut scratch).map_err(|e| format!("scratch buffer: {e}"))?;
    res.SetPrivateDataInterface(&SCRATCH, &scratch.ok_or("scratch buffer")?).map_err(|e| format!("scratch buffer: {e}"))
}

/// The scratch buffer of `res` (None: it has none).
unsafe fn scratch_of(res: &ID3D12Resource) -> Option<ID3D12Resource> {
    let mut p: *mut c_void = std::ptr::null_mut();
    let mut size = std::mem::size_of::<*mut c_void>() as u32;
    res.GetPrivateData(&SCRATCH, &mut size, Some(&mut p as *mut _ as *mut c_void)).ok()?;
    (!p.is_null()).then(|| ID3D12Resource::from_raw(p))
}

/// `sync` of a depth buffer with a stencil plane: subresource 0 of `from` (the depth buffer or the
/// twin, the state it is in, where its words go in the scratch buffer) into the scratch buffer,
/// XCL's pass over them with `conversion`, then into subresource 0 of `to` – the transitions each
/// copy needs around it; the scratch buffer starts and ends in COMMON.
unsafe fn through_scratch(rec: &dyn Recorder, scratch: &ID3D12Resource, from: (&ID3D12Resource, D3D12_RESOURCE_STATES, D3D12_PLACED_SUBRESOURCE_FOOTPRINT), to: (&ID3D12Resource, D3D12_RESOURCE_STATES, D3D12_PLACED_SUBRESOURCE_FOOTPRINT), conversion: u32, pass: &(ID3D12RootSignature, ID3D12PipelineState)) {
    let (src, dst, uav, common) = (D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    let placed = |fp| D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(scratch), Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { PlacedFootprint: fp } };
    let plane = |r: &ID3D12Resource| D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(r), Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: 0 } };
    // (a resource already in the copy's state gets no transition: the PC refuses one to itself)
    let around = |r: &ID3D12Resource, state, copy, mut b: Vec<D3D12_RESOURCE_BARRIER>, enter: bool| {
        if state != copy {
            b.push(if enter { transition(r, state, copy) } else { transition(r, copy, state) });
        }
        rec.barrier(&b);
    };
    around(from.0, from.1, src, vec![transition(scratch, common, dst)], true);
    rec.copy_region(&placed(from.2), &plane(from.0));
    around(from.0, from.1, src, vec![transition(scratch, dst, uav)], false);
    let fp = |f: D3D12_PLACED_SUBRESOURCE_FOOTPRINT| [f.Offset as u32, f.Footprint.RowPitch];
    let (w, h) = (to.2.Footprint.Width, to.2.Footprint.Height);
    let constants = [w, h, conversion, 0, fp(from.2)[0], fp(from.2)[1], fp(to.2)[0], fp(to.2)[1]];
    rec.pass(&pass.0, &pass.1, &constants, scratch.GetGPUVirtualAddress(), [w.div_ceil(8), h.div_ceil(8), 1]);
    around(to.0, to.1, dst, vec![transition(scratch, uav, src)], true);
    rec.copy_region(&plane(to.0), &placed(to.2));
    around(to.0, to.1, dst, vec![transition(scratch, src, common)], false);
}

fn transition(res: &ID3D12Resource, before: D3D12_RESOURCE_STATES, after: D3D12_RESOURCE_STATES) -> D3D12_RESOURCE_BARRIER {
    D3D12_RESOURCE_BARRIER {
        Type: D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        Flags: D3D12_RESOURCE_BARRIER_FLAG_NONE,
        Anonymous: D3D12_RESOURCE_BARRIER_0 {
            Transition: ManuallyDrop::new(D3D12_RESOURCE_TRANSITION_BARRIER {
                // borrowed: the barrier holds no reference of its own
                pResource: unsafe { std::mem::transmute_copy(res) },
                Subresource: D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                StateBefore: before,
                StateAfter: after,
            }),
        },
    }
}

/// At a barrier of the title that moves depth buffer `res` into (`into_uav`) or out of the
/// UNORDERED_ACCESS layout: the depth buffer is copied to its twin, or the twin back to it, with
/// the transitions the copy needs around it, on `rec`'s list (a depth plane through the scratch
/// buffer and XCL's pass). Nothing when `res` has no twin. Returns whether it copied.
pub unsafe fn sync(res: usize, into_uav: bool, rec: &dyn Recorder) -> bool {
    let r = res as *mut c_void;
    let Some(depth) = ID3D12Resource::from_raw_borrowed(&r) else { return false };
    let Some(twin) = twin_of(depth) else { return false };
    let made = state_of(res);
    let d = depth.GetDesc();
    if stencil_plane(d.Format) {
        let mut dev: Option<ID3D12Device> = None;
        let _ = depth.GetDevice(&mut dev);
        let (Some(scratch), Some(dev)) = (scratch_of(depth), dev) else { return false };
        let Ok(pass) = plane_pass(&dev) else { return false };
        let (dfp, tfp, _) = plane_layout(&dev, &d, &twin.GetDesc());
        let uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        // a D24 plane's unorm depths and the twin's floats; a D32 plane's floats as they are
        let d24 = d.Format == DXGI_FORMAT_R24G8_TYPELESS;
        if into_uav {
            through_scratch(rec, &scratch, (depth, made, dfp), (&twin, uav, tfp), if d24 { 2 } else { 0 }, &pass);
        } else {
            through_scratch(rec, &scratch, (&twin, uav, tfp), (depth, made, dfp), if d24 { 1 } else { 0 }, &pass);
        }
        note_copy(&d, into_uav);
        return true;
    }
    let uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    let (src, dst) = (D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    let (depth_copy, twin_copy) = if into_uav { (src, dst) } else { (dst, src) };
    // (a depth buffer already in the copy's state gets no transition: the PC refuses one from a
    // state to itself)
    let moves = made != depth_copy;
    let mut before = vec![transition(&twin, uav, twin_copy)];
    let mut after = vec![transition(&twin, twin_copy, uav)];
    if moves {
        before.insert(0, transition(depth, made, depth_copy));
        after.insert(0, transition(depth, depth_copy, made));
    }
    rec.barrier(&before);
    if into_uav {
        rec.copy(&twin, depth);
    } else {
        rec.copy(depth, &twin);
    }
    rec.barrier(&after);
    note_copy(&d, into_uav);
    true
}

/// A copy between depth buffer `d` and its twin: counted, the first 4 and every power of two logged.
fn note_copy(d: &D3D12_RESOURCE_DESC, into_uav: bool) {
    static N: AtomicU64 = AtomicU64::new(0);
    let n = N.fetch_add(1, Ordering::Relaxed) + 1;
    if n <= 4 || n.is_power_of_two() {
        let what = if into_uav { "copied to the twin" } else { "the twin copied back" };
        crate::xlog_info!("D3D12.X: a depth buffer and its twin ({}x{}x{}, format {}): {what} ({n} copies so far)", d.Width, d.Height, d.DepthOrArraySize, d.Format.0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn depth_desc(format: DXGI_FORMAT, flags: D3D12_RESOURCE_FLAGS) -> D3D12_RESOURCE_DESC {
        D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            Alignment: 65536,
            Width: 768,
            Height: 768,
            DepthOrArraySize: 6,
            MipLevels: 1,
            Format: format,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Layout: D3D12_TEXTURE_LAYOUT_UNKNOWN,
            Flags: flags,
        }
    }

    /// A single-plane depth buffer without unordered access gets a twin of its format and size
    /// with unordered access and no depth; one of two planes with several slices, a UAV texture
    /// or MSAA does not.
    #[test]
    fn a_d16_or_d32_depth_buffer_gets_a_twin_with_unordered_access() {
        let ds = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        for f in [DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R32_TYPELESS] {
            let t = twin_desc(&depth_desc(f, ds | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)).unwrap();
            assert_eq!((t.Format, t.Width, t.Height, t.DepthOrArraySize, t.MipLevels, t.Alignment), (f, 768, 768, 6, 1, 0));
            assert_eq!(t.Flags, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        }
        assert!(twin_desc(&depth_desc(DXGI_FORMAT_R24G8_TYPELESS, ds)).is_none(), "depth + stencil, 6 slices");
        assert!(twin_desc(&depth_desc(DXGI_FORMAT_R32G8X24_TYPELESS, ds)).is_none(), "depth + stencil, 6 slices");
        assert!(twin_desc(&depth_desc(DXGI_FORMAT_R16_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)).is_none(), "not a depth buffer");
        let mut msaa = depth_desc(DXGI_FORMAT_R32_TYPELESS, ds);
        msaa.SampleDesc.Count = 4;
        assert!(twin_desc(&msaa).is_none());
    }

    /// A UAV made without a description is a view of the twin in its format's readable type.
    #[test]
    fn a_twins_uav_without_a_description_has_the_twins_format() {
        let ds = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        let shadows = twin_desc(&depth_desc(DXGI_FORMAT_R16_TYPELESS, ds)).unwrap();
        let v = default_uav_desc(&shadows);
        assert_eq!((v.Format, v.ViewDimension), (DXGI_FORMAT_R16_UNORM, D3D12_UAV_DIMENSION_TEXTURE2DARRAY));
        let a = unsafe { v.Anonymous.Texture2DArray };
        assert_eq!((a.MipSlice, a.FirstArraySlice, a.ArraySize, a.PlaneSlice), (0, 0, 6, 0));
        let mut one = twin_desc(&depth_desc(DXGI_FORMAT_R32_TYPELESS, ds)).unwrap();
        one.DepthOrArraySize = 1;
        let v = default_uav_desc(&one);
        assert_eq!((v.Format, v.ViewDimension), (DXGI_FORMAT_R32_FLOAT, D3D12_UAV_DIMENSION_TEXTURE2D));
    }

    /// A depth buffer with a stencil plane and one subresource (a material-ID buffer of
    /// 2560x1440) gets a twin of R32 words for its depth plane; with mips it gets none. The UAVs
    /// of its depth plane are R32_FLOAT views of the twin, those of its stencil plane none; a
    /// single-plane depth buffer's UAVs keep their view.
    #[test]
    fn a_depth_stencil_buffer_gets_a_twin_of_its_depth_plane() {
        let ds = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        for f in [DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R32G8X24_TYPELESS] {
            let mut d = depth_desc(f, ds);
            (d.Width, d.Height, d.DepthOrArraySize) = (2560, 1440, 1);
            let t = twin_desc(&d).unwrap();
            assert_eq!((t.Format, t.Width, t.Height, t.DepthOrArraySize, t.MipLevels), (DXGI_FORMAT_R32_TYPELESS, 2560, 1440, 1, 1));
            assert_eq!(t.Flags, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            d.MipLevels = 2;
            assert!(twin_desc(&d).is_none(), "{f:?} with mips");
        }
        let mut d = depth_desc(DXGI_FORMAT_R24G8_TYPELESS, ds);
        d.DepthOrArraySize = 1;
        let t = twin_desc(&d).unwrap();
        let view = |format, plane| {
            let mut v = D3D12_UNORDERED_ACCESS_VIEW_DESC { Format: format, ViewDimension: D3D12_UAV_DIMENSION_TEXTURE2D, ..Default::default() };
            v.Anonymous.Texture2D = D3D12_TEX2D_UAV { MipSlice: 0, PlaneSlice: plane };
            v
        };
        // (formats as `pc_shader_view_format` leaves them: R24_UNORM_X8 / X24_G8_UINT, R32_FLOAT_X8X24 / X32_G8X24_UINT)
        let depth_plane = twin_view(DXGI_FORMAT_R24G8_TYPELESS, &t, Some(&view(DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 0))).unwrap();
        assert_eq!((depth_plane.Format, depth_plane.ViewDimension, unsafe { depth_plane.Anonymous.Texture2D.PlaneSlice }), (DXGI_FORMAT_R32_FLOAT, D3D12_UAV_DIMENSION_TEXTURE2D, 0));
        assert_eq!(twin_view(DXGI_FORMAT_R32G8X24_TYPELESS, &t, Some(&view(DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, 0))).map(|v| v.Format), Some(DXGI_FORMAT_R32_FLOAT));
        assert!(twin_view(DXGI_FORMAT_R24G8_TYPELESS, &t, Some(&view(DXGI_FORMAT_X24_TYPELESS_G8_UINT, 1))).is_none(), "the stencil plane");
        assert!(twin_view(DXGI_FORMAT_R32G8X24_TYPELESS, &t, Some(&view(DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, 1))).is_none(), "the stencil plane");
        assert_eq!(twin_view(DXGI_FORMAT_R24G8_TYPELESS, &t, None).map(|v| v.Format), Some(DXGI_FORMAT_R32_FLOAT));
        let shadows = twin_desc(&depth_desc(DXGI_FORMAT_R16_TYPELESS, ds)).unwrap();
        assert_eq!(twin_view(DXGI_FORMAT_R16_TYPELESS, &shadows, Some(&view(DXGI_FORMAT_R16_UNORM, 0))).map(|v| v.Format), Some(DXGI_FORMAT_R16_UNORM));
    }

    /// The transitions `sync` asks for, as (resource, before, after); nothing is recorded on a list.
    #[derive(Default)]
    struct Transitions(std::cell::RefCell<Vec<(usize, i32, i32)>>);

    impl Recorder for Transitions {
        unsafe fn barrier(&self, barriers: &[D3D12_RESOURCE_BARRIER]) {
            for b in barriers {
                let t = &b.Anonymous.Transition;
                self.0.borrow_mut().push((std::mem::transmute_copy::<_, usize>(&t.pResource), t.StateBefore.0, t.StateAfter.0));
            }
        }
        unsafe fn copy(&self, _: &ID3D12Resource, _: &ID3D12Resource) {}
        unsafe fn copy_region(&self, _: &D3D12_TEXTURE_COPY_LOCATION, _: &D3D12_TEXTURE_COPY_LOCATION) {}
        unsafe fn pass(&self, _: &ID3D12RootSignature, _: &ID3D12PipelineState, _: &[u32; 8], _: u64, _: [u32; 3]) {}
    }

    /// The copies' transitions start from the state the depth buffer is in: where the title's
    /// barriers left it, else where it was made – and one already in the copy's state gets no
    /// transition from that state to itself. A released depth buffer's state is forgotten.
    #[test]
    fn gpu_the_copies_start_from_the_state_the_depth_buffer_is_in() {
        use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
        unsafe {
            let mut dev: Option<ID3D12Device> = None;
            if D3D12CreateDevice(None, D3D_FEATURE_LEVEL_11_0, &mut dev).is_err() {
                crate::gpu::recomp::runner::skip_test("no GPU");
                return;
            }
            let dev = dev.unwrap();
            let mut d = depth_desc(DXGI_FORMAT_R16_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
            (d.Width, d.Height, d.DepthOrArraySize, d.Alignment) = (64, 32, 1, 0);
            let default = D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() };
            let mut depth: Option<ID3D12Resource> = None;
            dev.CreateCommittedResource(&default, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_SOURCE, None, &mut depth).unwrap();
            let depth = depth.unwrap();
            let res = depth.as_raw() as usize;
            let twin = make_twin(&depth).expect("a twin");
            let seen = Transitions::default();
            let (src, dst, uav) = (D3D12_RESOURCE_STATE_COPY_SOURCE.0, D3D12_RESOURCE_STATE_COPY_DEST.0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS.0);
            let t = twin.as_raw() as usize;

            super::super::enhanced_barrier::forget(res);
            // made in COPY_SOURCE, copied to the twin: only the twin moves
            MADE_IN.lock().unwrap().get_or_insert_with(HashMap::new).insert(res, src);
            assert!(sync(res, true, &seen));
            assert_eq!(seen.0.take(), vec![(t, uav, dst), (t, dst, uav)]);
            // the twin copied back: the depth buffer moves to COPY_DEST and back
            assert!(sync(res, false, &seen));
            assert_eq!(seen.0.take(), vec![(res, src, dst), (t, uav, src), (res, dst, src), (t, src, uav)]);

            // the title's barriers moved it since: from where they left it
            let srv = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE.0;
            super::super::enhanced_barrier::note_state(res, srv as u32);
            assert!(sync(res, true, &seen));
            assert_eq!(seen.0.take(), vec![(res, srv, src), (t, uav, dst), (res, src, srv), (t, dst, uav)]);
            // (a state it cannot be in on the PC is not taken)
            super::super::enhanced_barrier::note_state(res, uav as u32);
            assert_eq!(state_of(res).0, src);
            super::super::enhanced_barrier::forget(res);

            // released: a later resource at this address is not taken for one made in COPY_SOURCE
            forget(res);
            assert_eq!(state_of(res), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
    }

    /// The round trip of a D16 depth buffer that shaders write: its clear reaches the twin at the
    /// barrier into UNORDERED_ACCESS, a write through the twin's UAV reaches the depth buffer at
    /// the barrier out of it, and the depth buffer is in the state it was made in afterwards.
    #[test]
    fn gpu_a_uav_write_to_a_depth_buffer_goes_through_its_twin() {
        use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
        unsafe {
            let mut dev: Option<ID3D12Device> = None;
            if D3D12CreateDevice(None, D3D_FEATURE_LEVEL_11_0, &mut dev).is_err() {
                crate::gpu::recomp::runner::skip_test("no GPU");
                return;
            }
            let dev = dev.unwrap();
            let (w, h) = (64u32, 32u32);
            let mut d = depth_desc(DXGI_FORMAT_R16_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
            (d.Width, d.Height, d.DepthOrArraySize, d.Alignment) = (w as u64, h, 1, 0);
            let default = D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() };
            let mut depth: Option<ID3D12Resource> = None;
            dev.CreateCommittedResource(&default, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_DEPTH_WRITE, None, &mut depth).unwrap();
            let depth = depth.unwrap();
            assert!(twin_of(&depth).is_none());
            let twin = make_twin(&depth).expect("a twin");
            assert_eq!(twin_of(&depth).map(|t| t.as_raw()), Some(twin.as_raw()), "kept with the depth buffer");
            assert_eq!(make_twin(&depth).map(|t| t.as_raw()), Some(twin.as_raw()), "made once");

            // the depth buffer's DSV, and the twin's UAV in a shader-visible and a CPU heap (for the clear)
            let heap = |ty, n, flags| -> ID3D12DescriptorHeap { dev.CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC { Type: ty, NumDescriptors: n, Flags: flags, NodeMask: 0 }).unwrap() };
            let dsv_heap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
            let (gpu_heap, cpu_heap) = (heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE), heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE));
            let dsv = dsv_heap.GetCPUDescriptorHandleForHeapStart();
            dev.CreateDepthStencilView(&depth, Some(&D3D12_DEPTH_STENCIL_VIEW_DESC { Format: DXGI_FORMAT_D16_UNORM, ViewDimension: D3D12_DSV_DIMENSION_TEXTURE2D, ..Default::default() }), dsv);
            let uav = D3D12_UNORDERED_ACCESS_VIEW_DESC { Format: DXGI_FORMAT_R16_UNORM, ViewDimension: D3D12_UAV_DIMENSION_TEXTURE2D, ..Default::default() };
            dev.CreateUnorderedAccessView(&twin, None, Some(&uav), gpu_heap.GetCPUDescriptorHandleForHeapStart());
            dev.CreateUnorderedAccessView(&twin, None, Some(&uav), cpu_heap.GetCPUDescriptorHandleForHeapStart());

            // a readback buffer for one texture of R16 (rows of 256 bytes)
            let pitch = 256u32;
            let rb_desc = D3D12_RESOURCE_DESC {
                Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
                Width: (pitch * h) as u64,
                Height: 1,
                DepthOrArraySize: 1,
                MipLevels: 1,
                SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
                Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                ..Default::default()
            };
            let readback_props = D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_READBACK, ..Default::default() };
            let q: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC { Type: D3D12_COMMAND_LIST_TYPE_DIRECT, ..Default::default() }).unwrap();
            let fence: ID3D12Fence = dev.CreateFence(0, D3D12_FENCE_FLAG_NONE).unwrap();
            let mut fence_value = 0u64;
            // one list: `record`, then a copy of `src` (in `state`) to a readback buffer, run and read
            let run = |record: &dyn Fn(&ID3D12GraphicsCommandList), src: &ID3D12Resource, state: D3D12_RESOURCE_STATES, fence_value: &mut u64| -> Vec<u16> {
                let alloc: ID3D12CommandAllocator = dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT).unwrap();
                let list: ID3D12GraphicsCommandList = dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &alloc, None).unwrap();
                record(&list);
                let mut rb: Option<ID3D12Resource> = None;
                dev.CreateCommittedResource(&readback_props, D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST, None, &mut rb).unwrap();
                let rb = rb.unwrap();
                list.ResourceBarrier(&[transition(src, state, D3D12_RESOURCE_STATE_COPY_SOURCE)]);
                let footprint = D3D12_PLACED_SUBRESOURCE_FOOTPRINT {
                    Offset: 0,
                    Footprint: D3D12_SUBRESOURCE_FOOTPRINT { Format: DXGI_FORMAT_R16_TYPELESS, Width: w, Height: h, Depth: 1, RowPitch: pitch },
                };
                let to = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(&rb), Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { PlacedFootprint: footprint } };
                let from = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(src), Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: 0 } };
                list.CopyTextureRegion(&to, 0, 0, 0, &from, None);
                list.ResourceBarrier(&[transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, state)]);
                list.Close().unwrap();
                q.ExecuteCommandLists(&[Some(list.cast().unwrap())]);
                *fence_value += 1;
                q.Signal(&fence, *fence_value).unwrap();
                while fence.GetCompletedValue() < *fence_value {
                    std::thread::yield_now();
                }
                let mut p: *mut c_void = std::ptr::null_mut();
                rb.Map(0, None, Some(&mut p)).unwrap();
                let rows = std::slice::from_raw_parts(p as *const u8, (pitch * h) as usize);
                let px: Vec<u16> = (0..h as usize).flat_map(|y| (0..w as usize).map(move |x| u16::from_le_bytes([rows[y * pitch as usize + 2 * x], rows[y * pitch as usize + 2 * x + 1]]))).collect();
                rb.Unmap(0, None);
                px
            };

            // the depth buffer cleared to 0.25, then the barrier into UNORDERED_ACCESS: the twin has it
            let into = |l: &ID3D12GraphicsCommandList| {
                l.ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.25, 0, None);
                assert!(sync(depth.as_raw() as usize, true, l));
            };
            let twin_px = run(&into, &twin, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &mut fence_value);
            assert!(twin_px.iter().all(|&v| v.abs_diff(16384) <= 1), "the twin has the depth buffer's 0.25: {:?}", &twin_px[..4]);

            // a write through the twin's UAV (0.75), then the barrier out of UNORDERED_ACCESS: the depth buffer has it
            let out = |l: &ID3D12GraphicsCommandList| {
                l.SetDescriptorHeaps(&[Some(gpu_heap.clone())]);
                l.ClearUnorderedAccessViewFloat(gpu_heap.GetGPUDescriptorHandleForHeapStart(), cpu_heap.GetCPUDescriptorHandleForHeapStart(), &twin, &[0.75, 0.0, 0.0, 0.0], &[]);
                assert!(sync(depth.as_raw() as usize, false, l));
            };
            let depth_px = run(&out, &depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, &mut fence_value);
            assert!(depth_px.iter().all(|&v| v.abs_diff(49151) <= 1), "the depth buffer has the UAV's 0.75: {:?}", &depth_px[..4]);
            assert_eq!(dev.GetDeviceRemovedReason(), Ok(()), "the device is still there");
        }
    }

    /// A device, a direct queue and its fence, for the tests of depth buffers with a stencil plane.
    struct Gpu {
        dev: ID3D12Device,
        q: ID3D12CommandQueue,
        fence: ID3D12Fence,
        done: std::cell::Cell<u64>,
    }

    impl Gpu {
        /// None: no GPU, or no DXC for XCL's pass (the test is skipped).
        unsafe fn new() -> Option<Gpu> {
            use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
            let mut dev: Option<ID3D12Device> = None;
            if D3D12CreateDevice(None, D3D_FEATURE_LEVEL_11_0, &mut dev).is_err() {
                crate::gpu::recomp::runner::skip_test("no GPU");
                return None;
            }
            if crate::gpu::recomp::dxc::location().is_err() {
                crate::gpu::recomp::runner::skip_test("no DXC");
                return None;
            }
            let dev = dev.unwrap();
            let q = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC { Type: D3D12_COMMAND_LIST_TYPE_DIRECT, ..Default::default() }).unwrap();
            let fence = dev.CreateFence(0, D3D12_FENCE_FLAG_NONE).unwrap();
            Some(Gpu { dev, q, fence, done: Default::default() })
        }

        /// A committed resource of `desc` on a heap of `ty`, made in `state`.
        unsafe fn resource(&self, ty: D3D12_HEAP_TYPE, desc: &D3D12_RESOURCE_DESC, state: D3D12_RESOURCE_STATES) -> ID3D12Resource {
            let mut r: Option<ID3D12Resource> = None;
            self.dev.CreateCommittedResource(&D3D12_HEAP_PROPERTIES { Type: ty, ..Default::default() }, D3D12_HEAP_FLAG_NONE, desc, state, None, &mut r).unwrap();
            r.unwrap()
        }

        /// Subresource `sub` of a texture as words in a buffer: its footprint, and a buffer of it.
        unsafe fn footprint(&self, res: &ID3D12Resource, sub: u32, ty: D3D12_HEAP_TYPE) -> (D3D12_PLACED_SUBRESOURCE_FOOTPRINT, ID3D12Resource) {
            let (mut fp, mut bytes) = (D3D12_PLACED_SUBRESOURCE_FOOTPRINT::default(), 0u64);
            self.dev.GetCopyableFootprints(&res.GetDesc(), sub, 1, 0, Some(&mut fp), None, None, Some(&mut bytes));
            let desc = D3D12_RESOURCE_DESC { Dimension: D3D12_RESOURCE_DIMENSION_BUFFER, Width: bytes, Height: 1, DepthOrArraySize: 1, MipLevels: 1, SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 }, Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR, ..Default::default() };
            let state = if ty == D3D12_HEAP_TYPE_UPLOAD { D3D12_RESOURCE_STATE_GENERIC_READ } else { D3D12_RESOURCE_STATE_COPY_DEST };
            (fp, self.resource(ty, &desc, state))
        }

        /// One list: `record`, then subresource `sub` of `res` (in `state`) to a readback buffer;
        /// run, and its texels' first `size` bytes, row by row.
        unsafe fn read(&self, record: &dyn Fn(&ID3D12GraphicsCommandList), res: &ID3D12Resource, sub: u32, state: D3D12_RESOURCE_STATES, size: usize) -> Vec<u8> {
            let alloc: ID3D12CommandAllocator = self.dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT).unwrap();
            let list: ID3D12GraphicsCommandList = self.dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &alloc, None).unwrap();
            record(&list);
            let (fp, rb) = self.footprint(res, sub, D3D12_HEAP_TYPE_READBACK);
            let plane = |before, after| {
                let mut b = transition(res, before, after);
                (*b.Anonymous.Transition).Subresource = sub;
                b
            };
            list.ResourceBarrier(&[plane(state, D3D12_RESOURCE_STATE_COPY_SOURCE)]);
            let to = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(&rb), Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { PlacedFootprint: fp } };
            let from = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(res), Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: sub } };
            list.CopyTextureRegion(&to, 0, 0, 0, &from, None);
            list.ResourceBarrier(&[plane(D3D12_RESOURCE_STATE_COPY_SOURCE, state)]);
            list.Close().unwrap();
            self.q.ExecuteCommandLists(&[Some(list.cast().unwrap())]);
            self.done.set(self.done.get() + 1);
            self.q.Signal(&self.fence, self.done.get()).unwrap();
            while self.fence.GetCompletedValue() < self.done.get() {
                std::thread::yield_now();
            }
            let mut p: *mut c_void = std::ptr::null_mut();
            rb.Map(0, None, Some(&mut p)).unwrap();
            let f = fp.Footprint;
            let rows = std::slice::from_raw_parts(p as *const u8, (f.RowPitch * (f.Height - 1)) as usize + size * f.Width as usize);
            let texels = (0..f.Height as usize).flat_map(|y| rows[y * f.RowPitch as usize..][..size * f.Width as usize].to_vec()).collect();
            rb.Unmap(0, None);
            texels
        }

        /// An upload buffer of subresource 0 of `res` holding `words`, row by row, and a copy of it
        /// into `res` (in `state`) recorded on `list`.
        unsafe fn upload(&self, list: &ID3D12GraphicsCommandList, res: &ID3D12Resource, state: D3D12_RESOURCE_STATES, words: &[u32]) -> ID3D12Resource {
            let (fp, buffer) = self.footprint(res, 0, D3D12_HEAP_TYPE_UPLOAD);
            let mut p: *mut c_void = std::ptr::null_mut();
            buffer.Map(0, None, Some(&mut p)).unwrap();
            let w = fp.Footprint.Width as usize;
            for (y, row) in words.chunks(w).enumerate() {
                std::ptr::copy_nonoverlapping(row.as_ptr(), (p as *mut u8).add(y * fp.Footprint.RowPitch as usize) as *mut u32, row.len());
            }
            buffer.Unmap(0, None);
            list.ResourceBarrier(&[transition(res, state, D3D12_RESOURCE_STATE_COPY_DEST)]);
            let to = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(res), Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: 0 } };
            let from = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(&buffer), Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { PlacedFootprint: fp } };
            list.CopyTextureRegion(&to, 0, 0, 0, &from, None);
            list.ResourceBarrier(&[transition(res, D3D12_RESOURCE_STATE_COPY_DEST, state)]);
            buffer
        }

        /// A 64x32 depth buffer of `format` with a stencil plane (DEPTH_WRITE), its DSV (`view`) in
        /// a heap of its own, and its twin.
        unsafe fn depth(&self, format: DXGI_FORMAT, view: DXGI_FORMAT) -> (ID3D12Resource, ID3D12DescriptorHeap, ID3D12Resource) {
            let mut d = depth_desc(format, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
            (d.Width, d.Height, d.DepthOrArraySize, d.Alignment) = (64, 32, 1, 0);
            let depth = self.resource(D3D12_HEAP_TYPE_DEFAULT, &d, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            let heap: ID3D12DescriptorHeap = self.dev.CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC { Type: D3D12_DESCRIPTOR_HEAP_TYPE_DSV, NumDescriptors: 1, ..Default::default() }).unwrap();
            self.dev.CreateDepthStencilView(&depth, Some(&D3D12_DEPTH_STENCIL_VIEW_DESC { Format: view, ViewDimension: D3D12_DSV_DIMENSION_TEXTURE2D, ..Default::default() }), heap.GetCPUDescriptorHandleForHeapStart());
            let twin = make_twin(&depth).expect("a twin of the depth plane");
            assert_eq!(twin.GetDesc().Format, DXGI_FORMAT_R32_TYPELESS);
            (depth, heap, twin)
        }
    }

    fn words(bytes: &[u8]) -> Vec<u32> {
        bytes.chunks_exact(4).map(|c| u32::from_le_bytes(c.try_into().unwrap())).collect()
    }

    /// `f` as a D24 plane holds it, exactly: round(saturate(f) · (2^24 − 1)) to nearest even
    /// (the product of two 24-bit numbers is exact in f64), NaN 0.
    fn unorm24(f: f32) -> u32 {
        if f.is_nan() || f <= 0.0 {
            return 0;
        }
        (f.min(1.0) as f64 * 16_777_215.0).round_ties_even() as u32
    }

    /// The round trip of a D24S8 depth buffer: its depths reach the twin as the floats nearest
    /// to them at the barrier into UNORDERED_ACCESS and come back unchanged at the one out of
    /// it; floats written through the twin's UAV – the material IDs a retail title writes there,
    /// 1.0, values outside 0..1, NaN – land as the GPU's own clear stores the same depth, rounded
    /// to nearest; the stencil plane is left alone. The transitions bring every resource back.
    #[test]
    fn gpu_a_d24s8_depth_plane_goes_through_its_twin_exactly() {
        unsafe {
            let Some(g) = Gpu::new() else { return };
            let (depth, dsv_heap, twin) = g.depth(DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT);
            let dsv = dsv_heap.GetCPUDescriptorHandleForHeapStart();
            let (res, write, uav) = (depth.as_raw() as usize, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            super::super::enhanced_barrier::forget(res);
            forget(res);

            // the transitions: the depth buffer and the twin back where they were, the scratch buffer in COMMON
            let seen = Transitions::default();
            assert!(sync(res, true, &seen));
            let scratch = scratch_of(&depth).unwrap().as_raw() as usize;
            let (d, t, s) = (res, twin.as_raw() as usize, scratch);
            let (src, dst, common) = (D3D12_RESOURCE_STATE_COPY_SOURCE.0, D3D12_RESOURCE_STATE_COPY_DEST.0, 0);
            assert_eq!(seen.0.take(), vec![(s, common, dst), (d, write.0, src), (s, dst, uav.0), (d, src, write.0), (s, uav.0, src), (t, uav.0, dst), (s, src, common), (t, dst, uav.0)]);

            // (upload buffers live until their list has run)
            let uploads = std::cell::RefCell::new(Vec::new());
            // depths of every size (and a high byte the depth plane does not hold)
            let mut ks: Vec<u32> = (0..2048u32).map(|i| (i.wrapping_mul(0x9E37_79B1) >> 8) & 0xFF_FFFF).collect();
            ks[..10].copy_from_slice(&[0, 1, 2, 1280, 0x7F_FFFF, 0x80_0000, 0x80_0001, 0xFF_FFFE, 0xFF_FFFF, 0x55_5555]);
            let into = |l: &ID3D12GraphicsCommandList| {
                uploads.borrow_mut().push(g.upload(l, &depth, write, &ks.iter().map(|k| k | 0xAB00_0000).collect::<Vec<_>>()));
                l.ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_STENCIL, 0.0, 7, None);
                assert!(sync(res, true, l));
            };
            let floats = words(&g.read(&into, &twin, 0, uav, 4));
            for (&k, &f) in ks.iter().zip(&floats) {
                let (f, exact) = (f32::from_bits(f), k as f64 / 16_777_215.0);
                let nearest = f == 0.0 || [f32::from_bits(f.to_bits() - 1), f32::from_bits(f.to_bits() + 1)].iter().all(|&o| (o as f64 - exact).abs() >= (f as f64 - exact).abs());
                assert!(nearest && (f == 0.0) == (k == 0), "depth {k:#x} reached the twin as {f}, not the float nearest to {exact}");
            }
            let back = words(&g.read(&|l| assert!(sync(res, false, l)), &depth, 0, write, 4));
            let back: Vec<u32> = back.iter().map(|w| w & 0xFF_FFFF).collect();
            assert_eq!(back, ks, "the depths came back unchanged");

            // floats written through the UAV; the first 8 columns: material IDs as the title's
            // tiled lighting draws them (ID · 1/65535), the GPU's own clear of them next to them
            let id = |c: u32| (c * 17) as f32 * f32::from_bits(0x3780_0080);
            let mut fs: Vec<f32> = (0..2048u32).map(|i| (i.wrapping_mul(0x9E37_79B1) >> 9) as f32 / 8_388_607.0).collect();
            fs[8..18].copy_from_slice(&[0.0, 1.0, 0.5, -1.0, 2.0, f32::NAN, f32::INFINITY, -f32::INFINITY, 1e-40, 0.999_999_9]);
            for (i, f) in fs.iter_mut().enumerate().filter(|(i, _)| i % 64 < 8) {
                *f = id(i as u32 % 64);
            }
            let reference = g.depth(DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT);
            let out = |l: &ID3D12GraphicsCommandList| {
                uploads.borrow_mut().push(g.upload(l, &twin, uav, &fs.iter().map(|f| f.to_bits()).collect::<Vec<_>>()));
                for c in 0..8 {
                    let r = windows::Win32::Foundation::RECT { left: c, top: 0, right: c + 1, bottom: 32 };
                    l.ClearDepthStencilView(reference.1.GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, id(c as u32), 0, Some(&[r]));
                }
                assert!(sync(res, false, l));
            };
            let got: Vec<u32> = words(&g.read(&out, &depth, 0, write, 4)).iter().map(|w| w & 0xFF_FFFF).collect();
            let gpu: Vec<u32> = words(&g.read(&|_| {}, &reference.0, 0, write, 4)).iter().map(|w| w & 0xFF_FFFF).collect();
            for (i, (&f, &k)) in fs.iter().zip(&got).enumerate() {
                assert_eq!(k, unorm24(f), "{f} written through the UAV landed as {k:#x}");
                if i % 64 < 8 {
                    assert_eq!(k, gpu[i], "material ID {} landed as {k:#x}, the GPU stores {:#x} for that depth", i % 64 * 17, gpu[i]);
                }
            }
            assert_eq!(got[5], 5 * 17 * 256, "ID 85: 85 · 256");
            let stencil = g.read(&|_| {}, &depth, 1, write, 1);
            assert!(stencil.iter().all(|&s| s == 7), "the stencil plane is left alone: {:?}", &stencil[..4]);
            assert_eq!(g.dev.GetDeviceRemovedReason(), Ok(()), "the device is still there");
        }
    }

    /// A D32S8 depth buffer's depth plane and its twin hold the same floats, bit for bit, both ways.
    #[test]
    fn gpu_a_d32s8_depth_plane_and_its_twin_hold_the_same_bits() {
        unsafe {
            let Some(g) = Gpu::new() else { return };
            let (depth, dsv_heap, twin) = g.depth(DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
            let (res, write, uav) = (depth.as_raw() as usize, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            super::super::enhanced_barrier::forget(res);
            forget(res);
            let into = |l: &ID3D12GraphicsCommandList| {
                l.ClearDepthStencilView(dsv_heap.GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.375, 9, None);
                assert!(sync(res, true, l));
            };
            assert!(words(&g.read(&into, &twin, 0, uav, 4)).iter().all(|&w| w == 0.375f32.to_bits()), "the twin has the depth buffer's 0.375");
            let fs: Vec<u32> = (0..2048u32).map(|i| (i as f32 / 2047.0).to_bits() ^ (i & 1)).collect();
            // (the upload buffer lives until the list has run)
            let upload = std::cell::RefCell::new(None);
            let out = |l: &ID3D12GraphicsCommandList| {
                *upload.borrow_mut() = Some(g.upload(l, &twin, uav, &fs));
                assert!(sync(res, false, l));
            };
            assert_eq!(words(&g.read(&out, &depth, 0, write, 4)), fs, "the depth plane has the twin's floats, bit for bit");
            assert!(g.read(&|_| {}, &depth, 1, write, 1).iter().all(|&s| s == 9), "the stencil plane is left alone");
            assert_eq!(g.dev.GetDeviceRemovedReason(), Ok(()), "the device is still there");
        }
    }
}
