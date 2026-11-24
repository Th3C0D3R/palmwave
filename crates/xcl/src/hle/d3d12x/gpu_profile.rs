//! `XCL_GPU_PROFILE=1`: which GPU operations of the title's command lists take the time.
//!
//! A retail title's world frames kept the GPU busy for seconds to minutes (or for ever) with
//! nothing in the log saying which of their thousands of draws and dispatches it was. With the
//! variable set, every draw, dispatch and ExecuteIndirect recorded for console code (`bind`) –
//! and each pass of a two-pass mesh draw – gets a GPU timestamp before and after it:
//!
//! * a recording (one a PC list, from its first operation to its submission) has a timestamp
//!   query heap and a READBACK buffer of its own; operation `i` uses queries `2 i` (begin) and
//!   `2 i + 1` (end), and each is resolved into the buffer right after it is written – so the
//!   buffer fills while the list runs, and an operation the GPU is still in has its begin
//!   written and its end not (`Progress::Running`);
//! * when the lists go to a queue, the queue signals a fence of the profiler's; a thread reads
//!   the buffers once the fence is there (the queue's `GetTimestampFrequency`) and logs the
//!   slowest operations of each list over the threshold (`XCL_GPU_PROFILE_MS`, 50), and every
//!   10 s the pipelines with the most GPU time so far;
//! * a submission not finished after 10 s is logged with the operations done, running and not
//!   started.
//!
//! At most `OPS` operations a recording are timed; past half of that every 2nd, then every 4th
//! … operation is (`sampled`). Unset, `scope` returns at its first test and nothing else here
//! is called with anything to do.

use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

use windows::core::Interface;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::DXGI_SAMPLE_DESC;

/// timestamp queries of a recording (two an operation)
const QUERIES: u32 = 4096;
/// operations timed in a recording
const OPS: u32 = QUERIES / 2;
/// a submission not finished after this long is logged, and again every `STALL_AGAIN`
const STALL: Duration = Duration::from_secs(10);
const STALL_AGAIN: Duration = Duration::from_secs(60);
/// how often the pipelines with the most GPU time so far are logged
const TOTALS_EVERY: Duration = Duration::from_secs(10);
const LIST_TOP: usize = 10;
const RUN_TOP: usize = 20;
/// recordings alive at once (64 KiB of buffers each): past it lists are not timed
const MAX_RECORDINGS: usize = 512;
const POOL: usize = 64;
const PC_GET_DEVICE: usize = 7;
const PC_GET_TYPE: usize = 8;
const PC_END_QUERY: usize = 53;
const PC_RESOLVE_QUERY_DATA: usize = 54;

pub(super) fn enabled() -> bool {
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| std::env::var("XCL_GPU_PROFILE").is_ok_and(|v| !v.trim().is_empty() && v.trim() != "0"))
}

/// operations shorter than this are not logged list by list (`XCL_GPU_PROFILE_MS`)
fn threshold_ms() -> f64 {
    static T: OnceLock<f64> = OnceLock::new();
    *T.get_or_init(|| threshold_from(std::env::var("XCL_GPU_PROFILE_MS").ok().as_deref()))
}

fn threshold_from(ms: Option<&str>) -> f64 {
    ms.and_then(|v| v.trim().parse::<f64>().ok()).filter(|v| v.is_finite() && *v >= 0.0).unwrap_or(50.0)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(super) enum Kind {
    Draw,
    DrawIndexed,
    Dispatch,
    DispatchMesh,
    ExecuteIndirect,
    /// the passes of a console MS / GS pipeline (`real::TwoPassPso`): the console program as a
    /// compute dispatch, then the mesh pipeline over its records
    MeshCompute,
    MeshDraw,
    MeshIndirectCompute,
    MeshIndirectDraw,
    /// the dispatch that scales the arguments of an indirect mesh pass
    MeshScaleArgs,
}

impl Kind {
    fn label(self) -> &'static str {
        match self {
            Kind::Draw => "DrawInstanced",
            Kind::DrawIndexed => "DrawIndexedInstanced",
            Kind::Dispatch => "Dispatch",
            Kind::DispatchMesh => "DispatchMesh",
            Kind::ExecuteIndirect => "ExecuteIndirect",
            Kind::MeshCompute => "two-pass mesh, compute pass Dispatch",
            Kind::MeshDraw => "two-pass mesh, mesh pass DispatchMesh",
            Kind::MeshIndirectCompute => "two-pass mesh, compute pass ExecuteIndirect",
            Kind::MeshIndirectDraw => "two-pass mesh, mesh pass ExecuteIndirect",
            Kind::MeshScaleArgs => "two-pass mesh, argument scaling Dispatch",
        }
    }

    fn indirect(self) -> bool {
        matches!(self, Kind::ExecuteIndirect | Kind::MeshIndirectCompute | Kind::MeshIndirectDraw)
    }
}

