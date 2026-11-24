//! Stand-in formats for title textures in a format the PC device neither renders to nor writes
//! as a UAV.
//!
//! The console GPU renders to R9G9B9E5_SHAREDEXP (67); a PC device has it for sampling only. A
//! retail title in its Series S mode makes its temporal upscaler's history target that way
//! (1920x1080, ALLOW_RENDER_TARGET | ALLOW_UNORDERED_ACCESS): the PC refused it (E_INVALIDARG)
//! and the title asserted on the null resource. Such a texture is made in a stand-in format the
//! device does render to and write (`choose`, asked of the device once: `probe`), and the
//! resource is tagged with the format the title named (private data, so the tag goes with the
//! resource). Wherever the title names the format again it is the stand-in's: views of a tagged
//! resource (`view_format_of`), clear values and pipelines' render target formats
//! (`device_x::pc_target_format`: no pipeline renders to the title's format on such a device),
//! footprints and resolves (`copy_locations`, `resolve_format`).
//!
//! The GPU alone writes these textures, so nothing of the title's memory is laid out for them
//! (`title_textures::register` leaves them out). R11G11B10_FLOAT has R9G9B9E5's 4 bytes a texel:
//! the same tiles on the title's video memory pages (`vram`). R16G16B16A16_FLOAT, taken when the
//! device has no typed UAV loads and stores of R11G11B10_FLOAT, is twice the size, as the
//! console's 116 / 117 floats already are on the PC (`vram`'s tiles past the console's size).

use std::ffi::c_void;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use windows::core::{Interface, GUID};
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT;

/// The title formats that get a stand-in: R9G9B9E5_SHAREDEXP.
const SOURCES: [u32; 1] = [67];
/// The stand-ins, in order of preference: R11G11B10_FLOAT, R16G16B16A16_FLOAT.
const CANDIDATES: [u32; 2] = [26, 10];

/// The device's stand-in for each of `SOURCES` (0: none – not asked yet, or not needed).
static ACTIVE: [AtomicU32; SOURCES.len()] = [AtomicU32::new(0)];
/// Stand-in textures made so far.
static MADE: AtomicU64 = AtomicU64::new(0);
/// The private data key under which a stand-in holds the format the title named (a u32).
const SOURCE: GUID = GUID::from_u128(0x8e41_d2b7_30c5_4f1a_9b6e_52a7_0d3c_67f4);

/// Whether a format with this support (`Support1`, `Support2` of FORMAT_SUPPORT) is rendered to
/// and loaded and stored through a typed UAV.
fn capable(s: (i32, i32)) -> bool {
    let s1 = D3D12_FORMAT_SUPPORT1_RENDER_TARGET.0 | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW.0;
    let s2 = D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD.0 | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE.0;
    s.0 & s1 == s1 && s.1 & s2 == s2
}

/// The stand-in a device needs for `format`, `support` being what it says of a format (None: the
/// query failed). None: `format` is not one of `SOURCES`, the device renders to and writes it
/// itself, or it has no candidate either (creation decides then, as before).
pub fn choose(format: u32, support: impl Fn(u32) -> Option<(i32, i32)>) -> Option<u32> {
    if !SOURCES.contains(&format) || capable(support(format)?) {
        return None;
    }
    CANDIDATES.iter().copied().find(|&c| support(c).is_some_and(capable))
}

/// The format a title texture of `format` and resource flags `flags` is made in when `standin` is
/// the device's for it: the stand-in with ALLOW_RENDER_TARGET or ALLOW_UNORDERED_ACCESS. One that
/// is only sampled stays what it is, the title's bytes being its texels.
pub fn texture_format(format: u32, flags: u32, standin: Option<u32>) -> u32 {
    match standin {
        Some(s) if flags & 5 != 0 => s,
        _ => format,
    }
}

/// The format of a view named `view` (0: the resource's) of a stand-in in `standin` for
/// `source`, and whether the title asked for the texels' raw bits (R32_UINT of an R9G9B9E5
/// texture): the PC has no such view of the stand-in, and its bits are not the console's.
pub fn view_format(source: u32, standin: u32, view: u32) -> (u32, bool) {
    match view {
        0 => (0, false),
        v if v == source || v == standin => (standin, false),
        _ => (standin, true),
    }
}

