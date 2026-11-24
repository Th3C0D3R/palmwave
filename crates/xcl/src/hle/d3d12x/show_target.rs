//! `XCL_SHOW_TARGET=albedo|lit|depth|<n>`: a debug view in the XCL window in place of the
//! title's picture – what the title's G-buffer pass drew this frame, drawn by a pass of XCL's
//! own at the present. It is not the title's lighting.
//!
//! - `albedo`: render target 0 of the G-buffer pass (the albedo), read as sRGB;
//! - `lit`: that albedo × (0.25 + 0.75 × max(0, N·L)), N the normal in render target 1, L a
//!   fixed light from above and in front, where the depth buffer was drawn (depth > 0: reversed
//!   Z, cleared to 0), a dim sky colour where it was not;
//! - `depth`: the pass's depth buffer as a grey ramp, near white, far dark;
//! - `<n>`: the frame's n-th target as `XCL_RT_DUMP` numbers them (`rt_<frame>_<n>_…`).
//!
//! The G-buffer pass is an OMSetRenderTargets of five color targets or more of one size (the
//! world's) with a depth buffer of that size (`gbuffer`). A frame without one, and a frame
//! without target n, shows the title's picture, as does an unset switch.
//!
//! The targets are read in the states the title left them in at the present (`rt_dump` follows
//! its transitions): moved to PIXEL_SHADER_RESOURCE for the draw and back after it, each held by
//! a reference until the GPU is done with the draw (`Viewer::done`).
//!
//! The normal of render target 1 (R8G8B8A8_UNORM): two 12-bit octahedral coordinates –
//! G | B << 8 | A << 16 holds x in bits 0–11 and y in bits 12–23, 0…4095 for -1…1; R is
//! something else (0 on a character, 114–255 on the street around it). Checked on the rt dump of
//! a world frame, whose images keep R, G and B: x read so is smooth over a surface (median step
//! 12 of 4095 between texels three apart on a character, 0.7 % of steps over 512), the other
//! orders of G and B are not (median steps 196 to 1280); B's high nibble changes between such
//! neighbours as often as G's low nibble (86 %), its low nibble rarely (14 %), and it is the same
//! over flat faces (in 91 % of the 5×5 windows where x is, 0.5 % of the others) – y's low bits.
//! A, y's high byte, is not in the dumps. Which space the normals are in is not known: L =
//! (0, 0.6, 0.8) comes from above and in front both in a view space with +Y up and +Z towards
//! the viewer and in a world with +Z up.

use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Mutex, OnceLock};

use windows::core::Interface;
use windows::Win32::Foundation::RECT;
use windows::Win32::Graphics::Direct3D::D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::*;

use super::rt_dump::{Binding, Bound, Frame, Source, View};

/// Color targets of the G-buffer pass's size that an OMSetRenderTargets binds, at least.
pub(super) const GBUFFER_TARGETS: usize = 5;

/// What `XCL_SHOW_TARGET` shows.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Show {
    Albedo,
    Lit,
    Depth,
    /// the frame's n-th target, as the rt dump numbers them
    Target(usize),
}

/// The switch's value (None: not one of the views).
fn parse(v: &str) -> Option<Show> {
    match v.trim().to_ascii_lowercase().as_str() {
        "albedo" => Some(Show::Albedo),
        "lit" => Some(Show::Lit),
        "depth" => Some(Show::Depth),
        n => n.parse().ok().map(Show::Target),
    }
}

fn setting() -> Option<Show> {
    static S: OnceLock<Option<Show>> = OnceLock::new();
    *S.get_or_init(|| {
        let v = std::env::var("XCL_SHOW_TARGET").ok()?;
        let show = parse(&v);
        match show {
            Some(s) => crate::xlog_info!("XCL_SHOW_TARGET={v}: the window shows {s:?} of each frame's targets – a debug view, not the title's lighting"),
            None if !v.trim().is_empty() => crate::xlog_warn!("XCL_SHOW_TARGET={v:?}: expected albedo, lit, depth or a target number – off"),
            None => {}
        }
        show
    })
}

/// The switch is on.
pub(super) fn enabled() -> bool {
    setting().is_some()
}

/// The G-buffer pass among a frame's `bindings`: `GBUFFER_TARGETS` color targets or more of the
/// size of target 0 (at least `rt_dump::MIN_SIZE`), target 1 among them, and a depth buffer of
/// that size. Of several: the largest, then the one set most often, then the first.
fn gbuffer(bindings: &[Binding]) -> Option<&Binding> {
    let size = |t: &Bound| (t.width, t.height);
    let (min_w, min_h) = super::rt_dump::MIN_SIZE;
    bindings
        .iter()
        .enumerate()
        .filter(|(_, b)| {
            let (Some(Some(rt0)), Some(Some(rt1)), Some(depth)) = (b.rts.first(), b.rts.get(1), b.dsv) else { return false };
            let world = size(rt0);
            world.0 >= min_w && world.1 >= min_h && size(rt1) == world && size(&depth) == world && b.rts.iter().flatten().filter(|t| size(*t) == world).count() >= GBUFFER_TARGETS
        })
        .max_by_key(|&(k, b)| (b.rts[0].map_or(0, |t| t.width * t.height as u64), b.binds, std::cmp::Reverse(k)))
        .map(|(_, b)| b)
}

