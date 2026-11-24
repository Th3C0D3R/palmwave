//! `XCL_FRAME_CENSUS=n`: what one frame's draws and dispatches were, and what the GPU ran of
//! them.
//!
//! A retail title's first world frames record thousands of draws and dispatches and leave the
//! render targets and the depth at their cleared values. With the variable set, every Draw,
//! DispatchMesh, Dispatch and ExecuteIndirect console code records for the frame presented as
//! the n-th (numbered as the `rt_<n>` dumps are; `XCL_FRAME_CENSUS_FROM=n`: that frame and the
//! two after it) is kept with what was bound when it was recorded:
//!
//! * the queue and the PC list it ran on, in submission order;
//! * its pipeline – the hash of each console shader (`gpu::shader::fnv64`, as in the other log
//!   lines), and whether it is translated, a two-pass mesh pipeline, a PC one, or a stand-in
//!   (still being built, or whose build failed) – and why XCL left the operation out, if it did;
//! * the render targets and the depth target (resource, title address, format, size), the first
//!   viewport and scissor, and the depth test, function, write, stencil and cull mode of the
//!   title's pipeline desc;
//! * its counts. An ExecuteIndirect's are on the GPU: its count buffer and the start of its
//!   argument buffer are copied to a READBACK buffer right after it, in the same list, and
//!   read when the queue has run the list.
//!
//! ClearRenderTargetView and ClearDepthStencilView are kept too, in their place among the
//! draws. When the frame is presented and its lists are done (30 s at most), a thread logs
//! each ExecuteIndirect's counts and a summary – per target how many draws aimed at it, how
//! many of them had counts that draw something, how many ran with a stand-in – then the depth
//! states, clear values, viewports and scissors seen, and writes one row an operation to
//! `census_<frame>.csv` in the logs folder.
//!
//! `XCL_FRAME_CENSUS_MIN_OPS=m`: the frame where a title's world starts is not the same from
//! run to run. With it, every frame from n on is recorded, one with fewer than m operations
//! is dropped at its present (nothing logged, nothing written, its records freed and its
//! READBACK buffers given to the next recordings), and the census ends with the first frame
//! that has m (`XCL_FRAME_CENSUS`) or the first three (`XCL_FRAME_CENSUS_FROM`).
//!
//! Unset, `recording` returns at its first test and nothing else here has anything to do.

use std::collections::HashMap;
use std::ffi::c_void;
use std::fmt::Write as _;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};

use windows::core::Interface;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::DXGI_SAMPLE_DESC;

/// frames recorded from `XCL_FRAME_CENSUS_FROM`
const MAX_FRAMES: u64 = 3;
/// bytes of a READBACK buffer of a recording
const CHUNK: u32 = 64 << 10;
/// READBACK buffers of a recording: past them its ExecuteIndirects are not read back
const MAX_CHUNKS: usize = 64;
/// commands of an ExecuteIndirect copied at most
const MAX_COMMANDS: u32 = 1024;
/// how long a frame's lists are waited for once it is presented
const WAIT: Duration = Duration::from_secs(30);
/// ExecuteIndirects logged a frame (all are in the CSV), and commands shown of each
const LOG_INDIRECTS: usize = 256;
const LOG_COMMANDS: usize = 16;
const CSV_COMMANDS: usize = 64;
const PC_GET_TYPE: usize = 8;
const PC_COPY_BUFFER_REGION: usize = 15;

/// The frames recorded: `count` from `first` – with `min_ops`, the first `count` from `first`
/// that have that many operations.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Settings {
    first: u64,
    count: u64,
    min_ops: u64,
}

impl Settings {
    /// Whether frame `frame` is recorded, `kept` frames having been reported so far.
    fn wants(&self, frame: u64, kept: u64) -> bool {
        frame >= self.first && if self.min_ops == 0 { frame - self.first < self.count } else { kept < self.count }
    }

    /// Whether operations are kept while `presents` frames have been shown: during a frame
    /// that is wanted, and the one before (a list recorded then may be submitted in it).
    fn records(&self, presents: u64, kept: u64) -> bool {
        presents + 1 >= self.first && if self.min_ops == 0 { presents < self.first.saturating_add(self.count) } else { kept < self.count }
    }

    /// Whether a frame that recorded `ops` operations is reported (not: dropped).
    fn keeps(&self, ops: u64) -> bool {
        ops >= self.min_ops
    }
}

/// None: the census is off. `one`: `XCL_FRAME_CENSUS` (0 or not a number: off), `from`:
/// `XCL_FRAME_CENSUS_FROM`, which comes first; `min_ops`: `XCL_FRAME_CENSUS_MIN_OPS`.
fn schedule(one: Option<&str>, from: Option<&str>, min_ops: Option<&str>) -> Option<Settings> {
    let number = |v: Option<&str>| v.and_then(|v| v.trim().parse::<u64>().ok());
    let min_ops = number(min_ops).unwrap_or(0);
    if let Some(first) = number(from) {
        return Some(Settings { first, count: MAX_FRAMES, min_ops });
    }
    number(one).filter(|&n| n > 0).map(|first| Settings { first, count: 1, min_ops })
}

fn settings() -> Option<Settings> {
    static S: OnceLock<Option<Settings>> = OnceLock::new();
    *S.get_or_init(|| {
        let var = |name: &str| std::env::var(name).ok();
        schedule(var("XCL_FRAME_CENSUS").as_deref(), var("XCL_FRAME_CENSUS_FROM").as_deref(), var("XCL_FRAME_CENSUS_MIN_OPS").as_deref())
    })
}

/// frames reported so far (those `Settings::keeps`)
static KEPT: AtomicU64 = AtomicU64::new(0);

pub(super) fn enabled() -> bool {
    settings().is_some()
}

/// Whether operations recorded now are kept.
pub(super) fn recording() -> bool {
    settings().is_some_and(|s| s.records(super::present::presents(), KEPT.load(Ordering::Relaxed)))
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(super) enum Kind {
    Draw,
    DrawIndexed,
    DispatchMesh,
    Dispatch,
    ExecuteIndirect,
    ClearColor,
    ClearDepth,
}

impl Kind {
    const ALL: [Kind; 7] = [Kind::Draw, Kind::DrawIndexed, Kind::DispatchMesh, Kind::Dispatch, Kind::ExecuteIndirect, Kind::ClearColor, Kind::ClearDepth];

    fn label(self) -> &'static str {
        match self {
            Kind::Draw => "DrawInstanced",
            Kind::DrawIndexed => "DrawIndexedInstanced",
            Kind::DispatchMesh => "DispatchMesh",
            Kind::Dispatch => "Dispatch",
            Kind::ExecuteIndirect => "ExecuteIndirect",
            Kind::ClearColor => "ClearRenderTargetView",
            Kind::ClearDepth => "ClearDepthStencilView",
        }
    }

    fn clear(self) -> bool {
        matches!(self, Kind::ClearColor | Kind::ClearDepth)
    }
}

/// What a command of an ExecuteIndirect is.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
enum ArgKind {
    Draw,
    DrawIndexed,
    Dispatch,
    DispatchMesh,
}

impl ArgKind {
    /// of a D3D12_INDIRECT_ARGUMENT_TYPE (None: not one that draws or dispatches)
    fn of(ty: u32) -> Option<ArgKind> {
        Some(match ty {
            0 => ArgKind::Draw,
            1 => ArgKind::DrawIndexed,
            2 => ArgKind::Dispatch,
            10 => ArgKind::DispatchMesh,
            _ => return None,
        })
    }

    fn name(self) -> &'static str {
        match self {
            ArgKind::Draw => "draw",
            ArgKind::DrawIndexed => "indexed",
            ArgKind::Dispatch => "dispatch",
            ArgKind::DispatchMesh => "mesh",
        }
    }

    /// Whether a command with these counts draws or runs anything.
    fn nonzero(self, c: [u32; 3]) -> bool {
        match self {
            ArgKind::Draw | ArgKind::DrawIndexed => c[0] > 0 && c[1] > 0,
            ArgKind::Dispatch | ArgKind::DispatchMesh => c.iter().all(|&n| n > 0),
        }
    }

    fn counts(self, c: [u32; 3]) -> String {
        match self {
            ArgKind::Draw | ArgKind::DrawIndexed => format!("{}x{}", c[0], c[1]),
            ArgKind::Dispatch | ArgKind::DispatchMesh => format!("{}x{}x{}", c[0], c[1], c[2]),
        }
    }
}

/// A command signature: the bytes from one command to the next, and its draw or dispatch
/// argument with its offset in a command (None: it has none XCL knows).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Signature {
    stride: u32,
    arg: Option<(ArgKind, u32)>,
}

/// The signature of these D3D12_INDIRECT_ARGUMENT_DESCs (type first) with this byte stride.
fn signature(stride: u32, args: &[[u32; 4]]) -> Signature {
    let mut offset = 0u32;
    for a in args {
        if let Some(kind) = ArgKind::of(a[0]) {
            return Signature { stride, arg: Some((kind, offset)) };
        }
        offset = offset.saturating_add(super::device_x::indirect_arg_bytes(a));
    }
    Signature { stride, arg: None }
}

static SIGNATURES: Mutex<Option<HashMap<usize, Signature>>> = Mutex::new(None);

/// The command signature `sig` was made from these arguments.
pub(super) fn note_signature(sig: usize, stride: u32, args: &[[u32; 4]]) {
    if enabled() && sig != 0 {
        SIGNATURES.lock().unwrap().get_or_insert_with(HashMap::new).insert(sig, signature(stride, args));
    }
}