/// What XCL knows of a pipeline it built from console shaders: each stage's name and the hash
/// of its console container (`gpu::shader::fnv64`, as in the other log lines and the shader
/// dumps), and the bytes of DXIL it was translated to.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(super) struct Pipeline {
    pub stages: [(&'static str, u64); 3],
    pub dxil: usize,
}

static PIPELINES: Mutex<Option<HashMap<usize, Pipeline>>> = Mutex::new(None);

/// The pipeline state `pso` was built from these console shaders (`info` is not run when the
/// profiler and frame trace are off).
pub(super) fn note_pipeline(pso: usize, info: impl FnOnce() -> Pipeline) {
    if (enabled() || super::present::trace_frame()) && pso != 0 {
        PIPELINES.lock().unwrap().get_or_insert_with(HashMap::new).insert(pso, info());
    }
}

pub(super) fn pipeline(pso: usize) -> Option<Pipeline> {
    PIPELINES.lock().unwrap().as_ref().and_then(|m| m.get(&pso).copied())
}

/// The pipeline state is gone (another may get its address).
pub(super) fn forget_pipeline(pso: usize) {
    if enabled() || super::present::trace_frame() {
        if let Some(m) = PIPELINES.lock().unwrap().as_mut() {
            m.remove(&pso);
        }
    }
}

/// One timed operation of a recording.
#[derive(Clone, Debug, PartialEq)]
struct Op {
    /// its number among the recording's operations (timed or not)
    seq: u32,
    kind: Kind,
    /// the pipeline state bound (a stand-in resolved), and what it was built from
    pso: usize,
    pipeline: Option<Pipeline>,
    /// vertices or indices and instances; thread groups; an ExecuteIndirect's maximum count
    args: [u32; 3],
    /// ExecuteIndirect: a count buffer is set
    count_buffer: bool,
}

impl Op {
    fn pipeline_text(&self) -> String {
        match &self.pipeline {
            Some(p) => {
                let stages: Vec<String> = p.stages.iter().filter(|s| !s.0.is_empty()).map(|s| format!("{} {:016x}", s.0, s.1)).collect();
                format!("pipeline {:#x} {}, {} B DXIL", self.pso, stages.join(" "), p.dxil)
            }
            None => format!("pipeline {:#x} (no console shaders known)", self.pso),
        }
    }

    fn text(&self) -> String {
        let [a, b, c] = self.args;
        let call = match self.kind {
            Kind::Draw => format!("({a} vertices, {b} instances)"),
            Kind::DrawIndexed => format!("({a} indices, {b} instances)"),
            k if k.indirect() => format!("(max {a}, {})", if self.count_buffer { "count buffer" } else { "no count buffer" }),
            _ => format!("({a}, {b}, {c})"),
        };
        format!("op {} {}{call}, {}", self.seq, self.kind.label(), self.pipeline_text())
    }

    /// what the run's totals are kept by: the kind and the console shaders (the pipeline state
    /// when they are not known)
    fn key(&self) -> (Kind, [u64; 3]) {
        (self.kind, self.pipeline.map_or([self.pso as u64, u64::MAX, u64::MAX], |p| [p.stages[0].1, p.stages[1].1, p.stages[2].1]))
    }
}

/// Whether operation number `seq` of a recording that has timed `used` is timed: all of the
/// first `OPS / 2`, every 2nd of the next `OPS / 2`, every 4th of the next … while queries last.
fn sampled(seq: u32, used: u32) -> bool {
    let stride = 1u32 << (seq / (OPS / 2)).min(31);
    used < OPS && seq.is_multiple_of(stride)
}

/// What the two timestamps of an operation say (0: not written).
#[derive(Clone, Copy, Debug, PartialEq)]
enum Progress {
    NotStarted,
    /// begin written, end not: the GPU is in it
    Running,
    /// ticks from begin to end
    Done(u64),
}

fn progress(begin: u64, end: u64) -> Progress {
    match (begin, end) {
        (0, 0) => Progress::NotStarted,
        (_, 0) => Progress::Running,
        (0, _) => Progress::Done(0),
        (b, e) => Progress::Done(e.saturating_sub(b)),
    }
}

fn ms(ticks: u64, hz: u64) -> f64 {
    ticks as f64 * 1000.0 / hz.max(1) as f64
}

/// A recording's timestamps (`2 * ops` of them) read: which operations are done and how long
/// each took, which the GPU is in, how many it has not reached.
#[derive(Debug, Default, PartialEq)]
struct Summary {
    /// (operation index, milliseconds) of the operations done
    times: Vec<(usize, f64)>,
    running: Vec<usize>,
    not_started: usize,
    /// the first operation not started
    next: Option<usize>,
    /// from the first begin to the last end written
    span_ms: f64,
}

fn summarize(stamps: &[u64], hz: u64) -> Summary {
    let mut s = Summary::default();
    let (mut first, mut last) = (u64::MAX, 0u64);
    for (i, pair) in stamps.chunks_exact(2).enumerate() {
        for &t in pair.iter().filter(|&&t| t != 0) {
            first = first.min(t);
            last = last.max(t);
        }
        match progress(pair[0], pair[1]) {
            Progress::Done(ticks) => s.times.push((i, ms(ticks, hz))),
            Progress::Running => s.running.push(i),
            Progress::NotStarted => {
                s.not_started += 1;
                s.next.get_or_insert(i);
            }
        }
    }
    s.span_ms = if first <= last { ms(last - first, hz) } else { 0.0 };
    s
}

/// The `n` longest of `times` that took more than `over` ms, longest first.
fn slowest(times: &[(usize, f64)], over: f64, n: usize) -> Vec<(usize, f64)> {
    let mut v: Vec<(usize, f64)> = times.iter().copied().filter(|t| t.1 > over).collect();
    v.sort_by(|a, b| b.1.total_cmp(&a.1).then(a.0.cmp(&b.0)));
    v.truncate(n);
    v
}

/// GPU time of the run so far, by kind of operation and pipeline.
#[derive(Default)]
struct Totals {
    by: HashMap<(Kind, [u64; 3]), Total>,
    lists: u64,
    ops: u64,
    /// something was added since the totals were last logged
    changed: bool,
}

struct Total {
    ms: f64,
    count: u64,
    max_ms: f64,
    kind: Kind,
    pipeline: String,
}

impl Totals {
    fn add(&mut self, op: &Op, t: f64) {
        let e = self.by.entry(op.key()).or_insert_with(|| Total { ms: 0.0, count: 0, max_ms: 0.0, kind: op.kind, pipeline: op.pipeline_text() });
        e.ms += t;
        e.count += 1;
        e.max_ms = e.max_ms.max(t);
        self.ops += 1;
        self.changed = true;
    }

    /// the `n` entries with the most time, most first
    fn top(&self, n: usize) -> Vec<&Total> {
        let mut v: Vec<&Total> = self.by.values().collect();
        v.sort_by(|a, b| b.ms.total_cmp(&a.ms).then_with(|| a.pipeline.cmp(&b.pipeline)));
        v.truncate(n);
        v
    }
}

static TOTALS: Mutex<Option<Totals>> = Mutex::new(None);

// ---------------------------------------------------------------------------------------
// recording
// ---------------------------------------------------------------------------------------

/// The query heap and the READBACK buffer of a recording (`QUERIES * 8` bytes, mapped).
struct Buffers {
    device: usize,
    heap: ID3D12QueryHeap,
    readback: ID3D12Resource,
    mapped: usize,
}

// SAFETY: free-threaded COM objects; the mapping lives as long as `readback`
unsafe impl Send for Buffers {}

static SPARE: Mutex<Vec<Buffers>> = Mutex::new(Vec::new());
static ALIVE: AtomicUsize = AtomicUsize::new(0);
/// lists that got no buffers (`MAX_RECORDINGS`, or the device refused)
static NOT_TIMED: AtomicU64 = AtomicU64::new(0);

impl Buffers {
    unsafe fn take(dev: &ID3D12Device) -> Option<Buffers> {
        let key = dev.as_raw() as usize;
        let spare = {
            let mut g = SPARE.lock().unwrap();
            g.iter().position(|b| b.device == key).map(|i| g.swap_remove(i))
        };
        if let Some(b) = spare {
            // (zero: not written – the last recording's timestamps go)
            std::ptr::write_bytes(b.mapped as *mut u8, 0, QUERIES as usize * 8);
            ALIVE.fetch_add(1, Ordering::Relaxed);
            return Some(b);
        }
        if ALIVE.load(Ordering::Relaxed) >= MAX_RECORDINGS {
            return None;
        }
        let mut heap: Option<ID3D12QueryHeap> = None;
        dev.CreateQueryHeap(&D3D12_QUERY_HEAP_DESC { Type: D3D12_QUERY_HEAP_TYPE_TIMESTAMP, Count: QUERIES, NodeMask: 0 }, &mut heap).ok()?;
        let hp = D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_READBACK, ..Default::default() };
        let rd = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
            Width: QUERIES as u64 * 8,
            Height: 1,
            DepthOrArraySize: 1,
            MipLevels: 1,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            ..Default::default()
        };
        let mut rb: Option<ID3D12Resource> = None;
        dev.CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, None, &mut rb).ok()?;
        let (heap, readback) = (heap?, rb?);
        let mut p: *mut c_void = std::ptr::null_mut();
        readback.Map(0, None, Some(&mut p)).ok()?;
        if p.is_null() {
            return None;
        }
        std::ptr::write_bytes(p as *mut u8, 0, QUERIES as usize * 8);
        ALIVE.fetch_add(1, Ordering::Relaxed);
        Some(Buffers { device: key, heap, readback, mapped: p as usize })
    }

    fn give_back(self) {
        ALIVE.fetch_sub(1, Ordering::Relaxed);
        let mut g = SPARE.lock().unwrap();
        if g.len() < POOL {
            g.push(self);
        }
    }

    /// the first `n` timestamps as the GPU has written them so far
    unsafe fn stamps(&self, n: usize) -> Vec<u64> {
        (0..n.min(QUERIES as usize)).map(|i| (self.mapped as *const u64).add(i).read_volatile()).collect()
    }
}

