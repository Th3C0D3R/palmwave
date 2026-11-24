//! `XCL_RT_DUMP=1`: the targets a frame drew into, written to the logs folder at its present
//! (`rt_<frame>_<k>_<kind>_<format>_<w>x<h>.png`) – what the GPU drew before the final image:
//! a G-buffer or a depth buffer shows the world where later passes leave the presented frame
//! black. The kinds: `rtv` (a color render target the title bound), `dsv` (a depth-stencil
//! target it bound: the depth plane), `uav` (a 2D texture it moved into or out of
//! UNORDERED_ACCESS – what its compute passes can have written; a bindless title's
//! descriptor indices do not say which UAVs a dispatch writes). Depth and ID targets come
//! first, then the largest; `XCL_RT_DUMP_MAX=n` targets a frame at most (24).
//!
//! Frames are chosen as for `XCL_FRAME_DUMP`: every 120th (`XCL_RT_DUMP=n`, n > 1: every
//! n-th), and every one from `XCL_RT_DUMP_FROM=n` on (unset: `XCL_FRAME_DUMP_FROM`).
//!
//! The copy needs each target's state at the present. A bound target is in RENDER_TARGET or
//! DEPTH_WRITE; the transitions the title records are followed list by list, in submission
//! order (as `title_textures` does for the textures it uploads into).

use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::{Mutex, OnceLock};

use windows::core::Interface;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::*;

/// Targets smaller than this are not dumped (shadow tiles, luminance chains, …).
const MIN_SIZE: (u64, u32) = (640, 360);
/// Targets dumped per frame at most, without `XCL_RT_DUMP_MAX`.
const MAX_TARGETS: usize = 24;

/// `every` and `from` as `present::dump_due` takes them, and the targets a frame at most.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Settings {
    every: u64,
    from: u64,
    max: usize,
}

/// None: the dump is off.
fn schedule(rt: Option<&str>, rt_from: Option<&str>, frame_from: Option<&str>, max: Option<&str>) -> Option<Settings> {
    let rt = rt?.trim();
    if rt.is_empty() || rt == "0" {
        return None;
    }
    Some(Settings {
        every: rt.parse().ok().filter(|&n| n > 1).unwrap_or(120),
        from: rt_from.or(frame_from).and_then(|v| v.trim().parse().ok()).unwrap_or(u64::MAX),
        max: max.and_then(|v| v.trim().parse().ok()).filter(|&n| n > 0).unwrap_or(MAX_TARGETS),
    })
}

fn settings() -> Option<Settings> {
    static S: OnceLock<Option<Settings>> = OnceLock::new();
    *S.get_or_init(|| {
        let var = |name: &str| std::env::var(name).ok();
        schedule(var("XCL_RT_DUMP").as_deref(), var("XCL_RT_DUMP_FROM").as_deref(), var("XCL_FRAME_DUMP_FROM").as_deref(), var("XCL_RT_DUMP_MAX").as_deref())
    })
}

pub(super) fn enabled() -> bool {
    settings().is_some()
}

/// How the title used a target.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum View {
    Rtv,
    Dsv,
    Uav,
}

impl View {
    pub(super) fn name(self) -> &'static str {
        match self {
            View::Rtv => "rtv",
            View::Dsv => "dsv",
            View::Uav => "uav",
        }
    }
}

/// What a PC list recorded, in order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Note {
    /// OMSetRenderTargets named this resource through a view that wants this state. `sure`:
    /// the view writes, so the resource is in that state; a read-only depth view is content
    /// with others, and says nothing of a target already followed.
    Used(usize, View, D3D12_RESOURCE_STATES, bool),
    /// a transition of subresource 0 of this resource to this state (`uav`: of a 2D texture
    /// of `MIN_SIZE`, from or to UNORDERED_ACCESS)
    State(usize, D3D12_RESOURCE_STATES, bool),
}

/// A target of the frame, and the state of its subresource 0.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Target {
    res: usize,
    view: View,
    state: D3D12_RESOURCE_STATES,
}

/// PC command list → what it recorded since it was made or last reset. It applies when the
/// list is submitted.
static NOTES: Mutex<Option<HashMap<usize, Vec<Note>>>> = Mutex::new(None);
/// The targets of the lists submitted since the last present, in the order they came up.
static FRAME: Mutex<Vec<Target>> = Mutex::new(Vec::new());
/// The resources noted during this frame and the one before, each with a reference: a
/// target the title releases before the present is not looked at through a dead pointer.
static ALIVE: Mutex<Option<[HashMap<usize, ID3D12Resource>; 2]>> = Mutex::new(None);
/// depth-stencil view handle → its resource (with a reference, as `present` keeps the render
/// targets'), and whether the view only reads the depth
static DSVS: Mutex<Option<HashMap<usize, (ID3D12Resource, bool)>>> = Mutex::new(None);

fn note(list: usize, n: Note) {
    NOTES.lock().unwrap().get_or_insert_with(HashMap::new).entry(list).or_default().push(n);
}

unsafe fn keep_alive(res: usize) {
    let mut g = ALIVE.lock().unwrap();
    let alive = g.get_or_insert_with(Default::default);
    if !alive[0].contains_key(&res) {
        let raw = res as *mut c_void;
        if let Some(r) = ID3D12Resource::from_raw_borrowed(&raw) {
            alive[0].insert(res, r.clone());
        }
    }
}