/// The counts of the first `n` commands in `bytes` (an argument buffer from its first command):
/// vertices or indices and instances of a draw, the thread groups of a dispatch. Fewer when
/// `bytes` ends first.
fn decode(bytes: &[u8], sig: Signature, n: u32) -> Vec<[u32; 3]> {
    let Some((kind, offset)) = sig.arg else { return Vec::new() };
    let words = if matches!(kind, ArgKind::Draw | ArgKind::DrawIndexed) { 2 } else { 3 };
    let mut out = Vec::new();
    for i in 0..n as usize {
        let at = i * sig.stride as usize + offset as usize;
        let Some(b) = bytes.get(at..at + 4 * words) else { break };
        let mut c = [0u32; 3];
        for (k, w) in b.chunks_exact(4).enumerate() {
            c[k] = u32::from_le_bytes(w.try_into().unwrap());
        }
        out.push(c);
        if sig.stride == 0 {
            break;
        }
    }
    out
}

/// The commands the GPU runs of an ExecuteIndirect: its count buffer's value, `max` at most.
fn executed_commands(count: Option<u32>, max: u32) -> u32 {
    count.map_or(max, |c| c.min(max))
}

/// The bytes copied of an argument buffer with `available` bytes from its first command.
fn copy_bytes(stride: u32, max: u32, available: u64) -> u32 {
    (max.min(MAX_COMMANDS) as u64 * stride as u64).min(available).min(CHUNK as u64 - 4) as u32
}

/// A render target or depth target as it was when a draw was recorded.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
struct Target {
    /// the PC resource
    res: usize,
    /// where the title has it (0: not known)
    addr: usize,
    format: i32,
    width: u64,
    height: u32,
}

impl Target {
    fn text(&self) -> String {
        format!("{:#x}@{:#x}:{}:{}x{}", self.res, self.addr, self.format, self.width, self.height)
    }
}

/// The fixed-function state of the title's pipeline desc that decides whether a draw can
/// leave anything in its targets.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct DepthState {
    test: bool,
    /// D3D12_COMPARISON_FUNC
    func: i32,
    write: bool,
    stencil: bool,
    /// D3D12_CULL_MODE
    cull: i32,
}

fn func_name(f: i32) -> String {
    ["NEVER", "LESS", "EQUAL", "LESS_EQUAL", "GREATER", "NOT_EQUAL", "GREATER_EQUAL", "ALWAYS"].get(f.wrapping_sub(1) as usize).map_or_else(|| f.to_string(), |s| s.to_string())
}

fn cull_name(c: i32) -> String {
    ["NONE", "FRONT", "BACK"].get(c.wrapping_sub(1) as usize).map_or_else(|| c.to_string(), |s| s.to_string())
}

fn on(b: bool) -> &'static str {
    if b {
        "on"
    } else {
        "off"
    }
}

impl DepthState {
    fn text(&self) -> String {
        format!("test {}, {}, write {}, stencil {}, cull {}", on(self.test), func_name(self.func), on(self.write), on(self.stencil), cull_name(self.cull))
    }
}

/// What the title made a pipeline state from.
#[derive(Clone, Debug, Default, PartialEq)]
struct Pipeline {
    /// stage and `fnv64` of its shader
    stages: Vec<(&'static str, u64)>,
    /// None: a compute pipeline
    state: Option<DepthState>,
}

impl Pipeline {
    fn shaders(&self) -> String {
        self.stages.iter().map(|s| format!("{} {:016x}", s.0, s.1)).collect::<Vec<_>>().join(" ")
    }
}

static PIPELINES: Mutex<Option<HashMap<usize, Arc<Pipeline>>>> = Mutex::new(None);

unsafe fn hashed(code: usize, len: usize) -> Option<u64> {
    (code != 0 && len > 0 && crate::gpu::shader::readable(code, len)).then(|| crate::gpu::shader::fnv64(std::slice::from_raw_parts(code as *const u8, len)))
}

unsafe fn stage(stages: &mut Vec<(&'static str, u64)>, name: &'static str, b: &D3D12_SHADER_BYTECODE) {
    if let Some(h) = hashed(b.pShaderBytecode as usize, b.BytecodeLength) {
        stages.push((name, h));
    }
}

fn keep_pipeline(hr: i32, ppv: *mut *mut c_void, info: impl FnOnce() -> Option<Pipeline>) {
    if hr < 0 || ppv.is_null() {
        return;
    }
    let pso = unsafe { *ppv } as usize;
    if pso == 0 {
        return;
    }
    if let Some(p) = info() {
        PIPELINES.lock().unwrap().get_or_insert_with(HashMap::new).insert(pso, Arc::new(p));
    }
}

/// CreateGraphicsPipelineState gave the title `*ppv` for `desc` (a PC graphics desc).
pub(super) unsafe fn note_graphics_pso(hr: i32, ppv: *mut *mut c_void, desc: *const c_void) {
    if !enabled() {
        return;
    }
    keep_pipeline(hr, ppv, || {
        if !crate::gpu::shader::readable(desc as usize, std::mem::size_of::<D3D12_GRAPHICS_PIPELINE_STATE_DESC>()) {
            return None;
        }
        let d = std::ptr::read_unaligned(desc as *const D3D12_GRAPHICS_PIPELINE_STATE_DESC);
        let mut stages = Vec::new();
        for (name, b) in [("VS", &d.VS), ("HS", &d.HS), ("DS", &d.DS), ("GS", &d.GS), ("PS", &d.PS)] {
            stage(&mut stages, name, b);
        }
        let z = &d.DepthStencilState;
        let state = DepthState { test: z.DepthEnable.as_bool(), func: z.DepthFunc.0, write: z.DepthWriteMask.0 != 0, stencil: z.StencilEnable.as_bool(), cull: d.RasterizerState.CullMode.0 };
        Some(Pipeline { stages, state: Some(state) })
    });
}

/// CreateComputePipelineState gave the title `*ppv` for `desc`.
pub(super) unsafe fn note_compute_pso(hr: i32, ppv: *mut *mut c_void, desc: *const c_void) {
    if !enabled() {
        return;
    }
    keep_pipeline(hr, ppv, || {
        if !crate::gpu::shader::readable(desc as usize, std::mem::size_of::<D3D12_COMPUTE_PIPELINE_STATE_DESC>()) {
            return None;
        }
        let d = std::ptr::read_unaligned(desc as *const D3D12_COMPUTE_PIPELINE_STATE_DESC);
        let mut stages = Vec::new();
        stage(&mut stages, "CS", &d.CS);
        Some(Pipeline { stages, state: None })
    });
}

/// CreatePipelineState gave the title `*ppv` for the stream `desc`.
pub(super) unsafe fn note_stream_pso(hr: i32, ppv: *mut *mut c_void, desc: *const c_void) {
    if !enabled() {
        return;
    }
    keep_pipeline(hr, ppv, || {
        if !crate::gpu::shader::readable(desc as usize, 16) {
            return None;
        }
        let (size, base) = ((desc as *const usize).read_unaligned(), (desc as *const usize).add(1).read_unaligned());
        if !crate::gpu::shader::readable(base, size) {
            return None;
        }
        Some(stream_pipeline(std::slice::from_raw_parts(base as *const u8, size), |code, len| hashed(code, len)))
    });
}

/// The pipeline of a pipeline state stream (`hash`: of the shader at an address, of a
/// length). What a stream leaves out has D3D12's defaults.
fn stream_pipeline(buf: &[u8], hash: impl Fn(usize, usize) -> Option<u64>) -> Pipeline {
    let mut stages = Vec::new();
    let mut state = DepthState { test: true, func: D3D12_COMPARISON_FUNC_LESS.0, write: true, stencil: false, cull: D3D12_CULL_MODE_BACK.0 };
    let mut compute = false;
    let word = |p: &[u8], at: usize| p.get(at..at + 4).map_or(0, |b| i32::from_le_bytes(b.try_into().unwrap()));
    let mut off = 0usize;
    while off + 8 <= buf.len() {
        let ty = u32::from_le_bytes(buf[off..off + 4].try_into().unwrap());
        // (a type XCL does not know: the rest cannot be walked)
        let Some((size, align)) = super::null::subobject_payload(ty) else { break };
        let at = off + if align == 8 { 8 } else { 4 };
        let Some(p) = buf.get(at..at + size) else { break };
        match ty {
            1..=6 | 24 | 25 => {
                let name = ["VS", "PS", "DS", "HS", "GS", "CS"].get(ty as usize - 1).copied().unwrap_or(if ty == 24 { "AS" } else { "MS" });
                let (code, len) = (u64::from_le_bytes(p[0..8].try_into().unwrap()) as usize, u64::from_le_bytes(p[8..16].try_into().unwrap()) as usize);
                if let Some(h) = hash(code, len) {
                    stages.push((name, h));
                    compute |= ty == 6;
                }
            }
            // RASTERIZER, RASTERIZER1, RASTERIZER2: fill, cull, …
            10 | 27 | 28 => state.cull = word(p, 4),
            // DEPTH_STENCIL, DEPTH_STENCIL1, DEPTH_STENCIL2: enable, write mask, func, stencil enable, …
            11 | 21 | 26 => {
                state.test = word(p, 0) != 0;
                state.write = word(p, 4) != 0;
                state.func = word(p, 8);
                state.stencil = word(p, 12) != 0;
            }
            _ => {}
        }
        off = (at + size + 7) & !7;
    }
    Pipeline { stages, state: (!compute).then_some(state) }
}

/// The pipeline state is gone (another may get its address).
pub(super) fn forget_pipeline(pso: usize) {
    if enabled() {
        if let Some(m) = PIPELINES.lock().unwrap().as_mut() {
            m.remove(&pso);
        }
    }
}

/// What the pipeline state an operation ran with is.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
enum Status {
    /// built from translated console shaders
    Translated,
    /// a console MS / GS pipeline in two passes
    TwoPass,
    /// a pipeline of the PC's (shaders not the console's, or XCL's fallback)
    Pc,
    /// a stand-in whose pipeline is still being built: it draws nothing
    Building,
    /// a stand-in whose pipeline was not built
    StandIn,
    /// a clear
    Clear,
}

impl Status {
    const ALL: [Status; 5] = [Status::Translated, Status::TwoPass, Status::Pc, Status::Building, Status::StandIn];

    fn name(self) -> &'static str {
        match self {
            Status::Translated => "translated",
            Status::TwoPass => "two-pass",
            Status::Pc => "pc",
            Status::Building => "stand-in (building)",
            Status::StandIn => "stand-in (not built)",
            Status::Clear => "",
        }
    }

    fn standin(self) -> bool {
        matches!(self, Status::Building | Status::StandIn)
    }
}

/// `standin`: `pso_async::standin_state` of the pipeline state the title bound; `built`:
/// whether a pipeline state is (a two-pass pipeline, one of translated console shaders).
fn status_of(pso: usize, standin: Option<(usize, bool)>, built: impl Fn(usize) -> (bool, bool)) -> Status {
    let real = match standin {
        None => pso,
        Some((0, true)) => return Status::Building,
        Some((0, false)) => return Status::StandIn,
        Some((real, _)) => real,
    };
    match built(real) {
        (true, _) => Status::TwoPass,
        (false, true) => Status::Translated,
        (false, false) => Status::Pc,
    }
}

/// Where an ExecuteIndirect's count and arguments were copied to in its recording's buffers.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Slot {
    chunk: usize,
    count_at: Option<u32>,
    args_at: u32,
    args_bytes: u32,
}