/// What one PC list recorded since it was last submitted or reset.
#[derive(Default)]
struct Recording {
    list: usize,
    /// None: not asked for yet, or this list is not timed (`off`)
    buffers: Option<Buffers>,
    off: bool,
    ops: Vec<Op>,
    /// operations recorded, timed or not
    seen: u32,
}

static RECORDINGS: Mutex<Option<HashMap<usize, Recording>>> = Mutex::new(None);

/// A method of the PC list / queue behind `obj`: an adopted object's original, else its own.
unsafe fn native<F: Copy>(obj: *mut c_void, slot: usize) -> F {
    let f = if super::real::is_adopted(obj) { super::real::orig_fn(obj, slot) } else { *(*(obj as *const *const usize)).add(slot) };
    std::mem::transmute_copy(&f)
}

unsafe fn device_of(list: *mut c_void) -> Option<ID3D12Device> {
    type F = unsafe extern "system" fn(*mut c_void, *const windows::core::GUID, *mut *mut c_void) -> i32;
    let mut raw: *mut c_void = std::ptr::null_mut();
    (native::<F>(list, PC_GET_DEVICE)(list, &ID3D12Device::IID, &mut raw) >= 0 && !raw.is_null()).then(|| ID3D12Device::from_raw(raw))
}

/// Timestamp query `query` of `heap`, written when the GPU gets here and copied to its place
/// in `readback` at once.
unsafe fn stamp(list: *mut c_void, heap: usize, readback: usize, query: u32) {
    type End = unsafe extern "system" fn(*mut c_void, usize, D3D12_QUERY_TYPE, u32);
    type Resolve = unsafe extern "system" fn(*mut c_void, usize, D3D12_QUERY_TYPE, u32, u32, usize, u64);
    native::<End>(list, PC_END_QUERY)(list, heap, D3D12_QUERY_TYPE_TIMESTAMP, query);
    native::<Resolve>(list, PC_RESOLVE_QUERY_DATA)(list, heap, D3D12_QUERY_TYPE_TIMESTAMP, query, 1, readback, query as u64 * 8);
}