/// The title made the depth-stencil view `handle` of `res` (null: of nothing).
pub(super) unsafe fn note_dsv_view(handle: usize, res: *mut c_void, read_only: bool) {
    let new = ID3D12Resource::from_raw_borrowed(&res).map(|r| (r.clone(), read_only));
    let mut g = DSVS.lock().unwrap();
    let m = g.get_or_insert_with(HashMap::new);
    let old = match new {
        Some(v) => m.insert(handle, v),
        None => m.remove(&handle),
    };
    drop(g);
    drop(old);
}

/// The title bound `res` as a color render target on PC list `list`.
pub(super) unsafe fn note_bound(list: usize, res: usize) {
    keep_alive(res);
    note(list, Note::Used(res, View::Rtv, D3D12_RESOURCE_STATE_RENDER_TARGET, true));
}

/// The title bound the depth-stencil view `dsv` on PC list `list`.
pub(super) unsafe fn note_depth_bound(list: usize, dsv: usize) {
    let found = DSVS.lock().unwrap().as_ref().and_then(|m| m.get(&dsv)).map(|(r, read_only)| (r.as_raw() as usize, *read_only));
    let Some((res, read_only)) = found else { return };
    keep_alive(res);
    let state = if read_only { D3D12_RESOURCE_STATE_DEPTH_READ } else { D3D12_RESOURCE_STATE_DEPTH_WRITE };
    note(list, Note::Used(res, View::Dsv, state, !read_only));
}

/// The title recorded a transition of `res` from `before` to `after` on PC list `list`
/// (`begin_only`: the first half of a split barrier, which changes nothing yet).
pub(super) unsafe fn note_transition(list: usize, res: usize, subresource: u32, before: D3D12_RESOURCE_STATES, after: D3D12_RESOURCE_STATES, begin_only: bool) {
    if begin_only || (subresource != 0 && subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        return;
    }
    let raw = res as *mut c_void;
    let uav = (before.0 | after.0) & D3D12_RESOURCE_STATE_UNORDERED_ACCESS.0 != 0
        && ID3D12Resource::from_raw_borrowed(&raw).is_some_and(|r| {
            let d = r.GetDesc();
            d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width >= MIN_SIZE.0 && d.Height >= MIN_SIZE.1
        });
    if uav {
        keep_alive(res);
    }
    note(list, Note::State(res, after, uav));
}

/// PC list `list` starts a new recording (made, reset, released).
pub(super) fn list_reset(list: usize) {
    if enabled() {
        if let Some(m) = NOTES.lock().unwrap().as_mut() {
            m.remove(&list);
        }
    }
}

/// `frame` after a list that recorded `notes` has run.
fn apply(frame: &mut Vec<Target>, notes: &[Note]) {
    for &n in notes {
        let (res, view, state, sure) = match n {
            Note::Used(r, v, s, sure) => (r, Some(v), s, sure),
            Note::State(r, s, uav) => (r, uav.then_some(View::Uav), s, true),
        };
        match (frame.iter_mut().find(|t| t.res == res), view) {
            (Some(t), _) if sure => t.state = state,
            (Some(_), _) => {}
            (None, Some(view)) => frame.push(Target { res, view, state }),
            // not a target of this frame
            (None, None) => {}
        }
    }
}

/// The PC lists `lists` were submitted, in this order.
pub(super) fn submitted(lists: &[*mut c_void]) {
    if !enabled() {
        return;
    }
    let notes = NOTES.lock().unwrap();
    let Some(m) = notes.as_ref() else { return };
    let mut frame = FRAME.lock().unwrap();
    for l in lists {
        if let Some(n) = m.get(&(*l as usize)) {
            apply(&mut frame, n);
        }
    }
}

/// `max` of the (target, first, width, height) that are `MIN_SIZE` at least: those marked
/// `first` (depth and IDs) before the others, the largest first in each (the same size: in
/// the order given).
fn select<T>(mut targets: Vec<(T, bool, u64, u32)>, max: usize) -> Vec<(T, bool, u64, u32)> {
    targets.retain(|&(_, _, w, h)| w >= MIN_SIZE.0 && h >= MIN_SIZE.1);
    targets.sort_by_key(|&(_, first, w, h)| (!first, std::cmp::Reverse(w * h as u64)));
    targets.truncate(max);
    targets
}

/// A target to copy for the dump: its description, the state it is in, how the title used it.
pub(super) struct Source {
    pub res: ID3D12Resource,
    pub desc: D3D12_RESOURCE_DESC,
    pub state: D3D12_RESOURCE_STATES,
    pub view: View,
}