/// The format of a buffer footprint named `named` that is copied to or from a stand-in in
/// `standin` for `source`: the stand-in's when the texels are the same size. None: the rows are
/// not the stand-in's.
pub fn footprint_format(source: u32, standin: u32, named: u32) -> Option<u32> {
    let bytes = |f| match f {
        26 | 67 => 4,
        10 => 8,
        _ => 0,
    };
    (named == standin || (named == source && bytes(source) == bytes(standin))).then_some(standin)
}

unsafe fn support(dev: &ID3D12Device, format: u32) -> Option<(i32, i32)> {
    let mut f = D3D12_FEATURE_DATA_FORMAT_SUPPORT { Format: DXGI_FORMAT(format as i32), ..Default::default() };
    dev.CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &mut f as *mut _ as *mut c_void, std::mem::size_of_val(&f) as u32).ok()?;
    Some((f.Support1.0, f.Support2.0))
}

/// `choose` for `dev`.
pub unsafe fn choose_for(dev: &ID3D12Device, format: u32) -> Option<u32> {
    choose(format, |f| support(dev, f))
}

/// Asks the title's device (`real::create_device`) which of `SOURCES` need a stand-in.
pub(super) fn probe(dev: &ID3D12Device) {
    for (k, &format) in SOURCES.iter().enumerate() {
        let (own, standin) = unsafe { (support(dev, format), choose_for(dev, format)) };
        ACTIVE[k].store(standin.unwrap_or(0), Ordering::Release);
        let (s1, s2) = own.unwrap_or((0, 0));
        match standin {
            Some(s) => crate::xlog_info!("D3D12.X: format {format} is no render target or UAV format of this device (support {s1:#x} / {s2:#x}): the title's targets in it are made in format {s}"),
            None if own.is_some_and(capable) => {}
            None => crate::xlog_warn!("D3D12.X: format {format} is no render target or UAV format of this device (support {s1:#x} / {s2:#x}) and it has no stand-in for it"),
        }
    }
}

/// The device's stand-in for title format `format` (None: it has or needs none).
pub fn for_format(format: u32) -> Option<u32> {
    let k = SOURCES.iter().position(|&s| s == format)?;
    Some(ACTIVE[k].load(Ordering::Acquire)).filter(|&s| s != 0)
}

/// `texture_format` on the title's device.
pub fn resource_format(format: u32, flags: u32) -> u32 {
    texture_format(format, flags, for_format(format))
}

/// After a creation of the title's (`console_desc`: its description, `hr` / `ppv`: the result):
/// a texture made in a stand-in format is tagged with the format the title named.
pub(super) unsafe fn note_created(console_desc: usize, ppv: *mut *mut c_void, hr: i32) {
    if hr < 0 || console_desc == 0 || ppv.is_null() || (*ppv).is_null() || !crate::gpu::shader::readable(console_desc, 0x38) {
        return;
    }
    let u = |o: usize| ((console_desc + o) as *const u32).read_unaligned();
    let (dimension, format, flags) = (u(0), u(0x20), u(0x30));
    let Some(standin) = for_format(format).filter(|&s| dimension != D3D12_RESOURCE_DIMENSION_BUFFER.0 as u32 && texture_format(format, flags, Some(s)) == s) else { return };
    let Some(r) = ID3D12Resource::from_raw_borrowed(&*ppv) else { return };
    if r.GetDesc().Format.0 as u32 != standin || r.SetPrivateData(&SOURCE, 4, Some(&format as *const u32 as *const c_void)).is_err() {
        return;
    }
    if MADE.fetch_add(1, Ordering::AcqRel) < 8 {
        crate::xlog_info!("D3D12.X: {}x{} texture in format {format} (flags {flags:#x}) made in format {standin}: the PC neither renders to nor writes format {format}", ((console_desc + 0x10) as *const u64).read_unaligned(), u(0x18));
    }
}