#[derive(Clone, Debug, Default, PartialEq)]
struct Indirect {
    signature: Option<Signature>,
    max: u32,
    /// a count buffer was given
    counted: bool,
    slot: Option<Slot>,
    /// read back: the count buffer's value, and the counts of the commands the GPU ran (those
    /// copied of them)
    read: bool,
    count: Option<u32>,
    commands: Vec<[u32; 3]>,
}

impl Indirect {
    fn kind(&self) -> Option<ArgKind> {
        self.signature.and_then(|s| s.arg).map(|a| a.0)
    }

    fn executed(&self) -> u32 {
        executed_commands(self.count, self.max)
    }

    fn nonzero(&self) -> usize {
        self.kind().map_or(0, |k| self.commands.iter().filter(|&&c| k.nonzero(c)).count())
    }

    /// What the GPU wrote: `chunk` is the READBACK buffer the slot is in.
    fn read_back(&mut self, chunk: &[u8]) {
        let Some(slot) = self.slot else { return };
        self.count = slot.count_at.and_then(|at| chunk.get(at as usize..at as usize + 4)).map(|b| u32::from_le_bytes(b.try_into().unwrap()));
        let args = chunk.get(slot.args_at as usize..(slot.args_at + slot.args_bytes) as usize).unwrap_or(&[]);
        self.commands = self.signature.map_or(Vec::new(), |s| decode(args, s, self.executed()));
        self.read = true;
    }

    fn commands_text(&self, limit: usize) -> String {
        let Some(k) = self.kind() else { return String::new() };
        let mut v: Vec<String> = self.commands.iter().take(limit).map(|&c| k.counts(c)).collect();
        if self.commands.len() > limit {
            v.push(format!("+{}", self.commands.len() - limit));
        }
        v.join(" ")
    }
}

/// One operation of a recording.
#[derive(Clone, Debug, PartialEq)]
struct Op {
    kind: Kind,
    /// vertices or indices and instances; thread groups; an ExecuteIndirect's maximum count
    counts: [u32; 3],
    /// the pipeline state the title bound
    pso: usize,
    pipeline: Option<Arc<Pipeline>>,
    status: Status,
    /// XCL did not issue it: why
    skipped: Option<&'static str>,
    /// a draw (its targets count); not a compute dispatch
    graphics: bool,
    targets: Vec<Target>,
    /// the depth target, and whether its view only reads
    depth: Option<(Target, bool)>,
    /// x, y, width, height, min depth, max depth
    viewport: Option<[f32; 6]>,
    /// left, top, right, bottom
    scissor: Option<[i32; 4]>,
    indirect: Option<Indirect>,
    /// a clear's value: the color, or depth, stencil and the flags (1 depth, 2 stencil)
    clear: [f32; 4],
}

impl Op {
    #[cfg(test)]
    fn new(kind: Kind, counts: [u32; 3]) -> Op {
        Op {
            kind,
            counts,
            pso: 0,
            pipeline: None,
            status: if kind.clear() { Status::Clear } else { Status::Translated },
            skipped: None,
            graphics: !matches!(kind, Kind::Dispatch),
            targets: Vec::new(),
            depth: None,
            viewport: None,
            scissor: None,
            indirect: None,
            clear: [0.0; 4],
        }
    }

    /// Whether its counts draw or run anything (None: an ExecuteIndirect not read back).
    fn nonzero(&self) -> Option<bool> {
        let c = self.counts;
        match (&self.indirect, self.kind) {
            (Some(i), _) => i.read.then(|| i.nonzero() > 0),
            (None, Kind::Draw | Kind::DrawIndexed) => Some(ArgKind::Draw.nonzero(c)),
            (None, _) => Some(ArgKind::Dispatch.nonzero(c)),
        }
    }

    fn clear_text(&self) -> String {
        let c = self.clear;
        match self.kind {
            Kind::ClearDepth => format!("depth {} stencil {} flags {}", c[0], c[1], c[2]),
            _ => format!("{} {} {} {}", c[0], c[1], c[2], c[3]),
        }
    }
}

/// A READBACK buffer of `CHUNK` bytes, mapped.
struct Buffer {
    device: usize,
    res: ID3D12Resource,
    mapped: usize,
}

// SAFETY: a free-threaded COM object; the mapping lives as long as `res`
unsafe impl Send for Buffer {}

/// Buffers no recording has. One is never released: a list a queue has not run yet may
/// still copy into it (the next recording to take it then reads that copy – lists dropped
/// with copies in them are those of the frame before the first one recorded).
static SPARE: Mutex<Vec<Buffer>> = Mutex::new(Vec::new());

impl Buffer {
    unsafe fn take(dev: &ID3D12Device) -> Option<Buffer> {
        let device = dev.as_raw() as usize;
        let spare = {
            let mut g = SPARE.lock().unwrap();
            g.iter().position(|b| b.device == device).map(|i| g.swap_remove(i))
        };
        if spare.is_some() {
            return spare;
        }
        let hp = D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_READBACK, ..Default::default() };
        let rd = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
            Width: CHUNK as u64,
            Height: 1,
            DepthOrArraySize: 1,
            MipLevels: 1,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            ..Default::default()
        };
        let mut rb: Option<ID3D12Resource> = None;
        dev.CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, None, &mut rb).ok()?;
        let res = rb?;
        let mut p: *mut c_void = std::ptr::null_mut();
        res.Map(0, None, Some(&mut p)).ok()?;
        (!p.is_null()).then_some(Buffer { device, res, mapped: p as usize })
    }

    unsafe fn bytes(&self) -> Vec<u8> {
        std::slice::from_raw_parts(self.mapped as *const u8, CHUNK as usize).to_vec()
    }
}

#[derive(Default)]
struct Chunk {
    used: u32,
    buf: Option<Buffer>,
}

/// What one PC list recorded since it was last submitted or reset.
#[derive(Default)]
struct Recording {
    list: usize,
    ops: Vec<Op>,
    chunks: Vec<Chunk>,
}

impl Drop for Recording {
    fn drop(&mut self) {
        let bufs = self.chunks.drain(..).filter_map(|c| c.buf);
        SPARE.lock().unwrap().extend(bufs);
    }
}

impl Recording {
    /// Room for `bytes` in the recording's buffers: (chunk, offset). A chunk without a buffer
    /// yet is new. None: no room left.
    fn place(&mut self, bytes: u32) -> Option<(usize, u32)> {
        if bytes == 0 || bytes > CHUNK {
            return None;
        }
        if let Some(c) = self.chunks.last_mut() {
            let at = c.used.next_multiple_of(4);
            if at + bytes <= CHUNK {
                c.used = at + bytes;
                return Some((self.chunks.len() - 1, at));
            }
        }
        if self.chunks.len() >= MAX_CHUNKS {
            return None;
        }
        self.chunks.push(Chunk { used: bytes, buf: None });
        Some((self.chunks.len() - 1, 0))
    }
}

/// PC list → its recording.
#[derive(Default)]
struct Book {
    open: HashMap<usize, Recording>,
}

impl Book {
    fn recording(&mut self, list: usize) -> &mut Recording {
        self.open.entry(list).or_insert_with(|| Recording { list, ops: Vec::new(), chunks: Vec::new() })
    }

    fn discard(&mut self, list: usize) {
        self.open.remove(&list);
    }

    /// The recordings of `lists`, which were submitted in this order (a list that recorded
    /// nothing has none).
    fn take(&mut self, lists: &[usize]) -> Vec<Recording> {
        lists.iter().filter_map(|l| self.open.remove(l)).filter(|r| !r.ops.is_empty()).collect()
    }
}

static BOOK: Mutex<Option<Book>> = Mutex::new(None);

fn push(list: *mut c_void, op: Op) {
    BOOK.lock().unwrap().get_or_insert_with(Book::default).recording(list as usize).ops.push(op);
}

/// The list was reset, or is gone: what it recorded and did not submit is dropped.
pub(super) fn discard(list: *mut c_void) {
    if !enabled() {
        return;
    }
    if let Some(b) = BOOK.lock().unwrap().as_mut() {
        b.discard(list as usize);
    }
}

