//! `XCL_MESH_RECORD_DUMP=n`: what the first pass of the two-pass mesh draws of shown frame `n`
//! wrote – the records the pass-through mesh shader rasterises (`gpu::recomp::MsInterface`:
//! vertex count, primitive count, a vertex per thread of the group, a packed triangle per
//! thread). The first records of the frame's first draws are copied out of the record buffer
//! right after the compute pass and written to the logs folder two presents later:
//! `mesh_records_<n>.txt` (each draw's SGPRs and a reading of its records: counts, null and
//! out-of-range triangles, vertices that are zero or not finite by quarter of the group, the
//! NDC box, edge lengths) and `mesh_records_<n>_<k>.bin` (draw k's copied dwords).
//!
//! `XCL_MESH_RECORD_DUMP_DRAWS` (24) and `XCL_MESH_RECORD_DUMP_RECORDS` (4) bound the copy.
//! `XCL_MESH_RECORD_DUMP_ARGS=1` also copies an indirect draw's (x, y, z): the argument
//! buffer is taken to be in INDIRECT_ARGUMENT, as the group scaler takes it.

use std::ffi::c_void;
use std::fmt::Write as _;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Mutex, OnceLock};

use windows::core::Interface;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT_UNKNOWN, DXGI_SAMPLE_DESC};

use crate::gpu::recomp::{MsInterface, MESH_HEADER};

/// bytes before the records in a capture: the indirect draw's (x, y, z), or zeros
const ARGS_BYTES: u64 = 16;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Settings {
    frame: u64,
    draws: usize,
    records: usize,
    args: bool,
}

/// None: the dump is off.
fn schedule(frame: Option<&str>, draws: Option<&str>, records: Option<&str>, args: Option<&str>) -> Option<Settings> {
    let count = |v: Option<&str>, default: usize| v.and_then(|v| v.trim().parse().ok()).filter(|&n| n > 0).unwrap_or(default);
    Some(Settings {
        frame: frame?.trim().parse().ok()?,
        draws: count(draws, 24),
        records: count(records, 4),
        args: args.is_some_and(|v| v.trim() == "1"),
    })
}

fn settings() -> Option<Settings> {
    static S: OnceLock<Option<Settings>> = OnceLock::new();
    *S.get_or_init(|| {
        let var = |name: &str| std::env::var(name).ok();
        schedule(var("XCL_MESH_RECORD_DUMP").as_deref(), var("XCL_MESH_RECORD_DUMP_DRAWS").as_deref(), var("XCL_MESH_RECORD_DUMP_RECORDS").as_deref(), var("XCL_MESH_RECORD_DUMP_ARGS").as_deref())
    })
}

struct Capture {
    frame: u64,
    buffer: ID3D12Resource,
    record: MsInterface,
    records: usize,
    what: String,
    io: Vec<u32>,
    args: bool,
}

unsafe impl Send for Capture {}

static PENDING: Mutex<Vec<Capture>> = Mutex::new(Vec::new());
static TAKEN: AtomicUsize = AtomicUsize::new(0);

/// A two-pass draw recorded now is one the dump takes.
pub(super) fn due() -> bool {
    settings().is_some_and(|s| super::present::presents() == s.frame && TAKEN.load(Ordering::Relaxed) < s.draws)
}