/// `(the format the title named, the stand-in)` of a stand-in texture (None: `res` is none).
unsafe fn source_of(res: *mut c_void) -> Option<(u32, u32)> {
    if MADE.load(Ordering::Acquire) == 0 {
        return None;
    }
    let r = ID3D12Resource::from_raw_borrowed(&res)?;
    let format = r.GetDesc().Format.0 as u32;
    if !ACTIVE.iter().any(|a| a.load(Ordering::Acquire) == format) {
        return None;
    }
    let (mut source, mut size) = (0u32, 4u32);
    r.GetPrivateData(&SOURCE, &mut size, Some(&mut source as *mut u32 as *mut c_void)).ok()?;
    Some((source, format))
}

/// The format a view of `res` that the title names `view` has (None: `res` is no stand-in, the
/// view is as the title describes it). A view of the raw bits is logged once.
pub(super) unsafe fn view_format_of(res: *mut c_void, view: u32) -> Option<u32> {
    let (source, standin) = source_of(res)?;
    let (f, raw) = view_format(source, standin, view);
    static N: AtomicU64 = AtomicU64::new(0);
    if raw && N.fetch_add(1, Ordering::Relaxed) == 0 {
        crate::xlog_warn!("D3D12.X: a view in format {view} of a format {source} texture that is made in format {standin} on the PC – a view in format {standin}: the title does not read the bits it would on the console (logged once)");
    }
    Some(f)
}

/// A copy the PC cannot make as the title asked (first 4).
fn refused(what: std::fmt::Arguments) {
    static N: AtomicU64 = AtomicU64::new(0);
    if N.fetch_add(1, Ordering::Relaxed) < 4 {
        crate::xlog_warn!("D3D12.X: {what} – not copied: the two do not hold the same bits on the PC");
    }
}

/// Whether textures `dst` and `src` are copied one to the other as on the console: not a
/// stand-in and a texture in another format (the title's own format, sampled only).
pub(super) unsafe fn copy_between(dst: usize, src: usize) -> bool {
    let (Some((source, standin)), other) = (match (source_of(dst as *mut c_void), source_of(src as *mut c_void)) {
        (Some(d), _) => (Some(d), src),
        (None, s) => (s, dst),
    }) else {
        return true;
    };
    let other = other as *mut c_void;
    let format = ID3D12Resource::from_raw_borrowed(&other).map_or(0, |r| r.GetDesc().Format.0 as u32);
    if format != standin {
        refused(format_args!("a copy between a format {source} texture made in format {standin} and a texture in format {format}"));
    }
    format == standin
}

/// The locations of a CopyTextureRegion with a stand-in on one side: a footprint named in the
/// title's format takes the stand-in's. false: the copy is left out (`copy_between`, or a
/// footprint whose rows are not the stand-in's).
pub(super) unsafe fn copy_locations(dst: &mut D3D12_TEXTURE_COPY_LOCATION, src: &mut D3D12_TEXTURE_COPY_LOCATION) -> bool {
    if MADE.load(Ordering::Acquire) == 0 {
        return true;
    }
    let res = |l: &D3D12_TEXTURE_COPY_LOCATION| std::mem::transmute_copy::<_, usize>(&l.pResource);
    let footprint = |l: &D3D12_TEXTURE_COPY_LOCATION| l.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    let (rows, texture) = match (footprint(dst), footprint(src)) {
        (false, false) => return copy_between(res(dst), res(src)),
        (true, false) => (dst, res(src)),
        (false, true) => (src, res(dst)),
        (true, true) => return true,
    };
    let Some((source, standin)) = source_of(texture as *mut c_void) else { return true };
    let named = rows.Anonymous.PlacedFootprint.Footprint.Format.0 as u32;
    match footprint_format(source, standin, named) {
        Some(f) => {
            static N: AtomicU64 = AtomicU64::new(0);
            if f != named && N.fetch_add(1, Ordering::Relaxed) == 0 {
                crate::xlog_warn!("D3D12.X: a copy between a buffer and a format {source} texture that is made in format {standin} on the PC: the buffer's texels are the stand-in's, not the console's (logged once)");
            }
            rows.Anonymous.PlacedFootprint.Footprint.Format = DXGI_FORMAT(f as i32);
            true
        }
        None => {
            refused(format_args!("a copy between a buffer of format {named} texels and a format {source} texture made in format {standin}"));
            false
        }
    }
}