unsafe fn target(res: usize) -> Target {
    let raw = res as *mut c_void;
    let d = ID3D12Resource::from_raw_borrowed(&raw).map(|r| r.GetDesc()).unwrap_or_default();
    let addr = super::real::title_address(res).or_else(|| super::title_textures::address(res)).unwrap_or(0);
    Target { res, addr, format: d.Format.0, width: d.Width, height: d.Height }
}

unsafe fn capture(list: *mut c_void, kind: Kind, counts: [u32; 3], pso: usize, skipped: Option<&'static str>, graphics: bool) -> Op {
    let (mut targets, mut depth, mut viewport, mut scissor) = (Vec::new(), None, None, None);
    if graphics {
        let (vp, sc, rtvs, dsv) = super::list_x::census_state(list);
        viewport = vp.map(|v| [v.TopLeftX, v.TopLeftY, v.Width, v.Height, v.MinDepth, v.MaxDepth]);
        scissor = sc.map(|r| [r.left, r.top, r.right, r.bottom]);
        targets = rtvs.into_iter().filter_map(super::present::rtv_resource).map(|r| target(r)).collect();
        depth = dsv.and_then(super::rt_dump::dsv_resource).map(|(r, read_only)| (target(r), read_only));
    }
    let status = status_of(pso, super::pso_async::standin_state(pso), |real| {
        (super::real::two_pass_of(real).is_some(), super::real::is_translated_graphics(real) || super::real::is_translated_compute(real))
    });
    let pipeline = PIPELINES.lock().unwrap().as_ref().and_then(|m| m.get(&pso).cloned());
    Op { kind, counts, pso, pipeline, status, skipped, graphics, targets, depth, viewport, scissor, indirect: None, clear: [0.0; 4] }
}

/// Console code recorded a draw or dispatch on the PC list `list` with the pipeline state
/// `pso` bound (`skipped`: XCL left it out, and why). Called while `recording`.
pub(super) unsafe fn note(list: *mut c_void, kind: Kind, counts: [u32; 3], pso: usize, skipped: Option<&'static str>) {
    push(list, capture(list, kind, counts, pso, skipped, kind != Kind::Dispatch));
}

/// … an ExecuteIndirect, which the PC list has just recorded (unless `skipped`): its count
/// and its arguments are copied out behind it. `args_off` and `count_off` are the PC
/// resources' (`real::buffer_delta` added).
#[allow(clippy::too_many_arguments)]
pub(super) unsafe fn note_indirect(list: *mut c_void, pso: usize, sig: usize, max: u32, args: usize, args_off: u64, count_buf: usize, count_off: u64, skipped: Option<&'static str>) {
    let signature = SIGNATURES.lock().unwrap().as_ref().and_then(|m| m.get(&sig).copied());
    let graphics = signature.and_then(|s| s.arg).is_none_or(|a| a.0 != ArgKind::Dispatch);
    let mut op = capture(list, Kind::ExecuteIndirect, [max, 0, 0], pso, skipped, graphics);
    let slot = if skipped.is_none() { copy_out(list, signature, max, args, args_off, count_buf, count_off) } else { None };
    op.indirect = Some(Indirect { signature, max, counted: count_buf != 0, slot, ..Default::default() });
    push(list, op);
}

/// CopyBufferRegions of the count and of the first commands into the recording's buffers
/// (None: nothing to copy, no room, or a list that cannot copy).
unsafe fn copy_out(list: *mut c_void, signature: Option<Signature>, max: u32, args: usize, args_off: u64, count_buf: usize, count_off: u64) -> Option<Slot> {
    let ty = std::mem::transmute::<usize, unsafe extern "system" fn(*mut c_void) -> i32>(super::real::orig_fn(list, PC_GET_TYPE))(list);
    if ty != D3D12_COMMAND_LIST_TYPE_DIRECT.0 && ty != D3D12_COMMAND_LIST_TYPE_COMPUTE.0 {
        return None;
    }
    // (a range past the end of a buffer is not copied: that removes the device)
    let width = |res: usize| {
        let raw = res as *mut c_void;
        ID3D12Resource::from_raw_borrowed(&raw).map_or(0, |r| r.GetDesc().Width)
    };
    let args_bytes = match signature {
        Some(s) if args != 0 && s.arg.is_some() => copy_bytes(s.stride, max, width(args).saturating_sub(args_off)),
        _ => 0,
    };
    let count = count_buf != 0 && width(count_buf) >= count_off.saturating_add(4);
    let count_bytes = if count { 4 } else { 0 };
    let (chunk, at, dst) = {
        let mut g = BOOK.lock().unwrap();
        let r = g.get_or_insert_with(Book::default).recording(list as usize);
        let (chunk, at) = r.place(args_bytes + count_bytes)?;
        if r.chunks[chunk].buf.is_none() {
            r.chunks[chunk].buf = super::real::list_device(list).and_then(|d| Buffer::take(&d));
        }
        let Some(b) = &r.chunks[chunk].buf else {
            r.chunks.pop();
            return None;
        };
        (chunk, at, b.res.as_raw() as usize)
    };
    type Copy = unsafe extern "system" fn(*mut c_void, usize, u64, usize, u64, u64);
    let copy: Copy = std::mem::transmute(super::real::orig_fn(list, PC_COPY_BUFFER_REGION));
    if count {
        copy(list, dst, at as u64, count_buf, count_off, 4);
    }
    if args_bytes > 0 {
        copy(list, dst, (at + count_bytes) as u64, args, args_off, args_bytes as u64);
    }
    Some(Slot { chunk, count_at: count.then_some(at), args_at: at + count_bytes, args_bytes })
}

unsafe fn note_clear(list: *mut c_void, kind: Kind, targets: Vec<Target>, depth: Option<(Target, bool)>, clear: [f32; 4]) {
    push(list, Op { kind, counts: [0; 3], pso: 0, pipeline: None, status: Status::Clear, skipped: None, graphics: false, targets, depth, viewport: None, scissor: None, indirect: None, clear });
}

/// ClearRenderTargetView of the view `rtv` on the PC list `list`.
pub(super) unsafe fn clear_color(list: *mut c_void, rtv: usize, color: *const [f32; 4]) {
    if !recording() {
        return;
    }
    let targets = super::present::rtv_resource(rtv).map(|r| target(r)).into_iter().collect();
    note_clear(list, Kind::ClearColor, targets, None, color.as_ref().copied().unwrap_or_default());
}

/// ClearDepthStencilView of the view `dsv` on the PC list `list`.
pub(super) unsafe fn clear_depth(list: *mut c_void, dsv: usize, flags: u32, depth: f32, stencil: u8) {
    if !recording() {
        return;
    }
    let d = super::rt_dump::dsv_resource(dsv).map(|(r, read_only)| (target(r), read_only));
    note_clear(list, Kind::ClearDepth, Vec::new(), d, [depth, stencil as f32, flags as f32, 0.0]);
}

// ---------------------------------------------------------------------------------------
// submission
// ---------------------------------------------------------------------------------------

/// A fence of the census on a queue, done when it reaches the value.
struct Done(ID3D12Fence, u64);

unsafe impl Send for Done {}

/// A recording a queue was given.
struct Submitted {
    queue: usize,
    /// D3D12_COMMAND_LIST_TYPE of the queue
    queue_type: i32,
    /// None: no fence – what the buffers hold is not read
    done: Option<Done>,
    recording: Recording,
}

/// frame → what was submitted while it was made, in order
static FRAMES: Mutex<Option<HashMap<u64, Vec<Submitted>>>> = Mutex::new(None);
/// queue → the census's fence there and the last value signalled
static FENCES: Mutex<Option<HashMap<usize, Done>>> = Mutex::new(None);

/// `lists` were just submitted to `queue`.
pub(super) unsafe fn executed(queue: *mut c_void, lists: &[*mut c_void]) {
    let Some(settings) = settings() else { return };
    let recordings = {
        let mut g = BOOK.lock().unwrap();
        let Some(b) = g.as_mut() else { return };
        let lists: Vec<usize> = lists.iter().map(|&l| l as usize).collect();
        b.take(&lists)
    };
    let frame = super::present::presents();
    if recordings.is_empty() || !settings.wants(frame, KEPT.load(Ordering::Relaxed)) {
        return;
    }
    let Some(q) = ID3D12CommandQueue::from_raw_borrowed(&queue) else { return };
    let queue_type = q.GetDesc().Type.0;
    let mut frames = FRAMES.lock().unwrap();
    let subs = frames.get_or_insert_with(HashMap::new).entry(frame).or_default();
    for recording in recordings {
        // (one signal a recording that has something to read: they finish in this order)
        let done = if recording.chunks.is_empty() { None } else { signal(queue, q) };
        subs.push(Submitted { queue: queue as usize, queue_type, done, recording });
    }
}

unsafe fn signal(queue: *mut c_void, q: &ID3D12CommandQueue) -> Option<Done> {
    let mut g = FENCES.lock().unwrap();
    let m = g.get_or_insert_with(HashMap::new);
    if !m.contains_key(&(queue as usize)) {
        let mut dev: Option<ID3D12Device> = None;
        let _ = q.GetDevice(&mut dev);
        let fence = dev.and_then(|d| d.CreateFence::<ID3D12Fence>(0, D3D12_FENCE_FLAG_NONE).ok())?;
        m.insert(queue as usize, Done(fence, 0));
    }
    let e = m.get_mut(&(queue as usize))?;
    e.1 += 1;
    q.Signal(&e.0, e.1).ok().map(|()| Done(e.0.clone(), e.1))
}