/// The frame about to be presented as the `n`-th ends: the targets to copy for its dump
/// (none when this frame is not dumped).
pub(super) unsafe fn frame_targets(n: u64) -> Vec<Source> {
    let bound = std::mem::take(&mut *FRAME.lock().unwrap());
    let mut found = Vec::new();
    let settings = settings().filter(|s| super::present::dump_due(n, s.every, s.from));
    let old = {
        let mut g = ALIVE.lock().unwrap();
        let alive = g.get_or_insert_with(Default::default);
        for t in bound.iter().filter(|_| settings.is_some()) {
            let Some(res) = alive[0].get(&t.res).or_else(|| alive[1].get(&t.res)).cloned() else { continue };
            let d = res.GetDesc();
            if d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width < MIN_SIZE.0 || d.Height < MIN_SIZE.1 {
                continue;
            }
            let Some((kind, _)) = kind(d.Format, t.view == View::Dsv).filter(|_| d.SampleDesc.Count <= 1) else {
                crate::xlog_info!("rt dump: frame {n}: {} target {:#x} {}x{} skipped – format {} ({} samples) is not converted", t.view.name(), t.res, d.Width, d.Height, d.Format.0, d.SampleDesc.Count);
                continue;
            };
            let first = t.view == View::Dsv || matches!(kind, Kind::Rg32Uint | Kind::R32Uint | Kind::Float32 | Kind::Depth24 | Kind::Depth16);
            found.push((Source { res, desc: d, state: t.state, view: t.view }, first, d.Width, d.Height));
        }
        // this frame's resources stay alive through the next one: a list recorded now may be
        // submitted then
        let this = std::mem::take(&mut alive[0]);
        std::mem::replace(&mut alive[1], this)
    };
    drop(old);
    let Some(settings) = settings else { return Vec::new() };
    let chosen = select(found, settings.max);
    crate::xlog_info!("rt dump: frame {n}: {} of the {} targets of the frame are copied", chosen.len(), bound.len());
    chosen.into_iter().map(|t| t.0).collect()
}

/// How the texels of a format become colors.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Kind {
    Rgba8,
    Bgra8,
    Rgb10A2,
    R11G11B10Float,
    Rgba16Float,
    /// a normal's x and y, in -1..1
    Rg16Float,
    /// a normal's x and y, 0..1 for -1..1
    Rg16Unorm,
    R8,
    Rg8,
    /// two words, one of them likely an ID (a visibility buffer): each as an ID and as a grey
    Rg32Uint,
    /// an ID
    R32Uint,
    /// one value (a depth, a distance), as a grey: so are the next three
    Float32,
    R16Float,
    /// the depth plane of D24S8, as it is copied: 24 bits in a word
    Depth24,
    Depth16,
}

/// The conversion of `format` and its name in the file (None: not converted). `depth`: of a
/// depth-stencil target (its depth plane is what is copied). A typeless target is read as its
/// family's usual format: the views are not known here.
fn kind(format: DXGI_FORMAT, depth: bool) -> Option<(Kind, &'static str)> {
    Some(match format {
        DXGI_FORMAT_R8G8B8A8_UNORM => (Kind::Rgba8, "R8G8B8A8_UNORM"),
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB => (Kind::Rgba8, "R8G8B8A8_UNORM_SRGB"),
        DXGI_FORMAT_R8G8B8A8_TYPELESS => (Kind::Rgba8, "R8G8B8A8_TYPELESS"),
        DXGI_FORMAT_B8G8R8A8_UNORM => (Kind::Bgra8, "B8G8R8A8_UNORM"),
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB => (Kind::Bgra8, "B8G8R8A8_UNORM_SRGB"),
        DXGI_FORMAT_B8G8R8A8_TYPELESS => (Kind::Bgra8, "B8G8R8A8_TYPELESS"),
        DXGI_FORMAT_R10G10B10A2_UNORM => (Kind::Rgb10A2, "R10G10B10A2_UNORM"),
        DXGI_FORMAT_R10G10B10A2_TYPELESS => (Kind::Rgb10A2, "R10G10B10A2_TYPELESS"),
        DXGI_FORMAT_R11G11B10_FLOAT => (Kind::R11G11B10Float, "R11G11B10_FLOAT"),
        DXGI_FORMAT_R16G16B16A16_FLOAT => (Kind::Rgba16Float, "R16G16B16A16_FLOAT"),
        DXGI_FORMAT_R16G16B16A16_TYPELESS => (Kind::Rgba16Float, "R16G16B16A16_TYPELESS"),
        DXGI_FORMAT_R16G16_FLOAT => (Kind::Rg16Float, "R16G16_FLOAT"),
        DXGI_FORMAT_R16G16_TYPELESS => (Kind::Rg16Float, "R16G16_TYPELESS"),
        DXGI_FORMAT_R16G16_UNORM => (Kind::Rg16Unorm, "R16G16_UNORM"),
        DXGI_FORMAT_R8_UNORM => (Kind::R8, "R8_UNORM"),
        DXGI_FORMAT_R8_TYPELESS => (Kind::R8, "R8_TYPELESS"),
        DXGI_FORMAT_R8G8_UNORM => (Kind::Rg8, "R8G8_UNORM"),
        DXGI_FORMAT_R8G8_TYPELESS => (Kind::Rg8, "R8G8_TYPELESS"),
        DXGI_FORMAT_R32G32_UINT => (Kind::Rg32Uint, "R32G32_UINT"),
        DXGI_FORMAT_R32G32_TYPELESS => (Kind::Rg32Uint, "R32G32_TYPELESS"),
        DXGI_FORMAT_R32_UINT => (Kind::R32Uint, "R32_UINT"),
        DXGI_FORMAT_R32_FLOAT => (Kind::Float32, "R32_FLOAT"),
        DXGI_FORMAT_R32_TYPELESS => (Kind::Float32, "R32_TYPELESS"),
        DXGI_FORMAT_D32_FLOAT => (Kind::Float32, "D32_FLOAT"),
        DXGI_FORMAT_D32_FLOAT_S8X24_UINT => (Kind::Float32, "D32_FLOAT_S8X24_UINT"),
        DXGI_FORMAT_R32G8X24_TYPELESS => (Kind::Float32, "R32G8X24_TYPELESS"),
        DXGI_FORMAT_D24_UNORM_S8_UINT => (Kind::Depth24, "D24_UNORM_S8_UINT"),
        DXGI_FORMAT_R24G8_TYPELESS => (Kind::Depth24, "R24G8_TYPELESS"),
        DXGI_FORMAT_D16_UNORM => (Kind::Depth16, "D16_UNORM"),
        DXGI_FORMAT_R16_TYPELESS if depth => (Kind::Depth16, "R16_TYPELESS"),
        DXGI_FORMAT_R16_TYPELESS => (Kind::R16Float, "R16_TYPELESS"),
        DXGI_FORMAT_R16_FLOAT => (Kind::R16Float, "R16_FLOAT"),
        _ => return None,
    })
}

