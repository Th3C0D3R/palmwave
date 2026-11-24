//! `XCL_RT_DUMP=1`: the color render targets the title bound during a frame, written to the
//! logs folder at its present (`rt_<frame>_<k>_<format>_<w>x<h>.png`, largest first) – what
//! the GPU drew before the final image: a G-buffer shows the world where later passes leave
//! the presented frame black.
//!
//! Frames are chosen as for `XCL_FRAME_DUMP`: every 120th (`XCL_RT_DUMP=n`, n > 1: every
//! n-th), and every one from `XCL_RT_DUMP_FROM=n` on (unset: `XCL_FRAME_DUMP_FROM`).
//!
//! The copy needs each target's state at the present. A bound target is in RENDER_TARGET;
//! the transitions the title records after that are followed list by list, in submission
//! order (as `title_textures` does for the textures it uploads into).

use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::{Mutex, OnceLock};

use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::*;

/// Targets smaller than this are not dumped (shadow tiles, luminance chains, …).
const MIN_SIZE: (u64, u32) = (640, 360);
/// Targets dumped per frame at most.
const MAX_TARGETS: usize = 8;

/// (every, from) as `present::dump_due` takes them; None: the dump is off.
fn schedule(rt: Option<&str>, rt_from: Option<&str>, frame_from: Option<&str>) -> Option<(u64, u64)> {
    let rt = rt?.trim();
    if rt.is_empty() || rt == "0" {
        return None;
    }
    let from = rt_from.or(frame_from).and_then(|v| v.trim().parse().ok()).unwrap_or(u64::MAX);
    Some((rt.parse().ok().filter(|&n| n > 1).unwrap_or(120), from))
}

fn settings() -> Option<(u64, u64)> {
    static S: OnceLock<Option<(u64, u64)>> = OnceLock::new();
    *S.get_or_init(|| {
        let var = |name: &str| std::env::var(name).ok();
        schedule(var("XCL_RT_DUMP").as_deref(), var("XCL_RT_DUMP_FROM").as_deref(), var("XCL_FRAME_DUMP_FROM").as_deref())
    })
}

pub(super) fn enabled() -> bool {
    settings().is_some()
}

/// What a PC list recorded, in order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Note {
    /// OMSetRenderTargets named this resource
    Bound(usize),
    /// a transition of subresource 0 of this resource, to this state
    State(usize, D3D12_RESOURCE_STATES),
}

/// PC command list → what it recorded since it was made or last reset. It applies when the
/// list is submitted.
static NOTES: Mutex<Option<HashMap<usize, Vec<Note>>>> = Mutex::new(None);
/// The targets bound by the lists submitted since the last present, in the order they were
/// first bound, each with the state of its subresource 0.
static FRAME: Mutex<Vec<(usize, D3D12_RESOURCE_STATES)>> = Mutex::new(Vec::new());

fn note(list: usize, n: Note) {
    NOTES.lock().unwrap().get_or_insert_with(HashMap::new).entry(list).or_default().push(n);
}

/// The title bound `res` as a color render target on PC list `list`.
pub(super) fn note_bound(list: usize, res: usize) {
    note(list, Note::Bound(res));
}