/// Frame `n` was presented: what was submitted for it is read, logged and written once the
/// GPU has run it.
pub(super) fn presented(n: u64) {
    let Some(s) = settings().filter(|s| n >= s.first) else { return };
    // (also of a frame not wanted any more: lists were submitted for it before the last
    // frame kept was presented)
    let subs = FRAMES.lock().unwrap().as_mut().and_then(|m| m.remove(&n)).unwrap_or_default();
    let ops: usize = subs.iter().map(|s| s.recording.ops.len()).sum();
    // a frame with too few operations goes here: its records are freed, its buffers are the
    // next recordings' (`SPARE`)
    if !s.wants(n, KEPT.load(Ordering::Relaxed)) || !s.keeps(ops as u64) {
        return;
    }
    KEPT.fetch_add(1, Ordering::Relaxed);
    let spawned = std::thread::Builder::new().name("xcl-frame-census".into()).spawn(move || unsafe { finish(n, subs) });
    if let Err(e) = spawned {
        crate::xlog_error!("frame census: frame {n}: no thread to read it back: {e}");
    }
}

unsafe fn finish(frame: u64, mut subs: Vec<Submitted>) {
    let start = Instant::now();
    // (u64::MAX: the device is removed – what the buffers hold is all there will be)
    let pending = |subs: &[Submitted]| subs.iter().filter(|s| s.done.as_ref().is_some_and(|d| d.0.GetCompletedValue() < d.1)).count();
    while pending(&subs) > 0 && start.elapsed() < WAIT {
        std::thread::sleep(Duration::from_millis(5));
    }
    let late = pending(&subs);
    if late > 0 {
        crate::xlog_warn!("frame census: frame {frame}: {late} of {} lists not finished {} s after the present – their ExecuteIndirects are not read back", subs.len(), WAIT.as_secs());
    }
    for s in subs.iter_mut() {
        // (a list not finished: its buffers are not read, and not released – `SPARE`)
        if !s.done.as_ref().is_some_and(|d| d.0.GetCompletedValue() >= d.1) {
            continue;
        }
        let chunks: Vec<Option<Vec<u8>>> = s.recording.chunks.iter().map(|c| c.buf.as_ref().map(|b| b.bytes())).collect();
        for i in s.recording.ops.iter_mut().filter_map(|op| op.indirect.as_mut()) {
            if let Some(bytes) = i.slot.and_then(|slot| chunks.get(slot.chunk)).and_then(|c| c.as_deref()) {
                i.read_back(bytes);
            }
        }
    }
    report(frame, &subs);
}

// ---------------------------------------------------------------------------------------
// the report
// ---------------------------------------------------------------------------------------

/// What a frame's draws did with one target.
#[derive(Clone, Debug, Default, PartialEq)]
struct TargetSum {
    target: Target,
    depth: bool,
    /// draws recorded with it bound
    draws: u32,
    /// … that XCL issued with counts that draw something
    nonzero: u32,
    /// … that are ExecuteIndirects not read back
    unread: u32,
    /// … whose pipeline was a stand-in
    standins: u32,
    /// … that XCL left out
    skipped: u32,
    clears: u32,
    /// the distinct values it was cleared to (the first 4)
    clear_values: Vec<String>,
    /// `nonzero` since its last clear
    after_clear: u32,
}

#[derive(Debug, Default, PartialEq)]
struct Summary {
    ops: u32,
    kinds: Vec<(Kind, u32)>,
    statuses: Vec<(Status, u32)>,
    skipped: Vec<(&'static str, u32)>,
    /// ExecuteIndirects issued, those read back, the commands the GPU ran of them and those
    /// of them with counts that draw or run something
    indirects: u32,
    indirects_read: u32,
    commands: u64,
    commands_nonzero: u64,
    targets: Vec<TargetSum>,
    /// draws with no target bound
    untargeted: u32,
    /// of the draws (None: the pipeline's desc is not known)
    depth_states: Vec<(Option<DepthState>, u32)>,
    viewports: Vec<(Option<[u32; 6]>, u32)>,
    scissors: Vec<(Option<[i32; 4]>, u32)>,
}

fn tally<K: PartialEq>(v: &mut Vec<(K, u32)>, key: K) {
    match v.iter_mut().find(|e| e.0 == key) {
        Some(e) => e.1 += 1,
        None => v.push((key, 1)),
    }
}

fn target_sum(v: &mut Vec<TargetSum>, target: Target, depth: bool) -> &mut TargetSum {
    let at = v.iter().position(|t| t.target.res == target.res && t.depth == depth).unwrap_or_else(|| {
        v.push(TargetSum { target, depth, ..Default::default() });
        v.len() - 1
    });
    &mut v[at]
}

/// The summary of a frame's operations, in the order they were submitted.
fn summarize<'a>(ops: impl Iterator<Item = &'a Op>) -> Summary {
    let mut s = Summary::default();
    for op in ops {
        s.ops += 1;
        tally(&mut s.kinds, op.kind);
        let bound = op.targets.iter().map(|t| (*t, false)).chain(op.depth.iter().map(|d| (d.0, true)));
        if op.kind.clear() {
            for (t, depth) in bound {
                let e = target_sum(&mut s.targets, t, depth);
                e.clears += 1;
                e.after_clear = 0;
                let value = op.clear_text();
                if !e.clear_values.contains(&value) && e.clear_values.len() < 4 {
                    e.clear_values.push(value);
                }
            }
            continue;
        }
        tally(&mut s.statuses, op.status);
        if let Some(why) = op.skipped {
            tally(&mut s.skipped, why);
        }
        if let Some(i) = op.indirect.as_ref().filter(|_| op.skipped.is_none()) {
            s.indirects += 1;
            if i.read {
                s.indirects_read += 1;
                s.commands += i.executed() as u64;
                s.commands_nonzero += i.nonzero() as u64;
            }
        }
        if !op.graphics {
            continue;
        }
        let nonzero = op.nonzero();
        let mut any = false;
        for (t, depth) in bound {
            any = true;
            let e = target_sum(&mut s.targets, t, depth);
            e.draws += 1;
            e.standins += op.status.standin() as u32;
            match (op.skipped, nonzero) {
                (Some(_), _) => e.skipped += 1,
                (None, None) => e.unread += 1,
                (None, Some(true)) => {
                    e.nonzero += 1;
                    e.after_clear += 1;
                }
                (None, Some(false)) => {}
            }
        }
        s.untargeted += !any as u32;
        tally(&mut s.depth_states, op.pipeline.as_ref().and_then(|p| p.state));
        tally(&mut s.viewports, op.viewport.map(|v| v.map(f32::to_bits)));
        tally(&mut s.scissors, op.scissor);
    }
    s
}

fn counted<K>(v: &[(K, u32)], text: impl Fn(&K) -> String) -> String {
    let mut v: Vec<&(K, u32)> = v.iter().collect();
    v.sort_by_key(|e| std::cmp::Reverse(e.1));
    let shown: Vec<String> = v.iter().take(12).map(|e| format!("{} x {}", e.1, text(&e.0))).collect();
    format!("{}{}", shown.join("; "), if v.len() > 12 { format!("; {} more", v.len() - 12) } else { String::new() })
}

/// The summary as log lines (`lists` recordings on `queues` queues).
fn summary_lines(frame: u64, lists: usize, queues: usize, s: &Summary) -> Vec<String> {
    let head = format!("frame census: frame {frame}:");
    let n = |v: u32, what: &str| format!("{v} {what}");
    let kinds: Vec<String> = Kind::ALL.iter().filter_map(|k| s.kinds.iter().find(|e| e.0 == *k).map(|e| n(e.1, k.label()))).collect();
    let statuses: Vec<String> = Status::ALL.iter().filter_map(|k| s.statuses.iter().find(|e| e.0 == *k).map(|e| n(e.1, k.name()))).collect();
    let mut out = vec![format!("{head} {} operations in {lists} lists on {queues} queues: {}", s.ops, kinds.join(", "))];
    out.push(format!(
        "{head} pipelines: {}; left out by XCL: {}",
        if statuses.is_empty() { "none".into() } else { statuses.join(", ") },
        if s.skipped.is_empty() { "none".into() } else { counted(&s.skipped, |w| w.to_string()) }
    ));
    out.push(format!(
        "{head} ExecuteIndirect: {} issued, {} read back – the GPU ran {} commands, {} with non-zero counts",
        s.indirects, s.indirects_read, s.commands, s.commands_nonzero
    ));
    for t in &s.targets {
        out.push(format!(
            "{head} {} target {:#x} (title {:#x}) format {} {}x{}: {} draws, {} with non-zero counts ({} not read back), {} stand-ins, {} left out; {} clears{}; {} non-zero draws after its last clear",
            if t.depth { "depth" } else { "color" },
            t.target.res,
            t.target.addr,
            t.target.format,
            t.target.width,
            t.target.height,
            t.draws,
            t.nonzero,
            t.unread,
            t.standins,
            t.skipped,
            t.clears,
            if t.clear_values.is_empty() { String::new() } else { format!(" ({})", t.clear_values.join("; ")) },
            t.after_clear
        ));
    }
    if s.untargeted > 0 {
        out.push(format!("{head} {} draws with no target bound", s.untargeted));
    }
    out.push(format!("{head} depth states of the draws: {}", counted(&s.depth_states, |d| d.map_or("not known".into(), |d| d.text()))));
    out.push(format!(
        "{head} viewports (x y w h min max): {}",
        counted(&s.viewports, |v| v.map_or("none set".into(), |v| v.map(|b| f32::from_bits(b).to_string()).join(" ")))
    ));
    out.push(format!("{head} scissors (l t r b): {}", counted(&s.scissors, |r| r.map_or("none set".into(), |r| r.map(|v| v.to_string()).join(" ")))));
    out
}