fn texel_bytes(kind: Kind) -> usize {
    match kind {
        Kind::R8 => 1,
        Kind::Rg8 | Kind::R16Float | Kind::Depth16 => 2,
        Kind::Rgba16Float | Kind::Rg32Uint => 8,
        _ => 4,
    }
}

/// a half float → f32
fn half(v: u16) -> f32 {
    let (e, m) = ((v >> 10 & 0x1F) as i32, (v & 0x3FF) as f32 / 1024.0);
    let a = match e {
        0 => m * 2f32.powi(-14),
        31 if m == 0.0 => f32::INFINITY,
        31 => f32::NAN,
        _ => (1.0 + m) * 2f32.powi(e - 15),
    };
    if v >> 15 != 0 { -a } else { a }
}

fn unorm8(v: f32) -> u8 {
    (v.clamp(0.0, 1.0) * 255.0 + 0.5) as u8
}

/// an HDR value for viewing: Reinhard, then a gamma of 2 (negative and NaN: black)
fn tonemap(v: f32) -> u8 {
    let v = v.max(0.0).min(65504.0);
    unorm8((v / (1.0 + v)).sqrt())
}

/// a normal's x and y (-1..1) as the usual normal-map color, z rebuilt for a unit normal
fn normal(x: f32, y: f32) -> [u8; 3] {
    let z = (1.0 - x * x - y * y).max(0.0).sqrt();
    [unorm8(x * 0.5 + 0.5), unorm8(y * 0.5 + 0.5), unorm8(z * 0.5 + 0.5)]
}

fn word(t: &[u8]) -> u32 {
    u32::from_le_bytes([t[0], t[1], t[2], t[3]])
}

/// one texel (`texel_bytes` bytes) of a color format → 8-bit RGB
fn texel(kind: Kind, t: &[u8]) -> [u8; 3] {
    let h = |k: usize| u16::from_le_bytes([t[2 * k], t[2 * k + 1]]);
    match kind {
        Kind::Rgba8 => [t[0], t[1], t[2]],
        Kind::Bgra8 => [t[2], t[1], t[0]],
        Kind::R8 => [t[0]; 3],
        Kind::Rg8 => [t[0], t[1], 0],
        Kind::Rgb10A2 => [(word(t) >> 2) as u8, (word(t) >> 12) as u8, (word(t) >> 22) as u8],
        Kind::R11G11B10Float => {
            let f = super::present::small_float;
            [tonemap(f(word(t) & 0x7FF, 6)), tonemap(f(word(t) >> 11 & 0x7FF, 6)), tonemap(f(word(t) >> 22, 5))]
        }
        Kind::Rgba16Float => [tonemap(half(h(0))), tonemap(half(h(1))), tonemap(half(h(2)))],
        Kind::Rg16Float => normal(half(h(0)), half(h(1))),
        Kind::Rg16Unorm => normal(h(0) as f32 / 65535.0 * 2.0 - 1.0, h(1) as f32 / 65535.0 * 2.0 - 1.0),
        // (shown as IDs and greys: `panels`)
        Kind::Rg32Uint | Kind::R32Uint | Kind::Float32 | Kind::R16Float | Kind::Depth24 | Kind::Depth16 => [0; 3],
    }
}

/// An ID as a color: the same ID the same color, 0 black, the others never dark.
fn id_color(id: u32) -> [u8; 3] {
    if id == 0 {
        return [0; 3];
    }
    let h = id.wrapping_mul(0x9E37_79B1);
    [(h >> 24) as u8 | 0x40, (h >> 16) as u8 | 0x40, (h >> 8) as u8 | 0x40]
}

/// the value at the start of `t` in a format shown as a grey
fn scalar(kind: Kind, t: &[u8]) -> f32 {
    match kind {
        Kind::Float32 => f32::from_bits(word(t)),
        Kind::R16Float => half(u16::from_le_bytes([t[0], t[1]])),
        Kind::Depth24 => (word(t) & 0xFF_FFFF) as f32 / 16_777_215.0,
        Kind::Depth16 => u16::from_le_bytes([t[0], t[1]]) as f32 / 65535.0,
        _ => word(t) as f32,
    }
}