/// An operation being recorded: its end timestamp is written when this goes.
pub(super) struct Scope {
    list: *mut c_void,
    heap: usize,
    readback: usize,
    end: u32,
}

impl Drop for Scope {
    fn drop(&mut self) {
        unsafe { stamp(self.list, self.heap, self.readback, self.end) }
    }
}

/// Before an operation is recorded on the PC list `list`: its begin timestamp (None: the
/// profiler is off, or this operation is not timed). `pso`: the pipeline state bound, a
/// stand-in resolved; `args` and `count_buffer` as `Op` has them.
pub(super) unsafe fn scope(list: *mut c_void, kind: Kind, args: [u32; 3], count_buffer: bool, pso: impl FnOnce() -> usize) -> Option<Scope> {
    if !enabled() {
        return None;
    }
    begin(list, kind, args, count_buffer, pso())
}

unsafe fn begin(list: *mut c_void, kind: Kind, args: [u32; 3], count_buffer: bool, pso: usize) -> Option<Scope> {
    let (heap, readback, query) = {
        let mut g = RECORDINGS.lock().unwrap();
        let r = g.get_or_insert_with(HashMap::new).entry(list as usize).or_insert_with(|| Recording { list: list as usize, ..Default::default() });
        let seq = r.seen;
        r.seen += 1;
        if r.off || !sampled(seq, r.ops.len() as u32) {
            return None;
        }
        if r.buffers.is_none() {
            // timestamps are for direct and compute lists (not bundles, not copy lists)
            let ty = native::<unsafe extern "system" fn(*mut c_void) -> i32>(list, PC_GET_TYPE)(list);
            let timed = ty == D3D12_COMMAND_LIST_TYPE_DIRECT.0 || ty == D3D12_COMMAND_LIST_TYPE_COMPUTE.0;
            r.buffers = if timed { device_of(list).and_then(|d| Buffers::take(&d)) } else { None };
            if r.buffers.is_none() {
                r.off = true;
                if timed && NOT_TIMED.fetch_add(1, Ordering::Relaxed) == 0 {
                    crate::xlog_warn!("D3D12.X GPU profile: a list is not timed ({MAX_RECORDINGS} recordings not finished yet, or no query heap)");
                }
                return None;
            }
        }
        let b = r.buffers.as_ref()?;
        let query = 2 * r.ops.len() as u32;
        let pipeline = PIPELINES.lock().unwrap().as_ref().and_then(|m| m.get(&pso).copied());
        r.ops.push(Op { seq, kind, pso, pipeline, args, count_buffer });
        (b.heap.as_raw() as usize, b.readback.as_raw() as usize, query)
    };
    stamp(list, heap, readback, query);
    Some(Scope { list, heap, readback, end: query + 1 })
}

/// The list was reset, or is gone: what it recorded and did not submit is dropped.
pub(super) fn discard(list: *mut c_void) {
    if !enabled() {
        return;
    }
    drop_recording(list);
}

fn drop_recording(list: *mut c_void) {
    let gone = RECORDINGS.lock().unwrap().as_mut().and_then(|m| m.remove(&(list as usize)));
    if let Some(b) = gone.and_then(|r| r.buffers) {
        b.give_back();
    }
}

// ---------------------------------------------------------------------------------------
// submission and reading
// ---------------------------------------------------------------------------------------

/// queue → the profiler's fence there, the last value signalled, the queue's timestamp ticks a second
struct QueueFence {
    fence: ID3D12Fence,
    value: u64,
    hz: u64,
}

unsafe impl Send for QueueFence {}

static FENCES: Mutex<Option<HashMap<usize, QueueFence>>> = Mutex::new(None);

/// Recordings submitted together, done when `fence` reaches `value`.
struct Pending {
    queue: usize,
    fence: ID3D12Fence,
    value: u64,
    hz: u64,
    at: Instant,
    /// when it was last logged as not finished
    stalled: Option<Instant>,
    recordings: Vec<Recording>,
}