const CSV_HEADER: &str = "frame,submission,queue,queue_type,list,op,kind,outcome,pipeline,status,shaders,count0,count1,count2,nonzero,indirect_arg,indirect_stride,indirect_max,count_buffer,executed,nonzero_commands,commands,targets,depth_target,depth_read_only,vp_x,vp_y,vp_w,vp_h,vp_min,vp_max,sc_l,sc_t,sc_r,sc_b,depth_test,depth_func,depth_write,stencil,cull,clear";

/// One CSV row (no field holds a comma).
fn csv_row(frame: u64, submission: usize, queue: usize, queue_type: i32, list: usize, index: usize, op: &Op) -> String {
    let mut f: Vec<String> = vec![
        frame.to_string(),
        submission.to_string(),
        format!("{queue:#x}"),
        queue_type.to_string(),
        format!("{list:#x}"),
        index.to_string(),
        op.kind.label().into(),
        op.skipped.map_or("issued".into(), |w| format!("left out: {w}")),
        if op.kind.clear() { String::new() } else { format!("{:#x}", op.pso) },
        op.status.name().into(),
        op.pipeline.as_ref().map_or(String::new(), |p| p.shaders()),
    ];
    f.extend(op.counts.map(|c| c.to_string()));
    f.push(if op.kind.clear() { String::new() } else { op.nonzero().map_or("not read", |b| if b { "yes" } else { "no" }).into() });
    match &op.indirect {
        Some(i) => {
            f.push(i.kind().map_or("unknown", |k| k.name()).into());
            f.push(i.signature.map_or(String::new(), |s| s.stride.to_string()));
            f.push(i.max.to_string());
            f.push(match (i.counted, i.count) {
                (false, _) => "none".into(),
                (true, Some(c)) => c.to_string(),
                (true, None) => "not read".into(),
            });
            f.push(if i.read { i.executed().to_string() } else { String::new() });
            f.push(if i.read { i.nonzero().to_string() } else { String::new() });
            f.push(i.commands_text(CSV_COMMANDS));
        }
        None => f.extend(std::iter::repeat_n(String::new(), 7)),
    }
    f.push(op.targets.iter().map(|t| t.text()).collect::<Vec<_>>().join(" "));
    f.push(op.depth.map_or(String::new(), |d| d.0.text()));
    f.push(op.depth.map_or(String::new(), |d| d.1.to_string()));
    match op.viewport {
        Some(v) => f.extend(v.map(|x| x.to_string())),
        None => f.extend(std::iter::repeat_n(String::new(), 6)),
    }
    match op.scissor {
        Some(r) => f.extend(r.map(|x| x.to_string())),
        None => f.extend(std::iter::repeat_n(String::new(), 4)),
    }
    match op.pipeline.as_ref().and_then(|p| p.state) {
        Some(d) => f.extend([on(d.test).to_string(), func_name(d.func), on(d.write).to_string(), on(d.stencil).to_string(), cull_name(d.cull)]),
        None => f.extend(std::iter::repeat_n(String::new(), 5)),
    }
    f.push(if op.kind.clear() { op.clear_text() } else { String::new() });
    f.join(",")
}

fn csv(frame: u64, subs: &[Submitted]) -> String {
    let mut out = String::from(CSV_HEADER);
    out.push('\n');
    for (k, s) in subs.iter().enumerate() {
        for (i, op) in s.recording.ops.iter().enumerate() {
            let _ = writeln!(out, "{}", csv_row(frame, k, s.queue, s.queue_type, s.recording.list, i, op));
        }
    }
    out
}

/// What the GPU ran of an ExecuteIndirect, as a log line's end.
fn indirect_text(i: &Indirect) -> String {
    let what = match i.signature {
        Some(Signature { stride, arg: Some((k, _)) }) => format!("{} commands of {stride} bytes", k.name()),
        _ => "a signature XCL has no draw or dispatch argument of".into(),
    };
    let count = match (i.counted, i.count) {
        (false, _) => "no count buffer".into(),
        (true, Some(c)) => format!("count buffer {c}"),
        (true, None) => "count buffer not read".into(),
    };
    if !i.read {
        return format!("max {}, {what}, {count}: not read back", i.max);
    }
    format!("max {}, {what}, {count}: the GPU ran {}, {} with non-zero counts of the {} read: {}", i.max, i.executed(), i.nonzero(), i.commands.len(), i.commands_text(LOG_COMMANDS))
}