/// The least and the greatest of `values`, without those a clear leaves (0, 1, the largest
/// float or word) and those that are no number. None left: 0 and 1.
fn range(values: &[f32]) -> (f32, f32) {
    let drawn = |v: &&f32| v.is_finite() && ![0.0, 1.0, f32::MAX, u32::MAX as f32].contains(*v);
    let (lo, hi) = values.iter().filter(drawn).fold((f32::MAX, f32::MIN), |(lo, hi), &v| (lo.min(v), hi.max(v)));
    if lo > hi { (0.0, 1.0) } else { (lo, hi) }
}

/// `v` as a grey between `lo` (black) and `hi` (white)
fn grey(v: f32, lo: f32, hi: f32) -> u8 {
    if hi > lo {
        unorm8((v - lo) / (hi - lo))
    } else if v >= lo {
        255
    } else {
        0
    }
}

/// What a panel of the image shows of each texel.
#[derive(Clone, Copy)]
enum Show {
    Color,
    /// the word at this byte as an ID
    Id(usize),
    /// the value at this byte as a grey between the least and the greatest drawn
    Grey(usize),
}

/// The panels of the image of a format, and how many of them side by side.
fn panels(kind: Kind) -> (&'static [Show], usize) {
    match kind {
        // which word is the ID is not known: both words, each both ways
        Kind::Rg32Uint => (&[Show::Id(0), Show::Grey(0), Show::Id(4), Show::Grey(4)], 2),
        Kind::R32Uint => (&[Show::Id(0)], 1),
        Kind::Float32 | Kind::R16Float | Kind::Depth24 | Kind::Depth16 => (&[Show::Grey(0)], 1),
        _ => (&[Show::Color], 1),
    }
}

/// Every `step`-th texel of every `step`-th row of a `w`×`h` image with rows `pitch` bytes
/// apart, as (width, height, 8-bit RGB rows from the top) of its panels.
fn convert(kind: Kind, data: &[u8], w: usize, h: usize, pitch: usize, step: usize) -> (usize, usize, Vec<u8>) {
    let bytes = texel_bytes(kind);
    let (ow, oh) = (w.div_ceil(step), h.div_ceil(step));
    let texels = move || (0..h).step_by(step).flat_map(move |y| (0..w).step_by(step).map(move |x| &data[y * pitch + x * bytes..][..bytes]));
    let (shows, across) = panels(kind);
    let drawn: Vec<Vec<u8>> = shows
        .iter()
        .map(|&show| match show {
            Show::Color => texels().flat_map(|t| texel(kind, t)).collect(),
            Show::Id(at) => texels().flat_map(|t| id_color(word(&t[at..]))).collect(),
            Show::Grey(at) => {
                let values: Vec<f32> = texels().map(|t| scalar(kind, &t[at..])).collect();
                let (lo, hi) = range(&values);
                values.iter().flat_map(|&v| [grey(v, lo, hi); 3]).collect()
            }
        })
        .collect();
    let mut rgb = Vec::with_capacity(3 * ow * oh * drawn.len());
    for row in drawn.chunks(across) {
        for y in 0..oh {
            for panel in row {
                rgb.extend_from_slice(&panel[3 * ow * y..3 * ow * (y + 1)]);
            }
        }
    }
    (across * ow, drawn.len() / across * oh, rgb)
}

fn crc32(parts: &[&[u8]]) -> u32 {
    let mut c = !0u32;
    for &b in parts.iter().copied().flatten() {
        c ^= b as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
        }
    }
    !c
}

/// 8-bit RGB rows from the top as a PNG file.
fn png(w: usize, h: usize, rgb: &[u8]) -> Vec<u8> {
    let mut raw = Vec::with_capacity((3 * w + 1) * h);
    for row in rgb.chunks_exact(3 * w) {
        raw.push(0); // filter: none
        raw.extend_from_slice(row);
    }
    let mut out = b"\x89PNG\r\n\x1a\n".to_vec();
    let mut chunk = |name: &[u8; 4], body: &[u8]| {
        out.extend_from_slice(&(body.len() as u32).to_be_bytes());
        out.extend_from_slice(name);
        out.extend_from_slice(body);
        out.extend_from_slice(&crc32(&[name, body]).to_be_bytes());
    };
    let mut header = Vec::with_capacity(13);
    header.extend_from_slice(&(w as u32).to_be_bytes());
    header.extend_from_slice(&(h as u32).to_be_bytes());
    header.extend_from_slice(&[8, 2, 0, 0, 0]); // 8 bits, RGB
    chunk(b"IHDR", &header);
    chunk(b"IDAT", &miniz_oxide::deflate::compress_to_vec_zlib(&raw, 1));
    chunk(b"IEND", &[]);
    out
}