/// After a draw's compute pass (and its UAV barrier), in the title's list: copy the first
/// records of `res` – and, asked for, the indirect arguments at `args` – to a readback buffer.
/// `what` names the draw, `io` holds its SGPRs.
pub(super) unsafe fn capture(this: *mut c_void, list: &ID3D12GraphicsCommandList, res: &ID3D12Resource, record: &MsInterface, what: String, io: &[u32], args: Option<(usize, u64)>) {
    let Some(s) = settings() else { return };
    let Some(dev) = super::real::list_device(this) else { return };
    let record_bytes = record.record_dwords() as u64 * 4;
    let records = (s.records as u64).min(res.GetDesc().Width / record_bytes.max(1));
    if records == 0 || TAKEN.fetch_add(1, Ordering::Relaxed) >= s.draws {
        return;
    }
    let desc = D3D12_RESOURCE_DESC {
        Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
        Alignment: 0,
        Width: ARGS_BYTES + records * record_bytes,
        Height: 1,
        DepthOrArraySize: 1,
        MipLevels: 1,
        Format: DXGI_FORMAT_UNKNOWN,
        SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
        Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        Flags: D3D12_RESOURCE_FLAG_NONE,
    };
    let mut buffer: Option<ID3D12Resource> = None;
    let made = dev.CreateCommittedResource(&D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_READBACK, ..Default::default() }, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, None, &mut buffer);
    let (Ok(()), Some(buffer)) = (made, buffer) else { return };
    let args_raw = args.filter(|_| s.args).map(|(a, off)| (a as *mut c_void, off));
    let args_res = args_raw.as_ref().and_then(|(a, off)| Some((ID3D12Resource::from_raw_borrowed(a)?, *off)));
    if let Some((a, off)) = args_res {
        super::bind::emit_transition(list, a, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list.CopyBufferRegion(&buffer, 0, a, off, 12);
        super::bind::emit_transition(list, a, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }
    // the compute pass left the record buffer in UNORDERED_ACCESS (promoted from COMMON, or
    // put back there by the capture before this one)
    super::bind::emit_transition(list, res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list.CopyBufferRegion(&buffer, ARGS_BYTES, res, 0, records * record_bytes);
    super::bind::emit_transition(list, res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    PENDING.lock().unwrap().push(Capture { frame: s.frame, buffer, record: record.clone(), records: records as usize, what, io: io.to_vec(), args: args_res.is_some() });
}

/// At a present, after the GPU finished the frames before the last one: the captures of a
/// frame two presents back are complete on the GPU – read and written.
pub(super) unsafe fn flush() {
    if settings().is_none() {
        return;
    }
    let now = super::present::presents();
    let ready: Vec<Capture> = {
        let mut p = PENDING.lock().unwrap();
        if p.is_empty() || p.iter().any(|c| c.frame + 2 > now) {
            return;
        }
        std::mem::take(&mut *p)
    };
    let Some(boot) = crate::state::boot() else { return };
    let frame = ready[0].frame;
    let mut text = format!("mesh records of shown frame {frame}: {} two-pass draw(s)\n", ready.len());
    for (k, c) in ready.iter().enumerate() {
        let dwords = (ARGS_BYTES as usize + c.records * c.record.record_dwords() * 4) / 4;
        let mut p: *mut c_void = std::ptr::null_mut();
        if c.buffer.Map(0, None, Some(&mut p)).is_err() || p.is_null() {
            let _ = writeln!(text, "\ndraw {k}: {} – the readback could not be mapped", c.what);
            continue;
        }
        let data = std::slice::from_raw_parts(p as *const u32, dwords).to_vec();
        c.buffer.Unmap(0, None);
        let _ = writeln!(text, "\ndraw {k}: {}", c.what);
        if c.args {
            let _ = writeln!(text, "  indirect arguments: ({}, {}, {})", data[0], data[1], data[2]);
        }
        let _ = writeln!(text, "  s0..s31: {}", c.io.iter().take(32).map(|w| format!("{w:08x}")).collect::<Vec<_>>().join(" "));
        text.push_str(&report(&c.record, &data[ARGS_BYTES as usize / 4..], c.records));
        let bytes: Vec<u8> = data.iter().flat_map(|w| w.to_le_bytes()).collect();
        let _ = std::fs::write(boot.logs_dir().join(format!("mesh_records_{frame}_{k}.bin")), bytes);
    }
    let path = boot.logs_dir().join(format!("mesh_records_{frame}.txt"));
    match std::fs::write(&path, text) {
        Ok(()) => crate::xlog_info!("mesh record dump: frame {frame}, {} draw(s) written to {}", ready.len(), path.display()),
        Err(e) => crate::xlog_error!("mesh record dump: {}: {e}", path.display()),
    }
}

/// A reading of the first `records` records in `data` (see the module docs).
fn report(ms: &MsInterface, data: &[u32], records: usize) -> String {
    let (rd, vd, mv, mp) = (ms.record_dwords(), ms.vert_dwords(), ms.max_vertices as usize, ms.max_primitives as usize);
    let mut out = format!("  record: {mv} vertices and {mp} triangles at most, {vd} dwords a vertex, {rd} dwords\n");
    for g in 0..records {
        let Some(r) = data.get(g * rd..(g + 1) * rd) else { break };
        let (nv, np) = ((r[0] as usize).min(mv), (r[1] as usize).min(mp));
        let _ = writeln!(out, "  record {g}: {} vertices, {} triangles (header {:#x} {:#x} {:#x} {:#x})", r[0], r[1], r[0], r[1], r[2], r[3]);
        let position = |i: usize| -> [f32; 4] { std::array::from_fn(|c| f32::from_bits(r[MESH_HEADER + i * vd + c])) };
        // vertices by quarter of the group: a quarter is a 32-lane half of a console wave
        let quarters = mv.div_ceil(4).max(1);
        let (mut zero, mut bad) = ([0usize; 4], [0usize; 4]);
        let (mut lo, mut hi) = ([f32::INFINITY; 2], [f32::NEG_INFINITY; 2]);
        let ndc = |i: usize| -> Option<[f32; 2]> {
            let p = position(i);
            (p.iter().all(|v| v.is_finite()) && p[3] != 0.0).then(|| [p[0] / p[3], p[1] / p[3]])
        };
        for i in 0..nv {
            let q = (i / quarters).min(3);
            if r[MESH_HEADER + i * vd..MESH_HEADER + i * vd + 4].iter().all(|&w| w == 0) {
                zero[q] += 1;
            } else if !position(i).iter().all(|v| v.is_finite()) {
                bad[q] += 1;
            }
            if let Some(p) = ndc(i) {
                for c in 0..2 {
                    lo[c] = lo[c].min(p[c]);
                    hi[c] = hi[c].max(p[c]);
                }
            }
        }
        let _ = writeln!(out, "    vertices all zero, by quarter of the group: {zero:?}; not finite: {bad:?}");
        let _ = writeln!(out, "    NDC box: x {} .. {}, y {} .. {}", lo[0], hi[0], lo[1], hi[1]);
        let tri_base = MESH_HEADER + mv * vd;
        let (mut null, mut out_of_range) = (0usize, 0usize);
        let mut edges: Vec<f32> = Vec::new();
        let mut shown = String::new();
        for j in 0..np {
            let p = r[tri_base + j];
            let x = [(p & 0x3FF) as usize, ((p >> 10) & 0x3FF) as usize, ((p >> 20) & 0x3FF) as usize];
            if p >> 31 != 0 {
                null += 1;
                continue;
            }
            if x.iter().any(|&i| i >= nv) {
                out_of_range += 1;
                continue;
            }
            if let (Some(a), Some(b), Some(c)) = (ndc(x[0]), ndc(x[1]), ndc(x[2])) {
                let len = |u: [f32; 2], v: [f32; 2]| ((u[0] - v[0]).powi(2) + (u[1] - v[1]).powi(2)).sqrt();
                edges.push(len(a, b).max(len(b, c)).max(len(c, a)));
            }
            if shown.lines().count() < 6 {
                let _ = writeln!(shown, "    triangle {j}: {x:?} = {:?} {:?} {:?}", position(x[0]), position(x[1]), position(x[2]));
            }
        }
        let _ = writeln!(out, "    triangles: {null} null, {out_of_range} with an index past the vertex count (not drawn)");
        edges.sort_by(|a, b| a.total_cmp(b));
        if let (Some(&max), Some(&median)) = (edges.last(), edges.get(edges.len() / 2)) {
            let _ = writeln!(out, "    longest edge of a triangle in NDC: median {median}, largest {max} ({} triangles)", edges.len());
        }
        out.push_str(&shown);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_dump_is_off_unless_a_frame_is_named() {
        assert_eq!(schedule(None, Some("3"), None, None), None);
        assert_eq!(schedule(Some("x"), None, None, None), None);
        assert_eq!(schedule(Some(" 1800 "), None, None, None), Some(Settings { frame: 1800, draws: 24, records: 4, args: false }));
        assert_eq!(schedule(Some("0"), Some("2"), Some("0"), Some("1")), Some(Settings { frame: 0, draws: 2, records: 4, args: true }));
    }

    /// A record of 4 vertices and 3 triangles: one drawn, one null, one with an index past
    /// the vertex count; vertex 2 is all zero and vertex 3 has no finite position.
    #[test]
    fn a_record_is_read_as_the_pass_through_reads_it() {
        let ms = MsInterface { outputs: Vec::new(), threads: 8, max_vertices: 8, max_primitives: 8 };
        let mut r = vec![0u32; 2 * ms.record_dwords()];
        r[0] = 4;
        r[1] = 3;
        let f = |v: f32| v.to_bits();
        r[MESH_HEADER..MESH_HEADER + 4].copy_from_slice(&[f(-1.0), f(-1.0), f(0.5), f(2.0)]);
        r[MESH_HEADER + 4..MESH_HEADER + 8].copy_from_slice(&[f(1.0), f(0.0), f(0.5), f(1.0)]);
        r[MESH_HEADER + 12..MESH_HEADER + 16].copy_from_slice(&[f(f32::NAN), f(0.0), f(0.0), f(1.0)]);
        let tris = MESH_HEADER + 8 * ms.vert_dwords();
        r[tris] = 1 << 10; // (0, 1, 0)
        r[tris + 1] = 1 << 31;
        r[tris + 2] = 5 << 20;
        let text = report(&ms, &r, 2);
        assert!(text.contains("record 0: 4 vertices, 3 triangles"), "{text}");
        assert!(text.contains("vertices all zero, by quarter of the group: [0, 1, 0, 0]; not finite: [0, 1, 0, 0]"), "{text}");
        assert!(text.contains("NDC box: x -0.5 .. 1, y -0.5 .. 0"), "{text}");
        assert!(text.contains("triangles: 1 null, 1 with an index past the vertex count"), "{text}");
        assert!(text.contains("median 1.5811388, largest 1.5811388 (1 triangles)"), "{text}");
        assert!(text.contains("triangle 0: [0, 1, 0]"), "{text}");
        assert!(text.contains("record 1: 0 vertices, 0 triangles"), "{text}");
    }
}