fn report(frame: u64, subs: &[Submitted]) {
    let mut queues: Vec<usize> = subs.iter().map(|s| s.queue).collect();
    queues.sort_unstable();
    queues.dedup();
    let mut logged = 0usize;
    let mut indirects = 0usize;
    for (k, s) in subs.iter().enumerate() {
        for (n, op) in s.recording.ops.iter().enumerate() {
            let Some(i) = op.indirect.as_ref().filter(|_| op.skipped.is_none()) else { continue };
            indirects += 1;
            if logged < LOG_INDIRECTS {
                logged += 1;
                crate::xlog_info!(
                    "frame census: frame {frame}: submission {k} queue {:#x} list {:#x} op {n} ExecuteIndirect, pipeline {:#x} {} ({}): {}",
                    s.queue,
                    s.recording.list,
                    op.pso,
                    op.pipeline.as_ref().map_or(String::new(), |p| p.shaders()),
                    op.status.name(),
                    indirect_text(i)
                );
            }
        }
    }
    if indirects > logged {
        crate::xlog_info!("frame census: frame {frame}: {} more ExecuteIndirects are in the CSV", indirects - logged);
    }
    let summary = summarize(subs.iter().flat_map(|s| s.recording.ops.iter()));
    for line in summary_lines(frame, subs.len(), queues.len(), &summary) {
        crate::xlog_info!("{line}");
    }
    let Some(boot) = crate::state::boot() else { return };
    let path = boot.logs_dir().join(format!("census_{frame}.csv"));
    match std::fs::write(&path, csv(frame, subs)) {
        Ok(()) => crate::xlog_info!("frame census: frame {frame} written to {} ({} rows)", path.display(), summary.ops),
        Err(e) => crate::xlog_error!("frame census: {}: {e}", path.display()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const DRAW: Signature = Signature { stride: 16, arg: Some((ArgKind::Draw, 0)) };

    fn words(w: &[u32]) -> Vec<u8> {
        w.iter().flat_map(|v| v.to_le_bytes()).collect()
    }

    fn target(res: usize) -> Target {
        Target { res, addr: 0x1000 + res, format: 28, width: 1920, height: 1080 }
    }

    fn draw(counts: [u32; 3], rt: usize, depth: usize) -> Op {
        Op { targets: vec![target(rt)], depth: Some((target(depth), false)), ..Op::new(Kind::DrawIndexed, counts) }
    }

    #[test]
    fn the_variables_choose_one_frame_or_three() {
        assert_eq!(schedule(None, None, None), None);
        assert_eq!(schedule(Some("0"), None, None), None);
        assert_eq!(schedule(Some("yes"), None, None), None);
        assert_eq!(schedule(None, None, Some("1000")), None);
        assert_eq!(schedule(Some(" 412 "), None, None), Some(Settings { first: 412, count: 1, min_ops: 0 }));
        assert_eq!(schedule(None, Some("0"), None), Some(Settings { first: 0, count: 3, min_ops: 0 }));
        assert_eq!(schedule(Some("7"), Some("400"), Some("x")), Some(Settings { first: 400, count: 3, min_ops: 0 }));
        assert_eq!(schedule(None, Some("1700"), Some(" 1000 ")), Some(Settings { first: 1700, count: 3, min_ops: 1000 }));
        assert_eq!(schedule(Some("1700"), None, Some("1000")), Some(Settings { first: 1700, count: 1, min_ops: 1000 }));
        let s = Settings { first: 400, count: 3, min_ops: 0 };
        assert_eq!([399, 400, 402, 403].map(|n| s.wants(n, 0)), [false, true, true, false]);
        // operations are kept from the frame before (399 presents: frame 399 is being made)
        assert_eq!([398, 399, 400, 402, 403].map(|n| s.records(n, 0)), [false, true, true, true, false]);
        assert!(Settings { first: 0, count: 1, min_ops: 0 }.records(0, 0));
        // (every frame is reported, whatever it recorded)
        assert!(s.keeps(0));
    }

    #[test]
    fn with_a_minimum_frames_are_recorded_until_three_have_it() {
        let s = Settings { first: 1700, count: 3, min_ops: 1000 };
        assert_eq!([0, 16, 300, 999].map(|ops| s.keeps(ops)), [false; 4]);
        assert!(s.keeps(1000) && s.keeps(14_000));
        // any frame from the first, however late, while fewer than three were kept
        assert_eq!([1699, 1700, 1852, 90_000].map(|n| s.wants(n, 0)), [false, true, true, true]);
        assert_eq!([1698, 1699, 1700, 1852].map(|n| s.records(n, 2)), [false, true, true, true]);
        assert!(!s.wants(1855, 3) && !s.records(1855, 3));
        // a run as the retail title's: loading frames of 16 to 300 operations, then the world
        let mut kept = Vec::new();
        for (frame, ops) in [(1700u64, 16u64), (1701, 300), (1852, 14_000), (1853, 16), (1854, 13_900), (1855, 14_100), (1856, 14_000)] {
            if s.wants(frame, kept.len() as u64) && s.keeps(ops) {
                kept.push(frame);
            }
        }
        assert_eq!(kept, vec![1852, 1854, 1855]);
        // `XCL_FRAME_CENSUS=n`: the first frame from n that has the minimum
        let one = Settings { first: 1700, count: 1, min_ops: 1000 };
        assert!(one.wants(1852, 0) && !one.wants(1853, 1));
    }

    #[test]
    fn a_signature_is_its_first_draw_or_dispatch_argument() {
        assert_eq!(signature(16, &[[0, 0, 0, 0]]), DRAW);
        // a vertex buffer view (16 bytes) and an index buffer view (16) before the indexed draw
        assert_eq!(signature(52, &[[3, 0, 0, 0], [4, 0, 0, 0], [1, 0, 0, 0]]), Signature { stride: 52, arg: Some((ArgKind::DrawIndexed, 32)) });
        // two root constants (8 bytes) before a mesh dispatch
        assert_eq!(signature(20, &[[5, 0, 0, 2], [10, 0, 0, 0]]), Signature { stride: 20, arg: Some((ArgKind::DispatchMesh, 8)) });
        assert_eq!(signature(12, &[[2, 0, 0, 0]]), Signature { stride: 12, arg: Some((ArgKind::Dispatch, 0)) });
        assert_eq!(signature(8, &[[6, 0, 0, 0]]), Signature { stride: 8, arg: None });
    }

    #[test]
    fn arguments_decode_as_draws_indexed_draws_and_dispatches() {
        // D3D12_DRAW_ARGUMENTS: vertices, instances, first vertex, first instance
        let draws = words(&[3, 1, 0, 0, 0, 5, 9, 9, 36, 0, 0, 0]);
        assert_eq!(decode(&draws, DRAW, 3), vec![[3, 1, 0], [0, 5, 0], [36, 0, 0]]);
        assert_eq!(decode(&draws, DRAW, 2).len(), 2);
        // D3D12_DRAW_INDEXED_ARGUMENTS (20 bytes) behind 4 bytes of a root constant, 24 a command
        let indexed = Signature { stride: 24, arg: Some((ArgKind::DrawIndexed, 4)) };
        let bytes = words(&[77, 1200, 2, 0, 0, 0, 78, 0, 0, 6, 7, 8]);
        assert_eq!(decode(&bytes, indexed, 2), vec![[1200, 2, 0], [0, 0, 0]]);
        // D3D12_DISPATCH_ARGUMENTS
        let dispatch = Signature { stride: 12, arg: Some((ArgKind::Dispatch, 0)) };
        assert_eq!(decode(&words(&[8, 8, 1, 0, 4, 4]), dispatch, 2), vec![[8, 8, 1], [0, 4, 4]]);
        // the buffer ends before the count does: what is there
        assert_eq!(decode(&draws[..20], DRAW, 3), vec![[3, 1, 0]]);
        assert_eq!(decode(&draws, Signature { stride: 16, arg: None }, 3), Vec::<[u32; 3]>::new());
        assert!(ArgKind::Draw.nonzero([3, 1, 0]) && !ArgKind::Draw.nonzero([3, 0, 0]) && !ArgKind::DrawIndexed.nonzero([0, 5, 0]));
        assert!(ArgKind::Dispatch.nonzero([8, 8, 1]) && !ArgKind::DispatchMesh.nonzero([8, 0, 1]));
    }

    #[test]
    fn the_count_buffer_limits_what_runs_and_what_is_copied() {
        assert_eq!(executed_commands(None, 64), 64);
        assert_eq!(executed_commands(Some(3), 64), 3);
        assert_eq!(executed_commands(Some(900), 64), 64);
        assert_eq!(copy_bytes(20, 8, 1 << 20), 160);
        // not past the end of the argument buffer, nor of a chunk, nor `MAX_COMMANDS`
        assert_eq!(copy_bytes(20, 8, 50), 50);
        assert_eq!(copy_bytes(20, 8, 0), 0);
        assert_eq!(copy_bytes(20, 100_000, 1 << 30), MAX_COMMANDS * 20);
        assert_eq!(copy_bytes(4096, 100_000, 1 << 30), CHUNK - 4);
    }

    #[test]
    fn an_indirect_is_read_from_its_slot() {
        let mut chunk = vec![0xEEu8; 256];
        chunk[8..12].copy_from_slice(&2u32.to_le_bytes());
        chunk[12..60].copy_from_slice(&words(&[3, 1, 0, 0, 0, 5, 0, 0, 36, 4, 0, 0]));
        let mut i = Indirect { signature: Some(DRAW), max: 3, counted: true, slot: Some(Slot { chunk: 0, count_at: Some(8), args_at: 12, args_bytes: 48 }), ..Default::default() };
        assert_eq!(Op { indirect: Some(i.clone()), ..Op::new(Kind::ExecuteIndirect, [3, 0, 0]) }.nonzero(), None);
        i.read_back(&chunk);
        // the count buffer says 2 of the 3: the third command did not run
        assert_eq!((i.read, i.count, i.executed(), i.nonzero()), (true, Some(2), 2, 1));
        assert_eq!(i.commands, vec![[3, 1, 0], [0, 5, 0]]);
        assert_eq!(i.commands_text(1), "3x1 +1");
        assert_eq!(Op { indirect: Some(i.clone()), ..Op::new(Kind::ExecuteIndirect, [3, 0, 0]) }.nonzero(), Some(true));
        // no count buffer: `max` commands
        let mut all = Indirect { slot: Some(Slot { chunk: 0, count_at: None, args_at: 12, args_bytes: 48 }), counted: false, ..i.clone() };
        all.read_back(&chunk);
        assert_eq!((all.count, all.executed(), all.nonzero()), (None, 3, 2));
        // a count of zero: nothing ran
        chunk[8..12].copy_from_slice(&0u32.to_le_bytes());
        i.read_back(&chunk);
        assert_eq!((i.executed(), i.commands.len()), (0, 0));
        assert_eq!(Op { indirect: Some(i), ..Op::new(Kind::ExecuteIndirect, [3, 0, 0]) }.nonzero(), Some(false));
    }

    #[test]
    fn recordings_are_by_list_and_taken_in_submission_order() {
        let mut b = Book::default();
        b.recording(0xA).ops.push(Op::new(Kind::Draw, [3, 1, 0]));
        b.recording(0xB).ops.push(Op::new(Kind::Dispatch, [1, 1, 1]));
        b.recording(0xA).ops.push(Op::new(Kind::DrawIndexed, [6, 1, 0]));
        b.recording(0xC).ops.push(Op::new(Kind::Draw, [3, 1, 0]));
        // (a list that only had room placed for it recorded nothing)
        b.recording(0xD);
        b.discard(0xC);
        let taken = b.take(&[0xB, 0xD, 0xA, 0xE]);
        assert_eq!(taken.iter().map(|r| (r.list, r.ops.len())).collect::<Vec<_>>(), vec![(0xB, 1), (0xA, 2)]);
        assert_eq!(taken[1].ops[1].kind, Kind::DrawIndexed);
        assert!(b.open.is_empty());
        // the list records anew after its submission
        b.recording(0xA).ops.push(Op::new(Kind::Draw, [3, 1, 0]));
        assert_eq!(b.take(&[0xA])[0].ops.len(), 1);
    }

    #[test]
    fn readback_room_fills_chunks_in_order() {
        let mut r = Recording::default();
        assert_eq!(r.place(0), None);
        assert_eq!(r.place(CHUNK + 1), None);
        assert_eq!(r.place(6), Some((0, 0)));
        // aligned to 4
        assert_eq!(r.place(8), Some((0, 8)));
        assert_eq!(r.place(CHUNK - 8), Some((1, 0)));
        assert_eq!(r.place(4), Some((1, CHUNK - 8)));
        assert_eq!(r.place(4), Some((1, CHUNK - 4)));
        assert_eq!(r.place(4), Some((2, 0)));
        r.chunks.resize_with(MAX_CHUNKS, || Chunk { used: CHUNK, buf: None });
        assert_eq!(r.place(4), None);
    }

    #[test]
    fn a_pipelines_status_is_its_stand_ins() {
        let built = |pso: usize| (pso == 0x22, pso == 0x11 || pso == 0x33);
        assert_eq!(status_of(0x11, None, built), Status::Translated);
        assert_eq!(status_of(0x22, None, built), Status::TwoPass);
        assert_eq!(status_of(0x44, None, built), Status::Pc);
        assert_eq!(status_of(0x50, Some((0, true)), built), Status::Building);
        assert_eq!(status_of(0x50, Some((0, false)), built), Status::StandIn);
        // built in the background: what the real pipeline is
        assert_eq!(status_of(0x50, Some((0x33, false)), built), Status::Translated);
        assert_eq!(status_of(0x50, Some((0x44, false)), built), Status::Pc);
        assert!(Status::Building.standin() && Status::StandIn.standin() && !Status::Pc.standin());
    }

    #[test]
    fn a_stream_gives_its_shaders_and_depth_state() {
        let mut buf = Vec::new();
        let mut sub = |ty: u32, align8: bool, payload: &[u8]| {
            let at = buf.len();
            buf.extend_from_slice(&ty.to_le_bytes());
            buf.resize(at + if align8 { 8 } else { 4 }, 0);
            buf.extend_from_slice(payload);
            buf.resize((buf.len() + 7) & !7, 0);
        };
        let shader = |code: u64, len: u64| [code.to_le_bytes(), len.to_le_bytes()].concat();
        sub(25, true, &shader(0x1000, 64)); // MS
        sub(2, true, &shader(0x2000, 32)); // PS
        // RASTERIZER: fill SOLID, cull NONE
        let mut raster = words(&[3, 1]);
        raster.resize(44, 0);
        sub(10, false, &raster);
        // DEPTH_STENCIL2: enable, write mask ZERO, func GREATER_EQUAL, stencil on
        let mut depth = words(&[1, 0, 7, 1]);
        depth.resize(60, 0);
        sub(26, false, &depth);
        let hash = |code: usize, len: usize| (code != 0).then_some((code + len) as u64);
        let p = stream_pipeline(&buf, hash);
        assert_eq!(p.stages, vec![("MS", 0x1040), ("PS", 0x2020)]);
        assert_eq!(p.state, Some(DepthState { test: true, func: 7, write: false, stencil: true, cull: 1 }));
        assert_eq!(p.shaders(), "MS 0000000000001040 PS 0000000000002020");
        // what a stream leaves out: D3D12's defaults; a CS: no depth state
        let mut buf = Vec::new();
        let mut sub = |ty: u32, payload: &[u8]| {
            buf.extend_from_slice(&ty.to_le_bytes());
            buf.extend_from_slice(&[0; 4]);
            buf.extend_from_slice(payload);
        };
        sub(1, &shader(0x3000, 16));
        let vs = buf.clone();
        assert_eq!(stream_pipeline(&vs, hash).state, Some(DepthState { test: true, func: 2, write: true, stencil: false, cull: 3 }));
        assert_eq!(stream_pipeline(&[6, 0, 0, 0, 0, 0, 0, 0].iter().copied().chain(shader(0x4000, 8)).collect::<Vec<u8>>(), hash), Pipeline { stages: vec![("CS", 0x4008)], state: None });
        // an unknown subobject ends the walk; a truncated stream does not panic
        assert_eq!(stream_pipeline(&[99, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0], hash).stages, vec![]);
        assert_eq!(stream_pipeline(&vs[..12], hash).stages, vec![]);
    }

    #[test]
    fn the_summary_counts_draws_by_target() {
        let less = Arc::new(Pipeline { stages: vec![("VS", 1), ("PS", 2)], state: Some(DepthState { test: true, func: 7, write: true, stencil: false, cull: 3 }) });
        let viewport = Some([0.0, 0.0, 1920.0, 1080.0, 0.0, 1.0]);
        let mut ops = vec![
            Op { clear: [0.0, 0.0, 3.0, 0.0], depth: Some((target(0xD), false)), ..Op::new(Kind::ClearDepth, [0; 3]) },
            Op { pipeline: Some(less.clone()), viewport, scissor: Some([0, 0, 1920, 1080]), ..draw([36, 1, 0], 0xA, 0xD) },
            // an indexed draw of no instances
            Op { pipeline: Some(less.clone()), viewport, ..draw([36, 0, 0], 0xA, 0xD) },
            Op { status: Status::Building, viewport, ..draw([36, 1, 0], 0xA, 0xD) },
            Op { skipped: Some("ABI bind failed"), ..draw([36, 1, 0], 0xB, 0xD) },
            // a compute dispatch has no targets
            Op::new(Kind::Dispatch, [8, 8, 1]),
            Op { clear: [0.0, 0.0, 0.0, 1.0], targets: vec![target(0xA)], ..Op::new(Kind::ClearColor, [0; 3]) },
        ];
        // an ExecuteIndirect not read back, one whose commands are all empty, one that draws
        let indirect = |read: bool, commands: Vec<[u32; 3]>| Op {
            indirect: Some(Indirect { signature: Some(DRAW), max: 4, counted: true, read, count: read.then_some(commands.len() as u32), commands, ..Default::default() }),
            ..Op { depth: None, ..draw([4, 0, 0], 0xA, 0) }
        };
        ops.push(Op { kind: Kind::ExecuteIndirect, ..indirect(false, vec![]) });
        ops.push(Op { kind: Kind::ExecuteIndirect, ..indirect(true, vec![[0, 1, 0], [3, 0, 0]]) });
        ops.push(Op { kind: Kind::ExecuteIndirect, ..indirect(true, vec![[3, 1, 0]]) });
        let s = summarize(ops.iter());
        assert_eq!(s.ops, 10);
        assert_eq!((s.indirects, s.indirects_read, s.commands, s.commands_nonzero), (3, 2, 3, 1));
        assert_eq!(s.skipped, vec![("ABI bind failed", 1)]);
        assert_eq!(s.untargeted, 0);
        let sum = |res: usize, depth: bool| s.targets.iter().find(|t| t.target.res == res && t.depth == depth).unwrap();
        let d = sum(0xD, true);
        assert_eq!((d.draws, d.nonzero, d.unread, d.standins, d.skipped, d.clears, d.after_clear), (4, 2, 0, 1, 1, 1, 2));
        assert_eq!(d.clear_values, vec!["depth 0 stencil 0 flags 3"]);
        let a = sum(0xA, false);
        // 3 draws before its clear (one of no instances), 3 ExecuteIndirects after it
        assert_eq!((a.draws, a.nonzero, a.unread, a.standins, a.skipped, a.clears, a.after_clear), (6, 3, 1, 1, 0, 1, 1));
        assert_eq!(a.clear_values, vec!["0 0 0 1"]);
        let b = sum(0xB, false);
        assert_eq!((b.draws, b.nonzero, b.skipped), (1, 0, 1));
        assert_eq!(s.depth_states.iter().find(|e| e.0.is_some()).map(|e| e.1), Some(2));
        assert_eq!(s.depth_states.iter().find(|e| e.0.is_none()).map(|e| e.1), Some(5));
        assert_eq!(s.viewports.iter().find(|e| e.0.is_some()).map(|e| e.1), Some(3));
        assert_eq!(s.scissors.iter().find(|e| e.0 == Some([0, 0, 1920, 1080])).map(|e| e.1), Some(1));

        let lines = summary_lines(412, 2, 1, &s);
        assert_eq!(lines[0], "frame census: frame 412: 10 operations in 2 lists on 1 queues: 4 DrawIndexedInstanced, 1 Dispatch, 3 ExecuteIndirect, 1 ClearRenderTargetView, 1 ClearDepthStencilView");
        assert_eq!(lines[1], "frame census: frame 412: pipelines: 7 translated, 1 stand-in (building); left out by XCL: 1 x ABI bind failed");
        assert_eq!(lines[2], "frame census: frame 412: ExecuteIndirect: 3 issued, 2 read back – the GPU ran 3 commands, 1 with non-zero counts");
        assert_eq!(
            lines[3],
            "frame census: frame 412: depth target 0xd (title 0x100d) format 28 1920x1080: 4 draws, 2 with non-zero counts (0 not read back), 1 stand-ins, 1 left out; 1 clears (depth 0 stencil 0 flags 3); 2 non-zero draws after its last clear"
        );
        assert!(lines.iter().any(|l| l == "frame census: frame 412: depth states of the draws: 5 x not known; 2 x test on, GREATER_EQUAL, write on, stencil off, cull BACK"), "{lines:#?}");
        assert!(lines.iter().any(|l| l == "frame census: frame 412: viewports (x y w h min max): 4 x none set; 3 x 0 0 1920 1080 0 1"), "{lines:#?}");
    }

    #[test]
    fn the_csv_has_one_row_an_operation_and_as_many_fields_as_its_header() {
        let p = Arc::new(Pipeline { stages: vec![("VS", 0xAB), ("PS", 0xCD)], state: Some(DepthState { test: true, func: 7, write: false, stencil: false, cull: 1 }) });
        let mut i = Indirect { signature: Some(DRAW), max: 3, counted: true, slot: Some(Slot { chunk: 0, count_at: Some(0), args_at: 4, args_bytes: 32 }), ..Default::default() };
        i.read_back(&[words(&[2]), words(&[3, 1, 0, 0, 0, 5, 0, 0])].concat());
        let ops = vec![
            Op { pso: 0x7000, pipeline: Some(p.clone()), viewport: Some([0.0, 0.0, 1920.0, 1080.0, 0.0, 1.0]), scissor: Some([0, 0, 1920, 1080]), ..draw([36, 2, 0], 0xA, 0xD) },
            Op { pso: 0x7000, pipeline: Some(p), indirect: Some(i), ..draw([3, 0, 0], 0xA, 0xD) },
            Op { skipped: Some("pipeline still building"), status: Status::Building, ..Op::new(Kind::Dispatch, [8, 8, 1]) },
            Op { clear: [1.0, 0.0, 1.0, 0.0], depth: Some((target(0xD), false)), ..Op::new(Kind::ClearDepth, [0; 3]) },
        ];
        let subs = vec![Submitted { queue: 0x9000, queue_type: 0, done: None, recording: Recording { list: 0x8000, ops, chunks: Vec::new() } }];
        let text = csv(412, &subs);
        let rows: Vec<&str> = text.lines().collect();
        assert_eq!(rows.len(), 5);
        assert_eq!(rows[0], CSV_HEADER);
        let fields = CSV_HEADER.split(',').count();
        assert!(rows.iter().all(|r| r.split(',').count() == fields), "{text}");
        assert_eq!(
            rows[1],
            "412,0,0x9000,0,0x8000,0,DrawIndexedInstanced,issued,0x7000,translated,VS 00000000000000ab PS 00000000000000cd,36,2,0,yes,,,,,,,,0xa@0x100a:28:1920x1080,0xd@0x100d:28:1920x1080,false,0,0,1920,1080,0,1,0,0,1920,1080,on,GREATER_EQUAL,off,off,NONE,"
        );
        assert!(rows[2].contains(",3,0,0,yes,draw,16,3,2,2,1,3x1 0x5,"), "{}", rows[2]);
        assert!(rows[3].starts_with("412,0,0x9000,0,0x8000,2,Dispatch,left out: pipeline still building,0x0,stand-in (building),,8,8,1,yes,"), "{}", rows[3]);
        assert!(rows[4].ends_with(",depth 1 stencil 0 flags 1"), "{}", rows[4]);
        assert_eq!(indirect_text(subs[0].recording.ops[1].indirect.as_ref().unwrap()), "max 3, draw commands of 16 bytes, count buffer 2: the GPU ran 2, 1 with non-zero counts of the 2 read: 3x1 0x5");
    }
}