/// The copy of target `k` of frame `n` (`rb`, laid out as `fp`) is written as an image: one
/// texel in `step` so that a panel is 960 wide at most, every texel with `XCL_FRAME_DUMP_FULL=1`.
pub(super) unsafe fn write(rb: &ID3D12Resource, fp: &D3D12_PLACED_SUBRESOURCE_FOOTPRINT, format: DXGI_FORMAT, n: u64, k: usize, view: View) {
    let Some((kind, name)) = kind(format, view == View::Dsv) else { return };
    let (w, h, pitch) = (fp.Footprint.Width as usize, fp.Footprint.Height as usize, fp.Footprint.RowPitch as usize);
    let mut p: *mut c_void = std::ptr::null_mut();
    if w == 0 || h == 0 || rb.Map(0, None, Some(&mut p)).is_err() || p.is_null() {
        return;
    }
    let data = std::slice::from_raw_parts((p as *const u8).add(fp.Offset as usize), pitch * (h - 1) + w * texel_bytes(kind));
    let step = if std::env::var_os("XCL_FRAME_DUMP_FULL").is_some() { 1 } else { w.div_ceil(960) };
    let (ow, oh, rgb) = convert(kind, data, w, h, pitch, step);
    rb.Unmap(0, None);
    let lit = rgb.chunks_exact(3).filter(|c| c.iter().map(|&v| v as u32).sum::<u32>() > 30).count();
    let Some(boot) = crate::state::boot() else { return };
    let path = boot.logs_dir().join(format!("rt_{n}_{k}_{}_{name}_{w}x{h}.png", view.name()));
    match std::fs::write(&path, png(ow, oh, &rgb)) {
        Ok(()) => crate::xlog_info!("rt dump: frame {n} target {k} written to {} ({ow}x{oh}) – {lit} of {} pixels not black", path.display(), ow * oh),
        Err(e) => crate::xlog_error!("rt dump: {}: {e}", path.display()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_dump_is_off_unless_asked_for_and_follows_the_frame_dump_start() {
        assert_eq!(schedule(None, Some("5"), Some("7"), Some("3")), None);
        assert_eq!(schedule(Some("0"), None, None, None), None);
        assert_eq!(schedule(Some("1"), None, None, None), Some(Settings { every: 120, from: u64::MAX, max: 24 }));
        assert_eq!(schedule(Some("30"), None, Some(" 1825 "), Some("0")), Some(Settings { every: 30, from: 1825, max: 24 }));
        assert_eq!(schedule(Some("1"), Some("40"), Some("1825"), Some(" 60 ")), Some(Settings { every: 120, from: 40, max: 60 }));
    }

    #[test]
    fn depth_and_id_targets_come_first_then_the_largest_of_640x360_or_more() {
        let got = select(vec![("small", true, 639, 360), ("flat", false, 4096, 359), ("hd-a", false, 1920, 1080), ("min", false, 640, 360), ("4k", false, 3840, 2160), ("depth", true, 1280, 720), ("hd-b", false, 1920, 1080), ("ids", true, 1920, 1080)], 24);
        assert_eq!(got.iter().map(|t| t.0).collect::<Vec<_>>(), ["ids", "depth", "4k", "hd-a", "hd-b", "min"]);
        let many: Vec<(u64, bool, u64, u32)> = (0..30).map(|k| (k, k == 3, 640 + k, 360)).collect();
        assert_eq!(select(many.clone(), 24).len(), 24);
        assert_eq!(select(many, 4).iter().map(|t| t.0).collect::<Vec<_>>(), [3, 29, 28, 27]);
    }

    #[test]
    fn a_frame_holds_its_targets_in_the_states_the_lists_leave() {
        const SHADER: D3D12_RESOURCE_STATES = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        const TARGET: D3D12_RESOURCE_STATES = D3D12_RESOURCE_STATE_RENDER_TARGET;
        const UAV: D3D12_RESOURCE_STATES = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        const READ: D3D12_RESOURCE_STATES = D3D12_RESOURCE_STATE_DEPTH_READ;
        const WRITE: D3D12_RESOURCE_STATES = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        let t = |res, view, state| Target { res, view, state };
        let mut frame = Vec::new();
        // a transition before the first bind is not this frame's; the ones after it are
        apply(&mut frame, &[Note::State(1, SHADER, false), Note::Used(2, View::Rtv, TARGET, true), Note::Used(1, View::Rtv, TARGET, true), Note::State(2, SHADER, false)]);
        assert_eq!(frame, [t(2, View::Rtv, SHADER), t(1, View::Rtv, TARGET)]);
        apply(&mut frame, &[Note::State(1, D3D12_RESOURCE_STATE_COMMON, false), Note::Used(2, View::Rtv, TARGET, true), Note::State(3, SHADER, false)]);
        assert_eq!(frame, [t(2, View::Rtv, TARGET), t(1, View::Rtv, D3D12_RESOURCE_STATE_COMMON)]);

        // a texture moved into or out of UNORDERED_ACCESS is a target; a render target that
        // is also written by compute stays one
        let mut frame = Vec::new();
        apply(&mut frame, &[Note::State(4, UAV, true), Note::Used(5, View::Rtv, TARGET, true), Note::State(5, UAV, true), Note::State(4, SHADER, true)]);
        assert_eq!(frame, [t(4, View::Uav, SHADER), t(5, View::Rtv, UAV)]);

        // a depth target written, then read by a shader and bound read-only: the read-only
        // bind does not say which read states it is in
        let both = D3D12_RESOURCE_STATES(READ.0 | SHADER.0);
        let mut frame = Vec::new();
        apply(&mut frame, &[Note::Used(6, View::Dsv, WRITE, true), Note::State(6, both, false), Note::Used(6, View::Dsv, READ, false), Note::Used(7, View::Dsv, READ, false)]);
        assert_eq!(frame, [t(6, View::Dsv, both), t(7, View::Dsv, READ)]);
    }

    #[test]
    fn eight_bit_and_ten_bit_texels_keep_their_colors() {
        assert_eq!(texel(Kind::Rgba8, &[10, 20, 30, 255]), [10, 20, 30]);
        assert_eq!(texel(Kind::Bgra8, &[10, 20, 30, 255]), [30, 20, 10]);
        let t = (1023u32 | 512 << 10 | 4 << 20 | 3 << 30).to_le_bytes();
        assert_eq!(texel(Kind::Rgb10A2, &t), [255, 128, 1]);
        for f in [DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_UNORM] {
            assert!(kind(f, false).is_some(), "{f:?}");
        }
        assert_eq!(texel(Kind::R8, &[200]), [200, 200, 200]);
        assert_eq!(texel(Kind::Rg8, &[200, 100]), [200, 100, 0]);
        assert_eq!(kind(DXGI_FORMAT_R8_UNORM, false).map(|k| k.0), Some(Kind::R8));
        assert_eq!(kind(DXGI_FORMAT_R8G8_UNORM, false).map(|k| k.0), Some(Kind::Rg8));
        assert_eq!(kind(DXGI_FORMAT_R32G32B32A32_FLOAT, false), None);
        assert_eq!(kind(DXGI_FORMAT_BC1_UNORM, false), None);
    }

    #[test]
    fn ids_are_colors_and_depths_are_greys_between_the_values_drawn() {
        assert_eq!(id_color(0), [0, 0, 0]);
        assert_eq!(id_color(7), id_color(7));
        assert_ne!(id_color(7), id_color(8));
        assert!((1..2000).all(|id| id_color(id).iter().all(|&c| c >= 0x40)));

        // the cleared values (0, 1, the largest, no number) are not part of the range
        assert_eq!(range(&[0.0, 1.0, f32::MAX, f32::NAN, f32::INFINITY, 0.25, 0.75, 0.5]), (0.25, 0.75));
        assert_eq!(range(&[0.0, 1.0]), (0.0, 1.0));
        assert_eq!([grey(0.25, 0.25, 0.75), grey(0.5, 0.25, 0.75), grey(0.75, 0.25, 0.75), grey(0.0, 0.25, 0.75), grey(1.0, 0.25, 0.75), grey(f32::NAN, 0.25, 0.75)], [0, 128, 255, 0, 255, 0]);
        assert_eq!([grey(0.5, 0.5, 0.5), grey(0.0, 0.5, 0.5)], [255, 0]);

        // a reversed-Z depth buffer: cleared to 0, drawn at 0.25 … 0.75
        let depth: Vec<u8> = [0.0f32, 0.25, 0.5, 0.75].iter().flat_map(|v| v.to_le_bytes()).collect();
        assert_eq!(convert(Kind::Float32, &depth, 4, 1, 16, 1), (4, 1, vec![0, 0, 0, 0, 0, 0, 128, 128, 128, 255, 255, 255]));
        for (f, k) in [(DXGI_FORMAT_D32_FLOAT, Kind::Float32), (DXGI_FORMAT_R32_FLOAT, Kind::Float32), (DXGI_FORMAT_R32G8X24_TYPELESS, Kind::Float32), (DXGI_FORMAT_D32_FLOAT_S8X24_UINT, Kind::Float32), (DXGI_FORMAT_D24_UNORM_S8_UINT, Kind::Depth24), (DXGI_FORMAT_R24G8_TYPELESS, Kind::Depth24), (DXGI_FORMAT_R16_FLOAT, Kind::R16Float), (DXGI_FORMAT_R32_UINT, Kind::R32Uint), (DXGI_FORMAT_R32G32_UINT, Kind::Rg32Uint)] {
            assert_eq!(kind(f, true).map(|k| k.0), Some(k), "{f:?}");
        }
        // R16_TYPELESS: a depth as a depth-stencil target, a half float otherwise
        assert_eq!((kind(DXGI_FORMAT_R16_TYPELESS, true).unwrap().0, kind(DXGI_FORMAT_R16_TYPELESS, false).unwrap().0), (Kind::Depth16, Kind::R16Float));
        // D24S8: the stencil byte above the depth is not part of it
        assert_eq!(scalar(Kind::Depth24, &0xAB80_0000u32.to_le_bytes()), 0x80_0000 as f32 / 16_777_215.0);
        assert_eq!([scalar(Kind::Depth16, &[0xFF, 0xFF]), scalar(Kind::R16Float, &[0x00, 0x38])], [1.0, 0.5]);
        assert_eq!(convert(Kind::R32Uint, &[7, 0, 0, 0, 0, 0, 0, 0], 2, 1, 8, 1), (2, 1, [id_color(7), [0; 3]].concat()));
    }

    #[test]
    fn a_two_word_target_shows_each_word_as_ids_and_as_a_grey() {
        // 2×1 texels: (ID 7, 100) and (ID 9, 300)
        let data: Vec<u8> = [7u32, 100, 9, 300].iter().flat_map(|v| v.to_le_bytes()).collect();
        let (w, h, rgb) = convert(Kind::Rg32Uint, &data, 2, 1, 16, 1);
        assert_eq!((w, h), (4, 2));
        // first word: IDs | grey; below, the second word the same way
        assert_eq!(rgb[..12], [id_color(7), id_color(9), [0; 3], [255; 3]].concat());
        assert_eq!(rgb[12..], [id_color(100), id_color(300), [0; 3], [255; 3]].concat());
    }

    #[test]
    fn float_texels_are_tone_mapped() {
        assert_eq!([half(0x3C00), half(0xC000), half(0x3800), half(0)], [1.0, -2.0, 0.5, 0.0]);
        // 1.0 → sqrt(1/2), 3.0 → sqrt(3/4), 0 → black; alpha is not looked at
        let t: Vec<u8> = [0x3C00u16, 0x4200, 0, 0x3C00].iter().flat_map(|v| v.to_le_bytes()).collect();
        assert_eq!(texel(Kind::Rgba16Float, &t), [180, 221, 0]);
        // negative, infinite and NaN values: black, white, black
        let t: Vec<u8> = [0xBC00u16, 0x7C00, 0x7E00, 0].iter().flat_map(|v| v.to_le_bytes()).collect();
        assert_eq!(texel(Kind::Rgba16Float, &t), [0, 255, 0]);
        // R11G11B10: r and g 1.0 (exponent 15, 6 mantissa bits), b 2.0 (exponent 16, 5 mantissa bits)
        let t = (15u32 << 6 | (15 << 6) << 11 | (16 << 5) << 22).to_le_bytes();
        assert_eq!(texel(Kind::R11G11B10Float, &t), [180, 180, 208]);
    }

    #[test]
    fn two_channel_texels_are_normals() {
        // (0, 0): the normal facing the viewer, the blue of normal maps
        let t: Vec<u8> = [0u16, 0].iter().flat_map(|v| v.to_le_bytes()).collect();
        assert_eq!(texel(Kind::Rg16Float, &t), [128, 128, 255]);
        // (1, -1) is longer than a unit normal: z is 0
        let t: Vec<u8> = [0x3C00u16, 0xBC00].iter().flat_map(|v| v.to_le_bytes()).collect();
        assert_eq!(texel(Kind::Rg16Float, &t), [255, 0, 128]);
        // UNORM: 0..1 stands for -1..1
        let t: Vec<u8> = [0xFFFFu16, 0x8000].iter().flat_map(|v| v.to_le_bytes()).collect();
        assert_eq!(texel(Kind::Rg16Unorm, &t), [255, 128, 128]);
    }

    #[test]
    fn rows_are_read_at_their_pitch_and_thinned() {
        // 4×2 texels in rows 20 bytes apart; the padding is never read as a texel
        let mut data = vec![0xEEu8; 20 + 16];
        for (y, x) in [(0, 0), (0, 1), (0, 2), (0, 3), (1, 0), (1, 1), (1, 2), (1, 3)] {
            data[y * 20 + 4 * x..][..4].copy_from_slice(&[(10 * y + x) as u8, 0, 0, 255]);
        }
        assert_eq!(convert(Kind::Rgba8, &data, 4, 2, 20, 1), (4, 2, vec![0, 0, 0, 1, 0, 0, 2, 0, 0, 3, 0, 0, 10, 0, 0, 11, 0, 0, 12, 0, 0, 13, 0, 0]));
        assert_eq!(convert(Kind::Rgba8, &data, 4, 2, 20, 2), (2, 1, vec![0, 0, 0, 2, 0, 0]));
        // 8-byte texels
        let wide: Vec<u8> = [0x3C00u16, 0, 0, 0, 0, 0x3C00, 0, 0].iter().flat_map(|v| v.to_le_bytes()).collect();
        assert_eq!(convert(Kind::Rgba16Float, &wide, 2, 1, 16, 1), (2, 1, vec![180, 0, 0, 0, 180, 0]));
    }

    #[test]
    fn the_image_is_a_png_of_the_rows() {
        assert_eq!(crc32(&[b"IE", b"ND"]), 0xAE42_6082);
        let rgb = [1u8, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12];
        let file = png(2, 2, &rgb);
        assert_eq!(&file[..8], b"\x89PNG\r\n\x1a\n");
        // IHDR: 13 bytes – 2 × 2, 8 bits, RGB
        assert_eq!(&file[8..33], [&[0, 0, 0, 13][..], b"IHDR", &[0, 0, 0, 2, 0, 0, 0, 2, 8, 2, 0, 0, 0], &crc32(&[&file[12..29]]).to_be_bytes()].concat());
        let len = u32::from_be_bytes(file[33..37].try_into().unwrap()) as usize;
        assert_eq!(&file[37..41], b"IDAT");
        let rows = miniz_oxide::inflate::decompress_to_vec_zlib(&file[41..41 + len]).unwrap();
        assert_eq!(rows, [0, 1, 2, 3, 4, 5, 6, 0, 7, 8, 9, 10, 11, 12]);
        assert_eq!(&file[41 + len + 4..], [&[0, 0, 0, 0][..], b"IEND", &[0xAE, 0x42, 0x60, 0x82]].concat());
    }
}