/// The title recorded a transition of `res` to `after` on PC list `list` (`begin_only`: the
/// first half of a split barrier, which changes nothing yet).
pub(super) fn note_transition(list: usize, res: usize, subresource: u32, after: D3D12_RESOURCE_STATES, begin_only: bool) {
    if !begin_only && (subresource == 0 || subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        note(list, Note::State(res, after));
    }
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
fn apply(frame: &mut Vec<(usize, D3D12_RESOURCE_STATES)>, notes: &[Note]) {
    for &n in notes {
        let (res, state, bound) = match n {
            Note::Bound(r) => (r, D3D12_RESOURCE_STATE_RENDER_TARGET, true),
            Note::State(r, s) => (r, s, false),
        };
        match frame.iter_mut().find(|(r, _)| *r == res) {
            Some(e) => e.1 = state,
            None if bound => frame.push((res, state)),
            // not a target of this frame
            None => {}
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

/// The largest `MAX_TARGETS` of the (target, width, height) that are `MIN_SIZE` at least,
/// largest first (the same size: in the order given).
fn select<T>(mut targets: Vec<(T, u64, u32)>) -> Vec<(T, u64, u32)> {
    targets.retain(|&(_, w, h)| w >= MIN_SIZE.0 && h >= MIN_SIZE.1);
    targets.sort_by_key(|&(_, w, h)| std::cmp::Reverse(w * h as u64));
    targets.truncate(MAX_TARGETS);
    targets
}

/// The frame about to be presented as the `n`-th ends: the targets to copy for its dump, each
/// with its description and the state it is in (none when this frame is not dumped).
pub(super) unsafe fn frame_targets(n: u64) -> Vec<(ID3D12Resource, D3D12_RESOURCE_DESC, D3D12_RESOURCE_STATES)> {
    let bound = std::mem::take(&mut *FRAME.lock().unwrap());
    if !settings().is_some_and(|(every, from)| super::present::dump_due(n, every, from)) {
        return Vec::new();
    }
    let mut found = Vec::new();
    for &(r, state) in &bound {
        // (a target released since, or whose view was overwritten, is not looked at)
        let Some(res) = super::present::live_target(r) else { continue };
        let d = res.GetDesc();
        if d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width < MIN_SIZE.0 || d.Height < MIN_SIZE.1 {
            continue;
        }
        if d.SampleDesc.Count > 1 || kind(d.Format).is_none() {
            crate::xlog_info!("rt dump: frame {n}: target {r:#x} {}x{} skipped – format {} ({} samples) is not converted", d.Width, d.Height, d.Format.0, d.SampleDesc.Count);
            continue;
        }
        found.push(((res, d, state), d.Width, d.Height));
    }
    let chosen = select(found);
    crate::xlog_info!("rt dump: frame {n}: {} of the {} render targets bound are copied", chosen.len(), bound.len());
    chosen.into_iter().map(|(t, _, _)| t).collect()
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
}

/// The conversion of `format` and its name in the file (None: not converted). A typeless
/// target is read as its family's usual format: the views are not known here.
fn kind(format: DXGI_FORMAT) -> Option<(Kind, &'static str)> {
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
        _ => return None,
    })
}

fn texel_bytes(kind: Kind) -> usize {
    if kind == Kind::Rgba16Float { 8 } else { 4 }
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

/// one texel (`texel_bytes` bytes) → 8-bit RGB
fn texel(kind: Kind, t: &[u8]) -> [u8; 3] {
    let word = || u32::from_le_bytes([t[0], t[1], t[2], t[3]]);
    let h = |k: usize| u16::from_le_bytes([t[2 * k], t[2 * k + 1]]);
    match kind {
        Kind::Rgba8 => [t[0], t[1], t[2]],
        Kind::Bgra8 => [t[2], t[1], t[0]],
        Kind::Rgb10A2 => [(word() >> 2) as u8, (word() >> 12) as u8, (word() >> 22) as u8],
        Kind::R11G11B10Float => {
            let f = super::present::small_float;
            [tonemap(f(word() & 0x7FF, 6)), tonemap(f(word() >> 11 & 0x7FF, 6)), tonemap(f(word() >> 22, 5))]
        }
        Kind::Rgba16Float => [tonemap(half(h(0))), tonemap(half(h(1))), tonemap(half(h(2)))],
        Kind::Rg16Float => normal(half(h(0)), half(h(1))),
        Kind::Rg16Unorm => normal(h(0) as f32 / 65535.0 * 2.0 - 1.0, h(1) as f32 / 65535.0 * 2.0 - 1.0),
    }
}

/// Every `step`-th texel of every `step`-th row of a `w`×`h` image with rows `pitch` bytes
/// apart, as (width, height, 8-bit RGB rows from the top).
fn convert(kind: Kind, data: &[u8], w: usize, h: usize, pitch: usize, step: usize) -> (usize, usize, Vec<u8>) {
    let bytes = texel_bytes(kind);
    let (ow, oh) = (w.div_ceil(step), h.div_ceil(step));
    let mut rgb = Vec::with_capacity(3 * ow * oh);
    for y in (0..h).step_by(step) {
        for x in (0..w).step_by(step) {
            let at = y * pitch + x * bytes;
            rgb.extend_from_slice(&texel(kind, &data[at..at + bytes]));
        }
    }
    (ow, oh, rgb)
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
/// texel in `step` so that it is 960 wide at most, every texel with `XCL_FRAME_DUMP_FULL=1`.
pub(super) unsafe fn write(rb: &ID3D12Resource, fp: &D3D12_PLACED_SUBRESOURCE_FOOTPRINT, format: DXGI_FORMAT, n: u64, k: usize) {
    let Some((kind, name)) = kind(format) else { return };
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
    let path = boot.logs_dir().join(format!("rt_{n}_{k}_{name}_{w}x{h}.png"));
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
        assert_eq!(schedule(None, Some("5"), Some("7")), None);
        assert_eq!(schedule(Some("0"), None, None), None);
        assert_eq!(schedule(Some("1"), None, None), Some((120, u64::MAX)));
        assert_eq!(schedule(Some("30"), None, Some(" 1825 ")), Some((30, 1825)));
        assert_eq!(schedule(Some("1"), Some("40"), Some("1825")), Some((120, 40)));
    }

    #[test]
    fn the_eight_largest_targets_of_640x360_or_more_are_taken_largest_first() {
        let got = select(vec![("small", 639, 360), ("flat", 4096, 359), ("hd-a", 1920, 1080), ("min", 640, 360), ("4k", 3840, 2160), ("hd-b", 1920, 1080)]);
        assert_eq!(got.iter().map(|t| t.0).collect::<Vec<_>>(), ["4k", "hd-a", "hd-b", "min"]);
        let many: Vec<(u64, u64, u32)> = (0..12).map(|k| (k, 640 + k, 360)).collect();
        assert_eq!(select(many).iter().map(|t| t.0).collect::<Vec<_>>(), [11, 10, 9, 8, 7, 6, 5, 4]);
    }

    #[test]
    fn a_frame_holds_the_targets_bound_in_it_in_the_states_the_lists_leave() {
        const SHADER: D3D12_RESOURCE_STATES = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        let mut frame = Vec::new();
        // a transition before the first bind is not this frame's; the ones after it are
        apply(&mut frame, &[Note::State(1, SHADER), Note::Bound(2), Note::Bound(1), Note::State(2, SHADER)]);
        assert_eq!(frame, [(2, SHADER), (1, D3D12_RESOURCE_STATE_RENDER_TARGET)]);
        apply(&mut frame, &[Note::State(1, D3D12_RESOURCE_STATE_COMMON), Note::Bound(2), Note::State(3, SHADER)]);
        assert_eq!(frame, [(2, D3D12_RESOURCE_STATE_RENDER_TARGET), (1, D3D12_RESOURCE_STATE_COMMON)]);
    }

    #[test]
    fn eight_bit_and_ten_bit_texels_keep_their_colors() {
        assert_eq!(texel(Kind::Rgba8, &[10, 20, 30, 255]), [10, 20, 30]);
        assert_eq!(texel(Kind::Bgra8, &[10, 20, 30, 255]), [30, 20, 10]);
        let t = (1023u32 | 512 << 10 | 4 << 20 | 3 << 30).to_le_bytes();
        assert_eq!(texel(Kind::Rgb10A2, &t), [255, 128, 1]);
        for f in [DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_UNORM] {
            assert!(kind(f).is_some(), "{f:?}");
        }
        assert_eq!(kind(DXGI_FORMAT_R32_FLOAT), None);
        assert_eq!(kind(DXGI_FORMAT_D32_FLOAT), None);
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