/// The format of a resolve into `dst` that the title names `format`: a stand-in's own.
pub(super) unsafe fn resolve_format(dst: usize, format: i32) -> i32 {
    source_of(dst as *mut c_void).map_or(format, |(_, standin)| standin as i32)
}

#[cfg(test)]
mod tests {
    use super::*;
    use windows::Win32::Graphics::Dxgi::Common::DXGI_SAMPLE_DESC;

    /// Tests that set the device's stand-in (`ACTIVE`) run one at a time.
    static ACTIVE_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    const ALL: (i32, i32) = (D3D12_FORMAT_SUPPORT1_RENDER_TARGET.0 | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW.0, D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD.0 | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE.0);
    /// A format that is only sampled (what a PC device says of R9G9B9E5).
    const SAMPLED: (i32, i32) = (D3D12_FORMAT_SUPPORT1_TEXTURE2D.0 | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE.0, 0);

    /// A console texture description: 64x32, one mip, `format`, `flags`.
    fn console_desc(format: u32, flags: u32) -> [u8; 0x38] {
        let mut x = [0u8; 0x38];
        x[0..4].copy_from_slice(&(D3D12_RESOURCE_DIMENSION_TEXTURE2D.0 as u32).to_le_bytes());
        x[0x10..0x18].copy_from_slice(&64u64.to_le_bytes());
        x[0x18..0x1C].copy_from_slice(&32u32.to_le_bytes());
        x[0x1C..0x1E].copy_from_slice(&1u16.to_le_bytes());
        x[0x1E..0x20].copy_from_slice(&1u16.to_le_bytes());
        x[0x20..0x24].copy_from_slice(&format.to_le_bytes());
        x[0x24..0x28].copy_from_slice(&1u32.to_le_bytes());
        x[0x30..0x34].copy_from_slice(&flags.to_le_bytes());
        x
    }

    /// The device's support decides: R11G11B10_FLOAT when it is rendered to and loaded and stored
    /// through UAVs, R16G16B16A16_FLOAT when it lacks any of the three, nothing when the device
    /// has the title's format itself, when no candidate fits, or when the query fails.
    #[test]
    fn the_stand_in_follows_the_device_support() {
        let dev = |r9: Option<(i32, i32)>, r11: (i32, i32), r16: (i32, i32)| {
            move |f: u32| match f {
                67 => r9,
                26 => Some(r11),
                10 => Some(r16),
                _ => None,
            }
        };
        assert_eq!(choose(67, dev(Some(SAMPLED), ALL, ALL)), Some(26));
        let no_load = (ALL.0, D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE.0);
        let no_store = (ALL.0, D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD.0);
        let no_target = (D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW.0, ALL.1);
        for r11 in [no_load, no_store, no_target, SAMPLED] {
            assert_eq!(choose(67, dev(Some(SAMPLED), r11, ALL)), Some(10), "{r11:x?}");
        }
        // rendered to but not written, or written but not rendered to: a stand-in all the same
        assert_eq!(choose(67, dev(Some(no_load), ALL, ALL)), Some(26));
        assert_eq!(choose(67, dev(Some(no_target), ALL, ALL)), Some(26));
        assert_eq!(choose(67, dev(Some(ALL), ALL, ALL)), None, "the device has the format");
        assert_eq!(choose(67, dev(Some(SAMPLED), SAMPLED, no_load)), None, "no candidate");
        assert_eq!(choose(67, dev(None, ALL, ALL)), None, "the query failed");
        assert_eq!(choose(26, dev(Some(SAMPLED), SAMPLED, ALL)), None, "not a format with a stand-in");
    }

    /// Only targets and UAV textures are made in the stand-in; views of one are in its format,
    /// a view of the raw bits too (and is told apart); footprints follow when the texels fit.
    #[test]
    fn textures_views_and_footprints_take_the_stand_in() {
        for (flags, f) in [(0x30_0005, 26), (1, 26), (4, 26), (0, 67), (0x30_0000, 67), (2, 67)] {
            assert_eq!(texture_format(67, flags, Some(26)), f, "flags {flags:#x}");
        }
        assert_eq!(texture_format(67, 5, None), 67);
        assert_eq!(view_format(67, 26, 67), (26, false));
        assert_eq!(view_format(67, 26, 26), (26, false));
        assert_eq!(view_format(67, 26, 0), (0, false), "the resource's own format");
        assert_eq!(view_format(67, 26, 42), (26, true), "R32_UINT: the raw bits");
        assert_eq!(view_format(67, 10, 67), (10, false));
        assert_eq!(footprint_format(67, 26, 67), Some(26));
        assert_eq!(footprint_format(67, 26, 26), Some(26));
        assert_eq!(footprint_format(67, 10, 67), None, "4-byte texels for an 8-byte format");
        assert_eq!(footprint_format(67, 10, 10), Some(10));
        assert_eq!(footprint_format(67, 26, 42), None);
    }