/// What the pixel shader draws (its `look`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
enum Look {
    /// t0's color as stored (8- / 10-bit UNORM: the values the window shows as sRGB)
    Stored = 0,
    /// t0 through an _SRGB view (linear values): encoded again
    Linear = 1,
    /// t0's HDR color, tone-mapped as the rt dump does it
    Hdr = 2,
    /// t0's x and y of a normal in -1…1 (R16G16_FLOAT), as a normal-map color
    Normal = 3,
    /// … in 0…1 for -1…1 (R16G16_UNORM)
    NormalUnorm = 4,
    /// t0's one channel as a grey
    Channel = 5,
    /// t0's one value (a depth, a distance) on the depth ramp
    Scalar = 6,
    /// t3's first word as an ID color
    Id = 7,
    /// the G-buffer's albedo (t0)
    Albedo = 8,
    /// the albedo lit (t0, t1 the G-buffer's normals, t2 its depth)
    Lit = 9,
    /// the G-buffer's depth (t2) on the depth ramp
    Depth = 10,
}

/// How a target of `format` is read and drawn (`depth`: its depth plane): the view format and
/// the look, as the rt dump shows that format. None: a format the rt dump does not convert, or
/// a typed depth format, which no shader view reads (XCL makes the title's depth buffers
/// typeless, `device_x::pc_resource_desc`).
fn view_of(format: DXGI_FORMAT, depth: bool) -> Option<(DXGI_FORMAT, Look)> {
    Some(match format {
        DXGI_FORMAT_R8G8B8A8_UNORM | DXGI_FORMAT_R8G8B8A8_TYPELESS => (DXGI_FORMAT_R8G8B8A8_UNORM, Look::Stored),
        DXGI_FORMAT_B8G8R8A8_UNORM | DXGI_FORMAT_B8G8R8A8_TYPELESS => (DXGI_FORMAT_B8G8R8A8_UNORM, Look::Stored),
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB | DXGI_FORMAT_B8G8R8A8_UNORM_SRGB => (format, Look::Linear),
        DXGI_FORMAT_R10G10B10A2_UNORM | DXGI_FORMAT_R10G10B10A2_TYPELESS => (DXGI_FORMAT_R10G10B10A2_UNORM, Look::Stored),
        DXGI_FORMAT_R11G11B10_FLOAT => (format, Look::Hdr),
        DXGI_FORMAT_R16G16B16A16_FLOAT | DXGI_FORMAT_R16G16B16A16_TYPELESS => (DXGI_FORMAT_R16G16B16A16_FLOAT, Look::Hdr),
        DXGI_FORMAT_R16G16_FLOAT | DXGI_FORMAT_R16G16_TYPELESS => (DXGI_FORMAT_R16G16_FLOAT, Look::Normal),
        DXGI_FORMAT_R16G16_UNORM => (format, Look::NormalUnorm),
        DXGI_FORMAT_R8_UNORM | DXGI_FORMAT_R8_TYPELESS => (DXGI_FORMAT_R8_UNORM, Look::Channel),
        DXGI_FORMAT_R8G8_UNORM | DXGI_FORMAT_R8G8_TYPELESS => (DXGI_FORMAT_R8G8_UNORM, Look::Stored),
        DXGI_FORMAT_R32G32_UINT | DXGI_FORMAT_R32G32_TYPELESS => (DXGI_FORMAT_R32G32_UINT, Look::Id),
        DXGI_FORMAT_R32_UINT => (format, Look::Id),
        DXGI_FORMAT_R32_FLOAT | DXGI_FORMAT_R16_FLOAT => (format, Look::Scalar),
        DXGI_FORMAT_R32_TYPELESS => (DXGI_FORMAT_R32_FLOAT, Look::Scalar),
        DXGI_FORMAT_R32G8X24_TYPELESS => (DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, Look::Scalar),
        DXGI_FORMAT_R24G8_TYPELESS => (DXGI_FORMAT_R24_UNORM_X8_TYPELESS, Look::Scalar),
        DXGI_FORMAT_R16_TYPELESS if depth => (DXGI_FORMAT_R16_UNORM, Look::Scalar),
        DXGI_FORMAT_R16_TYPELESS => (DXGI_FORMAT_R16_FLOAT, Look::Scalar),
        _ => return None,
    })
}

/// The albedo's view: an _SRGB one where its format has one (linear values), else its own
/// (true: the shader decodes the sRGB curve itself).
fn albedo_view(format: DXGI_FORMAT) -> Option<(DXGI_FORMAT, bool)> {
    match format {
        DXGI_FORMAT_R8G8B8A8_TYPELESS => Some((DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false)),
        DXGI_FORMAT_B8G8R8A8_TYPELESS => Some((DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, false)),
        f => match view_of(f, false)? {
            (v, Look::Linear) => Some((v, false)),
            (v, Look::Stored) => Some((v, true)),
            _ => None,
        },
    }
}