unsafe impl Send for Pending {}

static PENDING: Mutex<Vec<Pending>> = Mutex::new(Vec::new());

/// `lists` were just submitted to `queue`: what they recorded is read when the GPU has done it.
pub(super) unsafe fn executed(queue: *mut c_void, lists: &[*mut c_void]) {
    if !enabled() {
        return;
    }
    if submit(queue, lists) {
        monitor();
    }
}

unsafe fn submit(queue: *mut c_void, lists: &[*mut c_void]) -> bool {
    let recordings: Vec<Recording> = {
        let mut g = RECORDINGS.lock().unwrap();
        let Some(m) = g.as_mut() else { return false };
        lists.iter().filter_map(|&l| m.get(&(l as usize)).is_some_and(|r| !r.ops.is_empty()).then(|| m.remove(&(l as usize))).flatten()).collect()
    };
    if recordings.is_empty() {
        return false;
    }
    let give_back = |recordings: Vec<Recording>| recordings.into_iter().filter_map(|r| r.buffers).for_each(Buffers::give_back);
    let Some(q) = ID3D12CommandQueue::from_raw_borrowed(&queue) else {
        give_back(recordings);
        return false;
    };
    let signalled = {
        let mut g = FENCES.lock().unwrap();
        let m = g.get_or_insert_with(HashMap::new);
        if !m.contains_key(&(queue as usize)) {
            let mut dev: Option<ID3D12Device> = None;
            let _ = q.GetDevice(&mut dev);
            if let Some(fence) = dev.and_then(|d| d.CreateFence::<ID3D12Fence>(0, D3D12_FENCE_FLAG_NONE).ok()) {
                m.insert(queue as usize, QueueFence { fence, value: 0, hz: q.GetTimestampFrequency().unwrap_or(0) });
            }
        }
        m.get_mut(&(queue as usize)).and_then(|e| {
            e.value += 1;
            q.Signal(&e.fence, e.value).ok().map(|()| (e.fence.clone(), e.value, e.hz))
        })
    };
    let Some((fence, value, hz)) = signalled else {
        give_back(recordings);
        return false;
    };
    PENDING.lock().unwrap().push(Pending { queue: queue as usize, fence, value, hz, at: Instant::now(), stalled: None, recordings });
    true
}

/// What the monitor found: a recording the GPU has finished, or one of a submission not
/// finished in time.
struct Report {
    queue: usize,
    list: usize,
    hz: u64,
    /// since the submission
    waited: Duration,
    finished: bool,
    /// not finished, and the first such recording on its queue: the one the GPU is in
    head: bool,
    ops: Vec<Op>,
    seen: u32,
    summary: Summary,
}

/// The submissions the GPU has finished (taken out, their buffers free again) and those not
/// finished `STALL` after they were submitted, as reports.
unsafe fn collect(now: Instant) -> Vec<Report> {
    let mut out = Vec::new();
    let mut done = Vec::new();
    // queues with a submission not finished before the one looked at
    let mut busy: Vec<usize> = Vec::new();
    {
        let mut g = PENDING.lock().unwrap();
        let mut i = 0;
        while i < g.len() {
            // (u64::MAX: the device is removed – what the buffers hold is all there will be)
            let completed = g[i].fence.GetCompletedValue();
            if completed >= g[i].value {
                done.push((g.remove(i), completed != u64::MAX));
                continue;
            }
            let p = &mut g[i];
            let mut head = !busy.contains(&p.queue);
            busy.push(p.queue);
            let waited = now.saturating_duration_since(p.at);
            if waited >= STALL && p.stalled.is_none_or(|t| now.saturating_duration_since(t) >= STALL_AGAIN) {
                p.stalled = Some(now);
                for r in &p.recordings {
                    let Some(b) = &r.buffers else { continue };
                    let summary = summarize(&b.stamps(2 * r.ops.len()), p.hz);
                    let unfinished = !summary.running.is_empty() || summary.not_started > 0;
                    out.push(Report { queue: p.queue, list: r.list, hz: p.hz, waited, finished: false, head: head && unfinished, ops: r.ops.clone(), seen: r.seen, summary });
                    head &= !unfinished;
                }
            }
            i += 1;
        }
    }
    for (p, finished) in done {
        let waited = now.saturating_duration_since(p.at);
        for r in p.recordings {
            let Some(b) = r.buffers else { continue };
            let summary = summarize(&b.stamps(2 * r.ops.len()), p.hz);
            b.give_back();
            out.push(Report { queue: p.queue, list: r.list, hz: p.hz, waited, finished, head: false, ops: r.ops, seen: r.seen, summary });
        }
    }
    out
}