    /// With a stand-in on the device, the title's description, clear value and pipeline formats
    /// name it; without one they are as before.
    #[test]
    fn descriptions_clears_and_pipelines_name_the_stand_in() {
        use super::super::device_x::{pc_clear, pc_resource_desc, pc_target_format};
        let _g = ACTIVE_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let format = |d: [u8; 0x38]| u32::from_le_bytes(d[0x20..0x24].try_into().unwrap());
        let before = ACTIVE[0].swap(26, Ordering::AcqRel);
        assert_eq!(format(pc_resource_desc(&console_desc(67, 0x30_0005))), 26);
        assert_eq!(format(pc_resource_desc(&console_desc(67, 0))), 67, "a sampled texture is the title's bytes");
        assert_eq!(format(pc_resource_desc(&console_desc(26, 5))), 26);
        assert_eq!(format(pc_resource_desc(&console_desc(86, 5))), 28, "the other target formats as before");
        assert_eq!(format(pc_resource_desc(&console_desc(40, 2))), 39);
        assert_eq!((pc_target_format(67), pc_target_format(26), pc_target_format(86), pc_target_format(118)), (26, 26, 28, 45));
        let mut clear = [0u8; 24];
        clear[0..4].copy_from_slice(&67u32.to_le_bytes());
        clear[4..8].copy_from_slice(&1.0f32.to_le_bytes());
        let mut buf = [0u8; 24];
        let p = unsafe { pc_clear(clear.as_ptr() as *const c_void, &mut buf) } as *const [u8; 24];
        let pc = unsafe { p.read() };
        assert_eq!((u32::from_le_bytes(pc[0..4].try_into().unwrap()), &pc[4..]), (26, &clear[4..]), "the colour stays");
        ACTIVE[0].store(0, Ordering::Release);
        assert_eq!((format(pc_resource_desc(&console_desc(67, 5))), pc_target_format(67)), (67, 67));
        ACTIVE[0].store(before, Ordering::Release);
    }

    fn transition(res: &ID3D12Resource, before: D3D12_RESOURCE_STATES, after: D3D12_RESOURCE_STATES) -> D3D12_RESOURCE_BARRIER {
        let t = D3D12_RESOURCE_TRANSITION_BARRIER { pResource: unsafe { std::mem::transmute_copy(res) }, Subresource: D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, StateBefore: before, StateAfter: after };
        D3D12_RESOURCE_BARRIER { Type: D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, Flags: D3D12_RESOURCE_BARRIER_FLAG_NONE, Anonymous: D3D12_RESOURCE_BARRIER_0 { Transition: std::mem::ManuallyDrop::new(t) } }
    }