/// What a frame's view reads – per register t0–t3, a target and the view format it is read
/// through – and draws of it; `what`: the targets, for the log.
struct Plan<'a> {
    look: Look,
    /// the albedo's view is not an _SRGB one
    stored_albedo: bool,
    regs: [Option<(&'a Source, DXGI_FORMAT)>; 4],
    what: String,
}

fn describe(t: Bound) -> String {
    format!("{:#x} {}x{} format {}", t.res, t.width, t.height, t.format.0)
}

/// A target made without shader views (DENY_SHADER_RESOURCE): one would be an invalid call.
fn denied(s: &Source) -> bool {
    s.desc.Flags.0 & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE.0 != 0
}

/// The view `show` of `frame` (Err: why the frame has none).
fn plan(show: Show, frame: &Frame) -> Result<Plan<'_>, String> {
    if let Show::Target(n) = show {
        let s = frame.targets.get(n).ok_or_else(|| format!("no target {n} (the frame has {})", frame.targets.len()))?;
        let view = view_of(s.desc.Format, s.view == View::Dsv).filter(|_| !denied(s));
        let (format, look) = view.ok_or_else(|| format!("target {n} (format {}, flags {:#x}) has no shader view", s.desc.Format.0, s.desc.Flags.0))?;
        let mut regs = [None; 4];
        regs[if look == Look::Id { 3 } else { 0 }] = Some((s, format));
        let what = format!("target {n}, {} {}", s.view.name(), describe(Bound { res: s.res.as_raw() as usize, width: s.desc.Width, height: s.desc.Height, format: s.desc.Format }));
        return Ok(Plan { look, stored_albedo: false, regs, what });
    }
    let b = gbuffer(&frame.bindings).ok_or("no G-buffer pass (five color targets or more of one size, with a depth buffer of it)")?;
    let (Some(Some(rt0)), Some(Some(rt1)), Some(depth)) = (b.rts.first(), b.rts.get(1), b.dsv) else { return Err("no G-buffer pass".into()) };
    let (rt0, rt1) = (*rt0, *rt1);
    let source = |what: &str, t: Bound| match frame.targets.iter().find(|s| s.res.as_raw() as usize == t.res) {
        Some(s) if !denied(s) => Ok(s),
        Some(_) => Err(format!("the G-buffer pass's {what} {:#x} has no shader view", t.res)),
        None => Err(format!("the G-buffer pass's {what} {:#x} is not among the frame's targets", t.res)),
    };
    let what = format!("the G-buffer pass ({} color targets, set {} times): RT0 {}, RT1 {}, depth {}", b.rts.iter().flatten().count(), b.binds, describe(rt0), describe(rt1), describe(depth));
    let depth_view = || view_of(depth.format, true).filter(|v| v.1 == Look::Scalar).map(|v| v.0).ok_or_else(|| format!("its depth buffer's format {} has no view of its depth", depth.format.0));
    let albedo = || albedo_view(rt0.format).ok_or_else(|| format!("its RT0's format {} is not an albedo's", rt0.format.0));
    let plan = match show {
        Show::Albedo => {
            let (view, stored) = albedo()?;
            Plan { look: Look::Albedo, stored_albedo: stored, regs: [Some((source("RT0", rt0)?, view)), None, None, None], what }
        }
        Show::Lit => {
            let (view, stored) = albedo()?;
            if !matches!(rt1.format, DXGI_FORMAT_R8G8B8A8_UNORM | DXGI_FORMAT_R8G8B8A8_TYPELESS) {
                return Err(format!("its RT1's format {} is not the packed normal's (R8G8B8A8_UNORM)", rt1.format.0));
            }
            let regs = [Some((source("RT0", rt0)?, view)), Some((source("RT1", rt1)?, DXGI_FORMAT_R8G8B8A8_UNORM)), Some((source("depth buffer", depth)?, depth_view()?)), None];
            Plan { look: Look::Lit, stored_albedo: stored, regs, what }
        }
        _ => Plan { look: Look::Depth, stored_albedo: false, regs: [None, None, Some((source("depth buffer", depth)?, depth_view()?)), None], what },
    };
    Ok(plan)
}

/// Logs what a frame's view reads when it is not what the last frame's read (and a frame that
/// has none), 16 times at most.
fn note(key: Option<Vec<usize>>, line: impl FnOnce() -> String) {
    static LAST: Mutex<Option<Option<Vec<usize>>>> = Mutex::new(None);
    static LINES: AtomicU32 = AtomicU32::new(0);
    let mut last = LAST.lock().unwrap();
    if last.as_ref() == Some(&key) {
        return;
    }
    *last = Some(key);
    let n = LINES.fetch_add(1, Ordering::Relaxed);
    if n < 16 {
        crate::xlog_info!("{}{}", line(), if n == 15 { " (no more logged)" } else { "" });
    }
}

const SHADER: &str = r#"
#define RS "RootConstants(num32BitConstants=6, b0), DescriptorTable(SRV(t0, numDescriptors=4), visibility=SHADER_VISIBILITY_PIXEL)"
// t0: a color target (the albedo), t1: the G-buffer's normals, t2: a depth plane, t3: IDs
Texture2DArray<float4> color : register(t0);
Texture2DArray<float4> normals : register(t1);
Texture2DArray<float> depth : register(t2);
Texture2DArray<uint4> ids : register(t3);
// look: what is drawn (`Look`); flags: 1 the back buffer is linear (FP16), 2 the albedo's view is
// not an _SRGB one; src: the targets' size, dst: the back buffer's
cbuffer C : register(b0) { uint look; uint flags; uint2 src; uint2 dst; };

[RootSignature(RS)]
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 decode(float3 c) { return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4)); }
float3 encode(float3 c) { return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055); }

// G | B << 8 | A << 16: two 12-bit octahedral coordinates, x in bits 0-11, y in 12-23
float3 gbuffer_normal(float4 t) {
    uint3 b = (uint3)round(saturate(t.gba) * 255.0);
    uint v = b.x | b.y << 8 | b.z << 16;
    float2 e = float2(v & 0xFFF, v >> 12) / 4095.0 * 2.0 - 1.0;
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0, float2(1, 1), float2(-1, -1));
    return normalize(n);
}

// reversed Z: white at the near plane, a sixteenth darker each octave of depth further; 0 black
float ramp(float d) { return d > 0 ? saturate(1.0 + log2(d) / 16.0) : 0.0; }

float3 id_color(uint id) {
    uint h = id * 0x9E3779B1u;
    return id == 0 ? 0 : float3((h >> 24) | 0x40, ((h >> 16) & 0xFF) | 0x40, ((h >> 8) & 0xFF) | 0x40) / 255.0;
}