/// A finished recording goes into the run's totals; its slowest operations and a recording not
/// finished are logged.
fn log(r: &Report, over: f64) {
    let s = &r.summary;
    let (timed, seen) = (r.ops.len(), r.seen);
    if !r.finished {
        // a list the GPU has not reached says nothing (the one before it on the queue does)
        if s.times.is_empty() && s.running.is_empty() && !r.head {
            return;
        }
        crate::xlog_warn!(
            "D3D12.X GPU profile: list {:#x} on queue {:#x} not finished {:.0} s after it was submitted: of {timed} operations timed ({seen} recorded) {} done in {:.1} ms, {} running, {} not started",
            r.list,
            r.queue,
            r.waited.as_secs_f64(),
            s.times.len(),
            s.times.iter().map(|t| t.1).sum::<f64>(),
            s.running.len(),
            s.not_started
        );
        for &i in &s.running {
            crate::xlog_warn!("D3D12.X GPU profile:   running: {}", r.ops[i].text());
        }
        if let (true, Some(i)) = (s.running.is_empty(), s.next) {
            crate::xlog_warn!("D3D12.X GPU profile:   next (the GPU is in what was recorded before it): {}", r.ops[i].text());
        }
        for (i, t) in slowest(&s.times, over, LIST_TOP) {
            crate::xlog_warn!("D3D12.X GPU profile:   done, {t:.1} ms: {}", r.ops[i].text());
        }
        return;
    }
    {
        let mut g = TOTALS.lock().unwrap();
        let totals = g.get_or_insert_with(Totals::default);
        totals.lists += 1;
        for &(i, t) in &s.times {
            totals.add(&r.ops[i], t);
        }
    }
    let slow = slowest(&s.times, over, LIST_TOP);
    if slow.is_empty() {
        return;
    }
    crate::xlog_info!(
        "D3D12.X GPU profile: list {:#x} on queue {:#x}: {timed} operations timed ({seen} recorded, {} without timestamps), {:.1} ms in them, {:.1} ms from its first to its last, done {:.0} ms after it was submitted ({} Hz); over {over} ms:",
        r.list,
        r.queue,
        s.running.len() + s.not_started,
        s.times.iter().map(|t| t.1).sum::<f64>(),
        s.span_ms,
        r.waited.as_secs_f64() * 1000.0,
        r.hz
    );
    for (k, (i, t)) in slow.iter().enumerate() {
        crate::xlog_info!("D3D12.X GPU profile:   #{} {t:.1} ms: {}", k + 1, r.ops[*i].text());
    }
}

/// The pipelines with the most GPU time so far, when anything was added since the last time.
fn log_totals() {
    let mut g = TOTALS.lock().unwrap();
    let Some(t) = g.as_mut().filter(|t| t.changed) else { return };
    t.changed = false;
    crate::xlog_info!(
        "D3D12.X GPU profile: most GPU time so far ({} lists, {} operations timed, {} lists not timed, {} submissions not finished):",
        t.lists,
        t.ops,
        NOT_TIMED.load(Ordering::Relaxed),
        PENDING.lock().unwrap().len()
    );
    for (k, e) in t.top(RUN_TOP).iter().enumerate() {
        crate::xlog_info!("D3D12.X GPU profile:   #{} {:.1} ms in {} operations (longest {:.1} ms): {}, {}", k + 1, e.ms, e.count, e.max_ms, e.kind.label(), e.pipeline);
    }
}