    /// On a GPU: the title's R9G9B9E5 target (ALLOW_RENDER_TARGET | ALLOW_UNORDERED_ACCESS) is
    /// made in the device's stand-in and tagged, its views named R9G9B9E5 are the stand-in's, and
    /// what is rendered to it and written through its UAV is read back. R11G11B10_FLOAT takes
    /// R9G9B9E5's bytes (the same tiles on `vram`'s pages).
    #[test]
    fn gpu_r9g9b9e5_target_is_made_rendered_and_written_in_its_stand_in() {
        use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
        unsafe {
            let mut dev: Option<ID3D12Device> = None;
            if D3D12CreateDevice(None, D3D_FEATURE_LEVEL_11_0, &mut dev).is_err() {
                crate::gpu::recomp::runner::skip_test("no GPU");
                return;
            }
            let dev = dev.unwrap();
            let Some(standin) = choose_for(&dev, 67) else {
                assert!(support(&dev, 67).is_some_and(capable), "no stand-in for R9G9B9E5 on a device that does not render to it");
                return;
            };
            let _g = ACTIVE_LOCK.lock().unwrap_or_else(|e| e.into_inner());
            let before = ACTIVE[0].swap(standin, Ordering::AcqRel);
            let (w, h) = (64u32, 32u32);
            let x = console_desc(67, 0x30_0005);
            let d: D3D12_RESOURCE_DESC = std::ptr::read_unaligned(super::super::device_x::pc_resource_desc(&x).as_ptr() as *const _);
            assert_eq!((d.Format.0 as u32, d.Flags.0), (standin, 5));
            let default = D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() };
            let mut target: Option<ID3D12Resource> = None;
            // refused in the title's format (what the title got), made in the stand-in
            let own = D3D12_RESOURCE_DESC { Format: DXGI_FORMAT(67), ..d };
            assert!(dev.CreateCommittedResource(&default, D3D12_HEAP_FLAG_NONE, &own, D3D12_RESOURCE_STATE_RENDER_TARGET, None, &mut target).is_err());
            dev.CreateCommittedResource(&default, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, None, &mut target).unwrap();
            let target = target.unwrap();
            let mut raw = target.as_raw();
            note_created(console_desc(26, 5).as_ptr() as usize, &mut raw, 0);
            assert_eq!(source_of(raw), None, "a texture the title named in that format itself is no stand-in");
            note_created(x.as_ptr() as usize, &mut raw, 0);
            assert_eq!(source_of(raw), Some((67, standin)));
            assert_eq!((view_format_of(raw, 67), view_format_of(raw, 0), view_format_of(raw, 42)), (Some(standin), Some(0), Some(standin)));
            assert_eq!(resolve_format(raw as usize, 67), standin as i32);

            // the same bytes as the title's format when the stand-in is R11G11B10_FLOAT
            let bytes = |format: u32| {
                let plain = D3D12_RESOURCE_DESC { Format: DXGI_FORMAT(format as i32), Width: 1920, Height: 1080, Flags: D3D12_RESOURCE_FLAG_NONE, ..d };
                let mut total = 0u64;
                dev.GetCopyableFootprints(&plain, 0, 1, 0, None, None, None, Some(&mut total));
                total
            };
            assert!(standin != 26 || bytes(26) == bytes(67), "{} / {}", bytes(26), bytes(67));

            let heap = |ty, flags| -> ID3D12DescriptorHeap { dev.CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC { Type: ty, NumDescriptors: 1, Flags: flags, NodeMask: 0 }).unwrap() };
            let rtv_heap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
            let (gpu_heap, cpu_heap) = (heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE), heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE));
            // the title's views name R9G9B9E5
            let view = DXGI_FORMAT(view_format_of(raw, 67).unwrap() as i32);
            let rtv = rtv_heap.GetCPUDescriptorHandleForHeapStart();
            dev.CreateRenderTargetView(&target, Some(&D3D12_RENDER_TARGET_VIEW_DESC { Format: view, ViewDimension: D3D12_RTV_DIMENSION_TEXTURE2D, ..Default::default() }), rtv);
            let uav = D3D12_UNORDERED_ACCESS_VIEW_DESC { Format: view, ViewDimension: D3D12_UAV_DIMENSION_TEXTURE2D, ..Default::default() };
            dev.CreateUnorderedAccessView(&target, None, Some(&uav), gpu_heap.GetCPUDescriptorHandleForHeapStart());
            dev.CreateUnorderedAccessView(&target, None, Some(&uav), cpu_heap.GetCPUDescriptorHandleForHeapStart());

            let texel = if standin == 26 { 4 } else { 8 };
            let pitch = (w * texel).next_multiple_of(256);
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
            let readback = D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_READBACK, ..Default::default() };
            let q: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC { Type: D3D12_COMMAND_LIST_TYPE_DIRECT, ..Default::default() }).unwrap();
            let fence: ID3D12Fence = dev.CreateFence(0, D3D12_FENCE_FLAG_NONE).unwrap();
            // one list: `record` (the target in `state`), then a copy to a readback buffer whose
            // footprint the title names R9G9B9E5 – every texel's bytes
            let run = |record: &dyn Fn(&ID3D12GraphicsCommandList), state: D3D12_RESOURCE_STATES, value: u64| -> Vec<Vec<u8>> {
                let alloc: ID3D12CommandAllocator = dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT).unwrap();
                let list: ID3D12GraphicsCommandList = dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &alloc, None).unwrap();
                record(&list);
                let mut rb: Option<ID3D12Resource> = None;
                dev.CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST, None, &mut rb).unwrap();
                let rb = rb.unwrap();
                list.ResourceBarrier(&[transition(&target, state, D3D12_RESOURCE_STATE_COPY_SOURCE)]);
                let named = if standin == 26 { 67 } else { standin as i32 };
                let footprint = D3D12_PLACED_SUBRESOURCE_FOOTPRINT { Offset: 0, Footprint: D3D12_SUBRESOURCE_FOOTPRINT { Format: DXGI_FORMAT(named), Width: w, Height: h, Depth: 1, RowPitch: pitch } };
                let mut to = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(&rb), Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { PlacedFootprint: footprint } };
                let mut from = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(&target), Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: 0 } };
                assert!(copy_locations(&mut to, &mut from));
                assert_eq!(to.Anonymous.PlacedFootprint.Footprint.Format.0 as u32, standin);
                list.CopyTextureRegion(&to, 0, 0, 0, &from, None);
                list.ResourceBarrier(&[transition(&target, D3D12_RESOURCE_STATE_COPY_SOURCE, state)]);
                list.Close().unwrap();
                q.ExecuteCommandLists(&[Some(list.cast().unwrap())]);
                q.Signal(&fence, value).unwrap();
                while fence.GetCompletedValue() < value {
                    std::thread::yield_now();
                }
                let mut p: *mut c_void = std::ptr::null_mut();
                rb.Map(0, None, Some(&mut p)).unwrap();
                let rows = std::slice::from_raw_parts(p as *const u8, (pitch * h) as usize);
                let px = (0..(w * h) as usize).map(|k| rows[k / w as usize * pitch as usize + k % w as usize * texel as usize..][..texel as usize].to_vec()).collect();
                rb.Unmap(0, None);
                std::mem::forget((to, from)); // borrowed resource pointers
                px
            };
            // (r, g, b) as the stand-in holds them: 11 / 11 / 10-bit floats in one dword, or half floats
            let packed = |c: [u32; 3], halves: [u16; 3]| -> Vec<u8> {
                if standin == 26 {
                    (c[0] | c[1] << 11 | c[2] << 22).to_le_bytes().to_vec()
                } else {
                    halves.iter().flat_map(|v| v.to_le_bytes()).chain([0x00, 0x3C]).collect()
                }
            };
            let render = |l: &ID3D12GraphicsCommandList| l.ClearRenderTargetView(rtv, &[1.0, 0.5, 0.25, 1.0], None);
            let rendered = run(&render, D3D12_RESOURCE_STATE_RENDER_TARGET, 1);
            let want = packed([0x3C0, 0x380, 0x1A0], [0x3C00, 0x3800, 0x3400]);
            assert!(rendered.iter().all(|p| *p == want), "rendered (1, 0.5, 0.25): {:x?}, not {want:x?}", rendered[0]);
            let write = |l: &ID3D12GraphicsCommandList| {
                l.ResourceBarrier(&[transition(&target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)]);
                l.SetDescriptorHeaps(&[Some(gpu_heap.clone())]);
                l.ClearUnorderedAccessViewFloat(gpu_heap.GetGPUDescriptorHandleForHeapStart(), cpu_heap.GetCPUDescriptorHandleForHeapStart(), &target, &[2.0, 1.0, 0.5, 1.0], &[]);
            };
            let written = run(&write, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 2);
            let want = packed([0x400, 0x3C0, 0x1C0], [0x4000, 0x3C00, 0x3800]);
            assert!(written.iter().all(|p| *p == want), "written (2, 1, 0.5): {:x?}, not {want:x?}", written[0]);
            assert_eq!(dev.GetDeviceRemovedReason(), Ok(()), "the device is still there");
            ACTIVE[0].store(before, Ordering::Release);
        }
    }
}