static const float3 LIGHT = float3(0, 0.6, 0.8);
static const float3 SKY = float3(0.02, 0.03, 0.05);

[RootSignature(RS)]
float4 ps(float4 pos : SV_Position) : SV_Target {
    uint2 p = min(uint2(pos.xy * float2(src) / float2(dst)), src - 1);
    int4 at = int4(p, 0, 0);
    float4 t = color.Load(at);
    float3 c;
    switch (look) {
    case 0: c = t.rgb; break;
    case 1: c = encode(saturate(t.rgb)); break;
    case 2: c = sqrt(max(t.rgb, 0) / (1 + max(t.rgb, 0))); break;
    case 3: case 4: {
        float2 n = look == 4 ? t.rg * 2 - 1 : t.rg;
        c = float3(n, sqrt(saturate(1 - dot(n, n)))) * 0.5 + 0.5;
        break;
    }
    case 5: c = t.rrr; break;
    case 6: c = ramp(t.r); break;
    case 7: c = id_color(ids.Load(at).r); break;
    case 8: c = flags & 2 ? t.rgb : encode(saturate(t.rgb)); break;
    case 9: {
        float3 a = flags & 2 ? decode(saturate(t.rgb)) : t.rgb;
        float l = 0.25 + 0.75 * max(0, dot(gbuffer_normal(normals.Load(at)), LIGHT));
        c = encode(saturate(depth.Load(at) > 0 ? a * l : SKY));
        break;
    }
    default: c = ramp(depth.Load(at)); break;
    }
    return float4(flags & 1 ? decode(c) : c, 1);
}
"#;

/// The view format of a register's null descriptor (no target there).
const NULL_VIEWS: [DXGI_FORMAT; 4] = [DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_UINT];

/// The pass drawing the views into back buffers of one format, its descriptors, and the targets
/// its last draw read (held until the GPU is done with it).
pub(super) struct Viewer {
    pub fmt: DXGI_FORMAT,
    rs: ID3D12RootSignature,
    pso: ID3D12PipelineState,
    srv: ID3D12DescriptorHeap,
    rtv: ID3D12DescriptorHeap,
    held: Vec<ID3D12Resource>,
}