/// The thread that reads finished recordings (started with the first submission).
fn monitor() {
    static STARTED: std::sync::Once = std::sync::Once::new();
    STARTED.call_once(|| {
        crate::xlog_info!(
            "D3D12.X GPU profile: on (XCL_GPU_PROFILE) – up to {OPS} operations a list timed, a list's operations over {} ms (XCL_GPU_PROFILE_MS) and the run's totals every {} s logged",
            threshold_ms(),
            TOTALS_EVERY.as_secs()
        );
        let spawned = std::thread::Builder::new().name("xcl-gpu-profile".into()).spawn(|| {
            let mut totals_at = Instant::now();
            loop {
                std::thread::sleep(Duration::from_millis(250));
                let now = Instant::now();
                for r in unsafe { collect(now) } {
                    log(&r, threshold_ms());
                }
                if now.duration_since(totals_at) >= TOTALS_EVERY {
                    totals_at = now;
                    log_totals();
                }
            }
        });
        if spawned.is_err() {
            crate::xlog_warn!("D3D12.X GPU profile: no thread to read the timestamps – nothing will be logged");
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn op(seq: u32, kind: Kind, pso: usize, pipeline: Option<Pipeline>) -> Op {
        Op { seq, kind, pso, pipeline, args: [3, 2, 1], count_buffer: false }
    }

    #[test]
    fn the_threshold_is_50_ms_unless_a_number_is_given() {
        assert_eq!(threshold_from(None), 50.0);
        assert_eq!(threshold_from(Some(" 5 ")), 5.0);
        assert_eq!(threshold_from(Some("0")), 0.0);
        assert_eq!(threshold_from(Some("0.5")), 0.5);
        assert_eq!(threshold_from(Some("-1")), 50.0);
        assert_eq!(threshold_from(Some("fast")), 50.0);
    }

    /// Every operation of the first half of the queries, then every 2nd, 4th … – and never more
    /// than `OPS` in a recording.
    #[test]
    fn a_recording_times_all_its_first_operations_and_samples_the_rest() {
        let half = OPS / 2;
        assert!((0..half).all(|seq| sampled(seq, seq)));
        assert!(sampled(half, half) && !sampled(half + 1, half + 1) && sampled(half + 2, half + 1));
        assert!(sampled(2 * half, 0) && !sampled(2 * half + 2, 0) && sampled(2 * half + 4, 0));
        assert!(!sampled(0, OPS), "no query left");
        // a long list: the bookkeeping `begin` does
        let mut used = 0u32;
        let mut last = 0;
        for seq in 0..200_000u32 {
            if sampled(seq, used) {
                used += 1;
                last = seq;
            }
        }
        assert!(used <= OPS && 2 * used <= QUERIES);
        assert!(used > OPS - 8, "the queries are used: {used}");
        assert!(last > 10_000, "the timed operations reach far into a long list: {last}");
    }

    #[test]
    fn two_timestamps_say_whether_an_operation_is_done_running_or_not_started() {
        assert_eq!(progress(0, 0), Progress::NotStarted);
        assert_eq!(progress(1000, 0), Progress::Running);
        assert_eq!(progress(1000, 1500), Progress::Done(500));
        // an end before its begin (never seen; not a negative time)
        assert_eq!(progress(1500, 1000), Progress::Done(0));
        assert_eq!(progress(0, 1000), Progress::Done(0));
    }

    /// The timestamp array of a list the GPU is in the middle of: two operations done, the
    /// third begun and not ended, two not reached.
    #[test]
    fn an_unfinished_operation_is_the_one_with_a_begin_and_no_end() {
        let hz = 10_000_000; // 100 ns ticks
        let stamps = [1_000_000, 1_500_000, 1_500_100, 31_500_100, 31_500_200, 0, 0, 0, 0, 0];
        let s = summarize(&stamps, hz);
        assert_eq!(s.times, vec![(0, 50.0), (1, 3000.0)]);
        assert_eq!(s.running, vec![2]);
        assert_eq!((s.not_started, s.next), (2, Some(3)));
        assert!((s.span_ms - 3050.02).abs() < 1e-6, "{}", s.span_ms);
        // finished: nothing running, nothing left
        let done = summarize(&[10, 20, 20, 50], 1000);
        assert_eq!((done.times, done.running, done.not_started, done.next, done.span_ms), (vec![(0, 10.0), (1, 30.0)], vec![], 0, None, 40.0));
        // not reached at all, and nothing recorded
        let none = summarize(&[0, 0, 0, 0], hz);
        assert_eq!((none.times.len(), none.running.len(), none.not_started, none.next, none.span_ms), (0, 0, 2, Some(0), 0.0));
        assert_eq!(summarize(&[], hz), Summary::default());
        // a queue that reports no frequency does not divide by zero
        assert!(summarize(&[1, 2], 0).times[0].1.is_finite());
    }

    #[test]
    fn the_slowest_operations_are_those_over_the_threshold_longest_first() {
        let times = [(0, 10.0), (1, 75.0), (2, 50.0), (3, 900.0), (4, 75.0), (5, 51.0)];
        assert_eq!(slowest(&times, 50.0, 10), vec![(3, 900.0), (1, 75.0), (4, 75.0), (5, 51.0)]);
        assert_eq!(slowest(&times, 50.0, 2), vec![(3, 900.0), (1, 75.0)]);
        assert_eq!(slowest(&times, 1000.0, 10), vec![]);
        assert_eq!(slowest(&times, 0.0, 10).len(), 6);
    }

    #[test]
    fn the_runs_totals_are_kept_by_kind_and_console_shaders() {
        let cs = Pipeline { stages: [("CS", 0xAA), ("", 0), ("", 0)], dxil: 1000 };
        let ms = Pipeline { stages: [("MS", 0xBB), ("PS", 0xCC), ("", 0)], dxil: 2000 };
        let mut t = Totals::default();
        // the same console shader in two pipeline states is one entry
        t.add(&op(0, Kind::Dispatch, 0x1000, Some(cs)), 10.0);
        t.add(&op(1, Kind::Dispatch, 0x2000, Some(cs)), 30.0);
        // the two passes of a mesh pipeline are two
        t.add(&op(2, Kind::MeshCompute, 0x3000, Some(ms)), 500.0);
        t.add(&op(3, Kind::MeshDraw, 0x3000, Some(ms)), 5.0);
        // pipelines without console shaders are kept by their address
        t.add(&op(4, Kind::Draw, 0x4000, None), 1.0);
        t.add(&op(5, Kind::Draw, 0x5000, None), 2.0);
        assert_eq!((t.by.len(), t.ops, t.changed), (5, 6, true));
        let top = t.top(3);
        assert_eq!(top.iter().map(|e| (e.kind, e.ms, e.count, e.max_ms)).collect::<Vec<_>>(), vec![(Kind::MeshCompute, 500.0, 1, 500.0), (Kind::Dispatch, 40.0, 2, 30.0), (Kind::MeshDraw, 5.0, 1, 5.0)]);
        assert!(top[0].pipeline.contains("MS 00000000000000bb PS 00000000000000cc, 2000 B DXIL"), "{}", top[0].pipeline);
        assert_eq!(t.top(20).len(), 5);
    }

    #[test]
    fn an_operation_is_logged_with_its_counts_and_its_shaders() {
        let ms = Pipeline { stages: [("MS", 0x1234), ("PS", 0x5678), ("", 0)], dxil: 4096 };
        let mut o = op(7, Kind::MeshIndirectCompute, 0xABC0, Some(ms));
        o.args = [1024, 0, 0];
        o.count_buffer = true;
        assert_eq!(o.text(), "op 7 two-pass mesh, compute pass ExecuteIndirect(max 1024, count buffer), pipeline 0xabc0 MS 0000000000001234 PS 0000000000005678, 4096 B DXIL");
        o.count_buffer = false;
        assert!(o.text().contains("(max 1024, no count buffer)"));
        assert_eq!(op(0, Kind::Dispatch, 0x10, None).text(), "op 0 Dispatch(3, 2, 1), pipeline 0x10 (no console shaders known)");
        assert!(op(0, Kind::DrawIndexed, 0x10, None).text().starts_with("op 0 DrawIndexedInstanced(3 indices, 2 instances)"));
        assert!(op(0, Kind::Draw, 0x10, None).text().starts_with("op 0 DrawInstanced(3 vertices, 2 instances)"));
    }

    /// On a device: operations recorded on a list get their timestamps as the GPU runs them,
    /// the report comes when the queue has passed the submission, a list reset without being
    /// submitted leaves nothing, and buffers used again start unwritten.
    #[test]
    fn a_submitted_recording_is_reported_with_its_timestamps() {
        use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
        unsafe {
            let mut dev: Option<ID3D12Device> = None;
            if D3D12CreateDevice(None, D3D_FEATURE_LEVEL_11_0, &mut dev).is_err() {
                crate::gpu::recomp::runner::skip_test("no GPU");
                return;
            }
            let dev = dev.unwrap();
            let q: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC { Type: D3D12_COMMAND_LIST_TYPE_DIRECT, ..Default::default() }).unwrap();
            let alloc: ID3D12CommandAllocator = dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT).unwrap();
            let cl: ID3D12GraphicsCommandList = dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &alloc, None).unwrap();
            let list = cl.as_raw();
            PIPELINES.lock().unwrap().get_or_insert_with(HashMap::new).insert(0x7700_0010, Pipeline { stages: [("CS", 0xC5), ("", 0), ("", 0)], dxil: 64 });
            // (nothing between the timestamps: the list needs no pipeline)
            drop(begin(list, Kind::Dispatch, [8, 8, 1], false, 0x7700_0010).expect("a direct list is timed"));
            drop(begin(list, Kind::ExecuteIndirect, [16, 0, 0], true, 0).expect("timed"));
            // before the GPU ran it: nothing written
            {
                let g = RECORDINGS.lock().unwrap();
                let r = &g.as_ref().unwrap()[&(list as usize)];
                assert_eq!((r.ops.len(), r.seen), (2, 2));
                assert_eq!(summarize(&r.buffers.as_ref().unwrap().stamps(4), 1).not_started, 2);
            }
            cl.Close().unwrap();
            q.ExecuteCommandLists(&[Some(cl.cast().unwrap())]);
            assert!(submit(q.as_raw(), &[list]));
            assert!(!submit(q.as_raw(), &[list]), "a recording is submitted once");
            let started = Instant::now();
            let report = loop {
                if let Some(r) = collect(Instant::now()).into_iter().find(|r| r.list == list as usize) {
                    break r;
                }
                assert!(started.elapsed().as_secs() < 20, "the queue never passed the profiler's fence");
                std::thread::sleep(Duration::from_millis(5));
            };
            assert!(report.finished && report.hz > 0 && report.queue == q.as_raw() as usize);
            assert_eq!((report.ops.len(), report.seen), (2, 2));
            assert_eq!(report.ops[0].pipeline.map(|p| p.stages[0]), Some(("CS", 0xC5)));
            assert!(report.ops[1].count_buffer && report.ops[1].pipeline.is_none());
            let s = &report.summary;
            assert_eq!((s.times.len(), s.running.len(), s.not_started), (2, 0, 0), "both operations have both timestamps: {s:?}");
            assert!(s.times.iter().all(|t| t.1 >= 0.0 && t.1 < 10_000.0), "{s:?}");
            log(&report, 0.0);

            // recorded again and reset without a submission: dropped; the buffers come back unwritten
            alloc.Reset().unwrap();
            cl.Reset(&alloc, None).unwrap();
            drop(begin(list, Kind::Draw, [3, 1, 0], false, 0).expect("timed"));
            {
                let g = RECORDINGS.lock().unwrap();
                let r = &g.as_ref().unwrap()[&(list as usize)];
                assert_eq!(r.buffers.as_ref().unwrap().stamps(QUERIES as usize).iter().filter(|&&t| t != 0).count(), 0, "buffers used again are zero");
            }
            drop_recording(list);
            assert!(!RECORDINGS.lock().unwrap().as_ref().unwrap().contains_key(&(list as usize)));
            cl.Close().unwrap();
            assert!(!submit(q.as_raw(), &[list]), "nothing recorded, nothing to wait for");

            // a bundle is not timed (no timestamp queries there)
            let balloc: ID3D12CommandAllocator = dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE).unwrap();
            let bundle: ID3D12GraphicsCommandList = dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, &balloc, None).unwrap();
            assert!(begin(bundle.as_raw(), Kind::Draw, [3, 1, 0], false, 0).is_none());
            assert!(begin(bundle.as_raw(), Kind::Draw, [3, 1, 0], false, 0).is_none());
            drop_recording(bundle.as_raw());
            PIPELINES.lock().unwrap().as_mut().unwrap().remove(&0x7700_0010);
        }
    }
}