impl Viewer {
    /// The pass drawing into `fmt` back buffers on `dev` (Err: no DXC, or the device refused it).
    pub unsafe fn new(dev: &ID3D12Device, fmt: DXGI_FORMAT) -> Result<Viewer, String> {
        use crate::gpu::recomp::dxc::compile_cached;
        let (vs, _) = compile_cached(SHADER, "vs", "vs_6_0")?;
        let (ps, _) = compile_cached(SHADER, "ps", "ps_6_0")?;
        let rs: ID3D12RootSignature = dev.CreateRootSignature(0, &ps).map_err(|e| format!("CreateRootSignature: {e}"))?;
        let mut pd = D3D12_GRAPHICS_PIPELINE_STATE_DESC {
            pRootSignature: std::mem::transmute_copy(&rs),
            VS: D3D12_SHADER_BYTECODE { pShaderBytecode: vs.as_ptr() as _, BytecodeLength: vs.len() },
            PS: D3D12_SHADER_BYTECODE { pShaderBytecode: ps.as_ptr() as _, BytecodeLength: ps.len() },
            SampleMask: u32::MAX,
            PrimitiveTopologyType: D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
            NumRenderTargets: 1,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            ..Default::default()
        };
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL.0 as u8;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RTVFormats[0] = fmt;
        let pso: ID3D12PipelineState = dev.CreateGraphicsPipelineState(&pd).map_err(|e| format!("CreateGraphicsPipelineState: {e}"))?;
        let heap = |ty, n, flags| {
            dev.CreateDescriptorHeap::<ID3D12DescriptorHeap>(&D3D12_DESCRIPTOR_HEAP_DESC { Type: ty, NumDescriptors: n, Flags: flags, NodeMask: 0 })
                .map_err(|e| format!("CreateDescriptorHeap: {e}"))
        };
        let srv = heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)?;
        let rtv = heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE)?;
        Ok(Viewer { fmt, rs, pso, srv, rtv, held: Vec::new() })
    }

    /// The GPU is done with the last draw: the targets it read are let go.
    pub fn done(&mut self) {
        self.held.clear();
    }

    /// Record on `list` the view the switch asks for of `frame`'s targets, drawn into all of
    /// `bb` (a `size` back buffer of `self.fmt` in PRESENT, left in RENDER_TARGET). False: the
    /// frame has nothing to show, nothing recorded. The descriptors are rewritten: the last
    /// draw must have finished on the GPU.
    pub unsafe fn record(&mut self, dev: &ID3D12Device, list: &ID3D12GraphicsCommandList, frame: &Frame, bb: &ID3D12Resource, size: (u32, u32)) -> bool {
        let Some(show) = setting() else { return false };
        let plan = match plan(show, frame) {
            Ok(p) => p,
            Err(why) => {
                note(None, || format!("show target: frame {}: {why} – the title's picture is shown", frame.number));
                return false;
            }
        };
        let sources: Vec<&Source> = plan.regs.iter().flatten().map(|r| r.0).collect();
        let key = sources.iter().map(|s| s.res.as_raw() as usize).chain([plan.look as usize]).collect();
        note(Some(key), || format!("show target: frame {}: {show:?} of {} – drawn in place of the title's picture (a debug view, not the title's lighting)", frame.number, plan.what));
        // read in PIXEL_SHADER_RESOURCE: the targets in another state are moved there and back
        let read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        let moved: Vec<&Source> = sources.iter().copied().filter(|s| s.state.0 & read.0 == 0).collect();
        let barrier = |s: &Source, before, after| {
            let mut b = super::present::transition(&s.res, before, after);
            (*b.Anonymous.Transition).Subresource = 0;
            b
        };
        let mut before: Vec<D3D12_RESOURCE_BARRIER> = moved.iter().map(|&s| barrier(s, s.state, read)).collect();
        before.push(super::present::transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));
        list.ResourceBarrier(&before);
        let first = sources[0];
        let src = (first.desc.Width as u32, first.desc.Height);
        let flags = (self.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) as u32 | (plan.stored_albedo as u32) << 1;
        let regs = plan.regs.map(|r| r.map(|(s, f)| (&s.res, f)));
        self.draw(dev, list, plan.look, flags, &regs, src, bb, size);
        let after: Vec<D3D12_RESOURCE_BARRIER> = moved.iter().map(|&s| barrier(s, read, s.state)).collect();
        if !after.is_empty() {
            list.ResourceBarrier(&after);
        }
        self.held = sources.iter().map(|s| s.res.clone()).collect();
        true
    }

    /// Record on `list` the draw of `look` of `regs` (t0–t3: a target readable by pixel shaders
    /// and its view format, None: nothing; `src` their size) into all of `bb` (`dst` large, in
    /// RENDER_TARGET).
    #[allow(clippy::too_many_arguments)]
    unsafe fn draw(&self, dev: &ID3D12Device, list: &ID3D12GraphicsCommandList, look: Look, flags: u32, regs: &[Option<(&ID3D12Resource, DXGI_FORMAT)>; 4], src: (u32, u32), bb: &ID3D12Resource, dst: (u32, u32)) {
        let inc = dev.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) as usize;
        let start = self.srv.GetCPUDescriptorHandleForHeapStart();
        for (k, reg) in regs.iter().enumerate() {
            let (res, format) = match *reg {
                Some((r, f)) => (Some(r), f),
                None => (None, NULL_VIEWS[k]),
            };
            // the first array slice and mip; of a depth buffer, its depth plane
            let view = D3D12_SHADER_RESOURCE_VIEW_DESC {
                Format: format,
                ViewDimension: D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
                Shader4ComponentMapping: D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                Anonymous: D3D12_SHADER_RESOURCE_VIEW_DESC_0 { Texture2DArray: D3D12_TEX2D_ARRAY_SRV { MipLevels: 1, ArraySize: 1, ..Default::default() } },
            };
            dev.CreateShaderResourceView(res, Some(&view), D3D12_CPU_DESCRIPTOR_HANDLE { ptr: start.ptr + k * inc });
        }
        let rtv = self.rtv.GetCPUDescriptorHandleForHeapStart();
        dev.CreateRenderTargetView(bb, None, rtv);
        // `dev` is the title's (adopted) device: its CreateRenderTargetView hook keeps a reference
        // to the view's resource (`present::note_rtv`). Held on a swap chain buffer, that reference
        // makes ResizeBuffers fail: this view is forgotten at once.
        super::present::note_rtv(rtv.ptr, std::ptr::null_mut());
        let constants = [look as u32, flags, src.0, src.1, dst.0, dst.1];
        list.SetGraphicsRootSignature(&self.rs);
        list.SetDescriptorHeaps(&[Some(self.srv.clone())]);
        list.SetGraphicsRoot32BitConstants(0, 6, constants.as_ptr() as _, 0);
        list.SetGraphicsRootDescriptorTable(1, self.srv.GetGPUDescriptorHandleForHeapStart());
        list.SetPipelineState(&self.pso);
        list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list.RSSetViewports(&[D3D12_VIEWPORT { TopLeftX: 0.0, TopLeftY: 0.0, Width: dst.0 as f32, Height: dst.1 as f32, MinDepth: 0.0, MaxDepth: 1.0 }]);
        list.RSSetScissorRects(&[RECT { left: 0, top: 0, right: dst.0 as i32, bottom: dst.1 as i32 }]);
        list.OMSetRenderTargets(1, Some(&rtv as *const _), false, None);
        list.DrawInstanced(3, 1, 0, 0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn target(res: usize, width: u64, height: u32, format: i32) -> Option<Bound> {
        Some(Bound { res, width, height, format: DXGI_FORMAT(format) })
    }

    /// The title's G-buffer pass: six 2560x1440 targets (29, 28, 28, 28, 34, 28) from `first`
    /// on, and its 2560x1440 depth buffer.
    fn title_pass(first: usize, binds: u32) -> Binding {
        let rts = [29, 28, 28, 28, 34, 28].iter().enumerate().map(|(k, &f)| target(first + k, 2560, 1440, f)).collect();
        Binding { rts, dsv: target(0xD0, 2560, 1440, 19), binds }
    }

    #[test]
    fn the_switch_names_a_view_or_a_target_number() {
        assert_eq!([parse("albedo"), parse(" Lit "), parse("DEPTH"), parse("0"), parse("31")], [Some(Show::Albedo), Some(Show::Lit), Some(Show::Depth), Some(Show::Target(0)), Some(Show::Target(31))]);
        assert_eq!([parse(""), parse("normal"), parse("-1"), parse("on")], [None; 4]);
    }

    #[test]
    fn the_g_buffer_pass_is_five_targets_of_one_size_and_a_depth_buffer_of_it() {
        let ui = Binding { rts: vec![target(0x10, 3840, 2160, 28)], dsv: None, binds: 40 };
        let decals = Binding { rts: vec![target(0xA0, 2560, 1440, 29), target(0xA1, 2560, 1440, 28), target(0xA4, 2560, 1440, 34)], dsv: target(0xD0, 2560, 1440, 19), binds: 2 };
        let lighting = Binding { rts: vec![target(0x20, 2560, 1440, 10), target(0x21, 2560, 1440, 42)], dsv: target(0xD1, 2560, 1440, 44), binds: 1 };
        let frame = vec![ui.clone(), decals.clone(), title_pass(0xA0, 8), lighting.clone()];
        assert_eq!(gbuffer(&frame), Some(&frame[2]));
        assert_eq!(gbuffer(&[ui, decals, lighting]), None, "no binding of five targets");

        // five color targets of the size of target 0 and a depth buffer of it, at least
        let mut b = title_pass(0xA0, 1);
        b.rts.truncate(5);
        assert!(gbuffer(&[b.clone()]).is_some(), "five");
        b.rts[4] = target(0xA4, 1280, 720, 34);
        assert!(gbuffer(&[b.clone()]).is_none(), "four of the size, one of another");
        b.rts[4] = None;
        assert!(gbuffer(&[b]).is_none(), "four and an empty slot");
        let mut b = title_pass(0xA0, 1);
        b.dsv = None;
        assert!(gbuffer(&[b.clone()]).is_none(), "no depth buffer");
        b.dsv = target(0xD0, 1280, 720, 19);
        assert!(gbuffer(&[b]).is_none(), "a depth buffer of another size");
        let mut b = title_pass(0xA0, 1);
        b.rts[1] = None;
        b.rts.push(target(0xA9, 2560, 1440, 28));
        assert!(gbuffer(&[b]).is_none(), "no target 1");
        let small = Binding { rts: (0..6).map(|k| target(0xB0 + k, 320, 180, 28)).collect(), dsv: target(0xD2, 320, 180, 19), binds: 9 };
        assert!(gbuffer(&[small.clone()]).is_none(), "smaller than 640x360");

        // of several: the largest, then the one set most often, then the first
        let mut half = title_pass(0xC0, 50);
        half.rts.iter_mut().flatten().for_each(|t| (t.width, t.height) = (1280, 720));
        half.dsv = target(0xD3, 1280, 720, 19);
        let frame = vec![small, half, title_pass(0xA0, 1), title_pass(0xE0, 8), title_pass(0xF0, 8)];
        assert_eq!(gbuffer(&frame).and_then(|b| b.rts[0]).map(|t| t.res), Some(0xE0));
    }

    /// The CPU's reading of render target 1, as the shader's `gbuffer_normal`.
    fn gbuffer_normal(texel: [u8; 4]) -> [f32; 3] {
        let v = texel[1] as u32 | (texel[2] as u32) << 8 | (texel[3] as u32) << 16;
        let e = |c: u32| c as f32 / 4095.0 * 2.0 - 1.0;
        let (x, y) = (e(v & 0xFFF), e(v >> 12));
        let z = 1.0 - x.abs() - y.abs();
        let sign = |v: f32| if v >= 0.0 { 1.0 } else { -1.0 };
        let (x, y) = if z < 0.0 { ((1.0 - y.abs()) * sign(x), (1.0 - x.abs()) * sign(y)) } else { (x, y) };
        let l = (x * x + y * y + z * z).sqrt();
        [x / l, y / l, z / l]
    }

    /// `n` (a unit vector) as the title packs it into render target 1 (R: `r`).
    fn pack_normal(n: [f32; 3], r: u8) -> [u8; 4] {
        let l1 = n[0].abs() + n[1].abs() + n[2].abs();
        let (mut x, mut y) = (n[0] / l1, n[1] / l1);
        if n[2] < 0.0 {
            let sign = |v: f32| if v >= 0.0 { 1.0 } else { -1.0 };
            (x, y) = ((1.0 - y.abs()) * sign(x), (1.0 - x.abs()) * sign(y));
        }
        let q = |v: f32| ((v * 0.5 + 0.5) * 4095.0).round() as u32;
        let v = q(x) | q(y) << 12;
        [r, v as u8, (v >> 8) as u8, (v >> 16) as u8]
    }

    fn unit(v: [f32; 3]) -> [f32; 3] {
        let l = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt();
        v.map(|c| c / l)
    }

    fn dot(a: [f32; 3], b: [f32; 3]) -> f32 {
        a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
    }

    #[test]
    fn normals_round_trip_through_the_12_bit_octahedral_packing() {
        // the axes, the octahedron's faces (each octant: the lower half is folded), and more
        let mut known = vec![[1.0, 0.0, 0.0], [-1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, -1.0, 0.0], [0.0, 0.0, 1.0], [0.0, 0.0, -1.0], [0.3, -0.9, 0.1], [-0.2, 0.05, -0.97]];
        for sx in [-1.0, 1.0] {
            for sy in [-1.0, 1.0] {
                for sz in [-1.0, 1.0] {
                    known.push([sx, sy, sz]);
                    known.push([0.2 * sx, 0.7 * sy, 0.4 * sz]);
                }
            }
        }
        let mut seed = 12345u32;
        let mut next = || {
            seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
            (seed >> 8) as f32 / (1 << 24) as f32 * 2.0 - 1.0
        };
        known.extend((0..2000).map(|_| [next(), next(), next()]).filter(|v| dot(*v, *v) > 1e-3));
        for v in known {
            let n = unit(v);
            let got = gbuffer_normal(pack_normal(n, 0x5A));
            assert!((dot(got, got) - 1.0).abs() < 1e-5, "{n:?}: {got:?} is not of length 1");
            assert!(dot(got, n) > 0.99999, "{n:?} came back as {got:?}");
        }
        // the layout: x = 2560 (0.25), y = 1536 (-0.25) are G | B << 8 | A << 16 = 0x600A00;
        // R is not part of it
        for r in [0, 0xFF] {
            let got = gbuffer_normal([r, 0x00, 0x0A, 0x60]);
            assert!(dot(got, unit([0.25, -0.25, 0.5])) > 0.9999, "{got:?}");
        }
        assert_eq!(pack_normal([0.0, 0.0, 1.0], 0)[1..], [0x00, 0x08, 0x80], "(0, 0, 1) is 2048 in both ranges");
    }

    #[test]
    fn targets_are_read_through_views_of_their_format() {
        assert_eq!(view_of(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false), Some((DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, Look::Linear)));
        assert_eq!(view_of(DXGI_FORMAT_R8G8B8A8_TYPELESS, false), Some((DXGI_FORMAT_R8G8B8A8_UNORM, Look::Stored)));
        assert_eq!(view_of(DXGI_FORMAT_R16G16_FLOAT, false), Some((DXGI_FORMAT_R16G16_FLOAT, Look::Normal)));
        assert_eq!(view_of(DXGI_FORMAT_R32G32_TYPELESS, false), Some((DXGI_FORMAT_R32G32_UINT, Look::Id)));
        // depth buffers: their depth plane; a typed depth format has no shader view
        assert_eq!(view_of(DXGI_FORMAT_R32G8X24_TYPELESS, true), Some((DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, Look::Scalar)));
        assert_eq!(view_of(DXGI_FORMAT_R24G8_TYPELESS, true), Some((DXGI_FORMAT_R24_UNORM_X8_TYPELESS, Look::Scalar)));
        assert_eq!((view_of(DXGI_FORMAT_R16_TYPELESS, true), view_of(DXGI_FORMAT_R16_TYPELESS, false)), (Some((DXGI_FORMAT_R16_UNORM, Look::Scalar)), Some((DXGI_FORMAT_R16_FLOAT, Look::Scalar))));
        for f in [DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_BC1_UNORM] {
            assert_eq!(view_of(f, true), None, "{f:?}");
        }
        // the albedo is read as sRGB where it can be
        assert_eq!(albedo_view(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), Some((DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false)));
        assert_eq!(albedo_view(DXGI_FORMAT_R8G8B8A8_TYPELESS), Some((DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false)));
        assert_eq!(albedo_view(DXGI_FORMAT_R8G8B8A8_UNORM), Some((DXGI_FORMAT_R8G8B8A8_UNORM, true)));
        assert_eq!(albedo_view(DXGI_FORMAT_R16G16_FLOAT), None);
    }

    fn srgb(c: f64) -> f64 {
        if c <= 0.04045 { c / 12.92 } else { ((c + 0.055) / 1.055).powf(2.4) }
    }

    fn unsrgb(c: f64) -> f64 {
        if c <= 0.0031308 { c * 12.92 } else { 1.055 * c.powf(1.0 / 2.4) - 0.055 }
    }

    fn ramp(d: f32) -> f64 {
        if d > 0.0 { (1.0 + (d as f64).log2() / 16.0).clamp(0.0, 1.0) } else { 0.0 }
    }

    /// A `w`x`h` texture of `format` holding `texels` (`bytes` each), in PIXEL_SHADER_RESOURCE,
    /// uploaded through `list` (the upload buffer comes with it: it must live until `list` ran).
    unsafe fn texture(dev: &ID3D12Device, list: &ID3D12GraphicsCommandList, format: DXGI_FORMAT, w: u32, h: u32, bytes: usize, texels: &[u8]) -> (ID3D12Resource, ID3D12Resource) {
        let desc = D3D12_RESOURCE_DESC { Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D, Width: w as u64, Height: h, DepthOrArraySize: 1, MipLevels: 1, Format: format, SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 }, ..Default::default() };
        let mut tex: Option<ID3D12Resource> = None;
        dev.CreateCommittedResource(&D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() }, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, None, &mut tex).unwrap();
        let tex = tex.unwrap();
        let (mut fp, mut size) = (D3D12_PLACED_SUBRESOURCE_FOOTPRINT::default(), 0u64);
        dev.GetCopyableFootprints(&desc, 0, 1, 0, Some(&mut fp), None, None, Some(&mut size));
        let buf = D3D12_RESOURCE_DESC { Dimension: D3D12_RESOURCE_DIMENSION_BUFFER, Width: size, Height: 1, DepthOrArraySize: 1, MipLevels: 1, SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 }, Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR, ..Default::default() };
        let mut up: Option<ID3D12Resource> = None;
        dev.CreateCommittedResource(&D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_UPLOAD, ..Default::default() }, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_GENERIC_READ, None, &mut up).unwrap();
        let up = up.unwrap();
        let mut p = std::ptr::null_mut();
        up.Map(0, None, Some(&mut p)).unwrap();
        for y in 0..h as usize {
            let row = &texels[y * w as usize * bytes..][..w as usize * bytes];
            std::ptr::copy_nonoverlapping(row.as_ptr(), (p as *mut u8).add(fp.Offset as usize + y * fp.Footprint.RowPitch as usize), row.len());
        }
        up.Unmap(0, None);
        let dst = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(&tex), Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: 0 } };
        let src = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(&up), Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { PlacedFootprint: fp } };
        list.CopyTextureRegion(&dst, 0, 0, 0, &src, None);
        list.ResourceBarrier(&[super::super::present::transition(&tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)]);
        (tex, up)
    }

    /// On a GPU (else WARP): a 4x1 G-buffer – the sky, an albedo lit from the light's side, from
    /// the other side, at a slant – drawn as `albedo`, `lit` and `depth` into an 8x2 back buffer
    /// (each texel two pixels wide and tall), as the CPU reads it.
    #[test]
    fn the_views_draw_the_g_buffer_as_the_cpu_reads_it() {
        use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIAdapter, IDXGIFactory4};
        unsafe {
            let mut dev: Option<ID3D12Device> = None;
            if D3D12CreateDevice(None, D3D_FEATURE_LEVEL_11_0, &mut dev).is_err() {
                let warp = CreateDXGIFactory1::<IDXGIFactory4>().and_then(|f| f.EnumWarpAdapter::<IDXGIAdapter>());
                if warp.is_err() || D3D12CreateDevice(&warp.unwrap(), D3D_FEATURE_LEVEL_11_0, &mut dev).is_err() {
                    crate::gpu::recomp::runner::skip_test("no GPU");
                    return;
                }
            }
            let dev = dev.unwrap();
            if crate::gpu::recomp::dxc::location().is_err() {
                crate::gpu::recomp::runner::skip_test("no DXC");
                return;
            }
            let mut viewer = Viewer::new(&dev, DXGI_FORMAT_R8G8B8A8_UNORM).unwrap();
            let light = unit([0.0, 0.6, 0.8]);
            let slant = unit([0.0, 0.6 * 0.5, 0.8 * 0.5 + 0.7]);
            let albedo: [[u8; 4]; 4] = [[0, 0, 0, 0], [200, 100, 50, 255], [200, 100, 50, 255], [90, 180, 30, 255]];
            let normals = [[0; 4], pack_normal(light, 0), pack_normal(light.map(|c| -c), 0xFF), pack_normal(slant, 0)];
            let depth = [0.0f32, 0.5, 0.01, 1e-4];
            let queue: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC { Type: D3D12_COMMAND_LIST_TYPE_DIRECT, ..Default::default() }).unwrap();
            let fence: ID3D12Fence = dev.CreateFence(0, D3D12_FENCE_FLAG_NONE).unwrap();
            let alloc: ID3D12CommandAllocator = dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT).unwrap();
            let list: ID3D12GraphicsCommandList = dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &alloc, None).unwrap();
            let (rt0, _u0) = texture(&dev, &list, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4, 1, 4, albedo.as_flattened());
            let (rt1, _u1) = texture(&dev, &list, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 1, 4, normals.as_flattened());
            let depth_bytes: Vec<u8> = depth.iter().flat_map(|d| d.to_le_bytes()).collect();
            let (ds, _u2) = texture(&dev, &list, DXGI_FORMAT_R32_FLOAT, 4, 1, 4, &depth_bytes);
            let (w, h) = (8u32, 2u32);
            let mut value = 0;
            for look in [Look::Albedo, Look::Lit, Look::Depth] {
                let desc = D3D12_RESOURCE_DESC { Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D, Width: w as u64, Height: h, DepthOrArraySize: 1, MipLevels: 1, Format: DXGI_FORMAT_R8G8B8A8_UNORM, SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 }, Flags: D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, ..Default::default() };
                let mut bb: Option<ID3D12Resource> = None;
                dev.CreateCommittedResource(&D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() }, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, None, &mut bb).unwrap();
                let bb = bb.unwrap();
                let regs = [Some((&rt0, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)), Some((&rt1, DXGI_FORMAT_R8G8B8A8_UNORM)), Some((&ds, DXGI_FORMAT_R32_FLOAT)), None];
                viewer.draw(&dev, &list, look, 0, &regs, (4, 1), &bb, (w, h));
                list.ResourceBarrier(&[super::super::present::transition(&bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE)]);
                let (rb, fp) = super::super::present::readback_copy(&list, &bb, &desc).unwrap();
                list.Close().unwrap();
                queue.ExecuteCommandLists(&[Some(list.cast().unwrap())]);
                value += 1;
                queue.Signal(&fence, value).unwrap();
                crate::gpu::fence::wait(&fence, value, &dev, None, 30_000).unwrap();
                let mut p = std::ptr::null_mut();
                rb.Map(0, None, Some(&mut p)).unwrap();
                for (x, y) in (0..h).flat_map(|y| (0..w).map(move |x| (x, y))) {
                    let got = std::slice::from_raw_parts((p as *const u8).add(fp.Offset as usize + y as usize * fp.Footprint.RowPitch as usize + 4 * x as usize), 3);
                    let k = x as usize / 2;
                    let want: [f64; 3] = match look {
                        Look::Albedo => albedo[k].map(|c| c as f64)[..3].try_into().unwrap(),
                        Look::Lit if depth[k] > 0.0 => {
                            let l = 0.25 + 0.75 * dot(gbuffer_normal(normals[k]), light).max(0.0) as f64;
                            [0, 1, 2].map(|c| 255.0 * unsrgb((srgb(albedo[k][c] as f64 / 255.0) * l).min(1.0)))
                        }
                        Look::Lit => [0.02, 0.03, 0.05].map(|c| 255.0 * unsrgb(c)),
                        _ => [255.0 * ramp(depth[k]); 3],
                    };
                    for c in 0..3 {
                        assert!((got[c] as f64 - want[c]).abs() <= 1.5, "{look:?}: pixel ({x}, {y}) is {got:?}, the CPU reads {want:?}");
                    }
                }
                rb.Unmap(0, None);
                alloc.Reset().unwrap();
                list.Reset(&alloc, None).unwrap();
            }
            viewer.done();
        }
    }
}
