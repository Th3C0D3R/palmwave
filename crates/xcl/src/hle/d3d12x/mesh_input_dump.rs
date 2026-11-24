//! `XCL_MESH_INPUT_DUMP=n`: what the first pass of the first two-pass mesh draws from shown
//! frame `n` on read (the frame selection of `mesh_dump`), as the GPU has it and as the CPU has
//! it. A retail title's mesh programs take their counts from memory alone: the descriptor of
//! the meshlets is loaded from a descriptor table (a 32-bit root argument), the meshlet count
//! is dword 0 of that buffer and meshlet m's vertex and triangle counts are bits 0..6 and
//! 7..13 (each + 1) of the dword at 12 + 16 m. Zeros there – an empty descriptor, memory
//! the GPU does not have, a CPU write that did not reach it – give a record of 1 vertex and 1
//! triangle.
//!
//! For each draw taken: every root argument that is an address (`bind::root_arguments`). Of a
//! descriptor table, its first slots; of each slot there that holds a buffer descriptor (V#),
//! and of each root buffer, the first bytes at its address – and, of a table's buffers, the
//! dword at 4 · (c & 0x7ffffff) for a first root constant c with bit 27 set: the draw's word of
//! the programs that put several meshlets in a group (bits 29..31 of c), whose counts are the
//! meshlet's times min(first + meshlets a group, word >> 15) − first. The bytes are copied from the
//! window buffer the shader reads (window = bits 32..33 of the address, offset = its low 32
//! bits) right after the compute pass, in the title's list, and written two presents later
//! next to the CPU's bytes at the same place (read when the draw was recorded) to
//! `mesh_inputs_<n>.txt` in the logs folder.
//!
//! `XCL_MESH_INPUT_DUMP_DRAWS` (24), `XCL_MESH_INPUT_DUMP_SLOTS` (96 slots of a table) and
//! `XCL_MESH_INPUT_DUMP_BYTES` (64 bytes of a buffer) bound the copy.

use std::collections::HashSet;
use std::ffi::c_void;
use std::fmt::Write as _;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Mutex, OnceLock};

use windows::core::Interface;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT_UNKNOWN, DXGI_SAMPLE_DESC};

const SLOT: usize = super::console_heap::SLOT as usize;
/// buffers followed from the tables of one draw
const MAX_BUFFERS: usize = 64;

/// A root argument the dump follows.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Root {
    /// a descriptor table: the 32-bit address of its first slot
    Table(u32),
    /// a root CBV / SRV / UAV
    Buffer(u64),
    /// the first of a parameter's root constants
    Constant(u32),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Settings {
    frame: u64,
    draws: usize,
    slots: usize,
    bytes: usize,
}

/// None: the dump is off.
fn schedule(frame: Option<&str>, draws: Option<&str>, slots: Option<&str>, bytes: Option<&str>) -> Option<Settings> {
    let count = |v: Option<&str>, default: usize, most: usize| v.and_then(|v| v.trim().parse().ok()).filter(|&n| n > 0).unwrap_or(default).min(most);
    Some(Settings {
        frame: frame?.trim().parse().ok()?,
        draws: count(draws, 24, usize::MAX),
        slots: count(slots, 96, 1024),
        bytes: count(bytes, 64, 4096).next_multiple_of(4),
    })
}

fn settings() -> Option<Settings> {
    static S: OnceLock<Option<Settings>> = OnceLock::new();
    *S.get_or_init(|| {
        let var = |name: &str| std::env::var(name).ok();
        schedule(var("XCL_MESH_INPUT_DUMP").as_deref(), var("XCL_MESH_INPUT_DUMP_DRAWS").as_deref(), var("XCL_MESH_INPUT_DUMP_SLOTS").as_deref(), var("XCL_MESH_INPUT_DUMP_BYTES").as_deref())
    })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Kind {
    /// the slots of a descriptor table
    Table,
    /// the first bytes of a buffer
    Buffer,
    /// a draw's word in a buffer (see the module docs)
    DrawWord,
}

/// Bytes read at an address.
#[derive(Clone, Debug, PartialEq, Eq)]
struct Region {
    label: String,
    /// the address as the program has it
    address: u64,
    /// where the shader reads it: the address in unified memory
    view: usize,
    len: usize,
    kind: Kind,
    /// the CPU's bytes when the draw was recorded (None: nothing readable there)
    cpu: Option<Vec<u8>>,
    /// offset in the capture's readback buffer (None: not copied – `place` says why)
    at: Option<u64>,
    place: String,
}

/// A buffer descriptor: its address, stride (0: raw) and records.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Vsharp {
    address: u64,
    stride: u32,
    records: u32,
}

fn dwords(b: &[u8]) -> Vec<u32> {
    b.chunks_exact(4).map(|c| u32::from_le_bytes(c.try_into().unwrap())).collect()
}

/// The V# in a slot. None: an image (type bits 28..31 of word 3 from 8), an empty slot, or
/// words that are no descriptor (a V# has the identity swizzle in word 3, `desc::buffer_words`).
fn vsharp(slot: &[u8]) -> Option<Vsharp> {
    let w = dwords(slot.get(..16)?);
    let address = w[0] as u64 | ((w[1] & 0xFFFF) as u64) << 32;
    (w[3] >> 28 < 8 && w[3] & 0xFFF == 0xFAC && address != 0).then_some(Vsharp { address, stride: (w[1] >> 16) & 0x3FFF, records: w[2] })
}

/// Where a translated shader reads `address`: window = bits 32..33, offset = the low 32 bits.
fn shader_view(base: usize, address: u64) -> usize {
    base + (address & 0x3_FFFF_FFFF) as usize
}

/// What a draw with these root arguments reads: each table's slots, the buffers its V# name
/// (from the CPU's view of the slots, `cpu(address, bytes)`) with the draw's word where they
/// hold one, each root buffer.
fn plan(base: usize, roots: &[(usize, Root)], s: &Settings, cpu: &dyn Fn(usize, usize) -> Option<Vec<u8>>) -> Vec<Region> {
    let region = |label: String, address: u64, len: usize, kind: Kind| {
        let view = shader_view(base, address);
        Region { label, address, view, len, kind, cpu: cpu(view, len), at: None, place: String::new() }
    };
    let word_of = |r: &(usize, Root)| match r.1 {
        Root::Constant(c) if c & 0x800_0000 != 0 => Some(4 * (c & 0x7FF_FFFF) as u64),
        _ => None,
    };
    let words: Vec<u64> = roots.iter().filter_map(word_of).collect();
    let mut out = Vec::new();
    let mut buffers = 0;
    for &(i, root) in roots {
        match root {
            Root::Constant(_) => {}
            Root::Buffer(va) => out.push(region(format!("p{i} root buffer"), va, s.bytes, Kind::Buffer)),
            Root::Table(t) => {
                let table = region(format!("p{i} table"), t as u64, s.slots * SLOT, Kind::Table);
                let slots = table.cpu.clone().unwrap_or_default();
                out.push(table);
                for (k, v) in slots.chunks_exact(SLOT).enumerate().filter_map(|(k, w)| Some((k, vsharp(w)?))) {
                    if buffers == MAX_BUFFERS {
                        break;
                    }
                    buffers += 1;
                    out.push(region(format!("p{i} slot {k} (+{:#x}) buffer", k * SLOT), v.address, s.bytes, Kind::Buffer));
                    let bytes = v.records as u64 * v.stride.max(1) as u64;
                    for &at in words.iter().filter(|&&at| at + 4 <= bytes) {
                        out.push(region(format!("p{i} slot {k} (+{:#x}) buffer, the draw's word at +{at:#x}", k * SLOT), v.address + at, 4, Kind::DrawWord));
                    }
                }
            }
        }
    }
    out
}

struct Capture {
    frame: u64,
    buffer: Option<ID3D12Resource>,
    what: String,
    io: Vec<u32>,
    regions: Vec<Region>,
}

unsafe impl Send for Capture {}

static PENDING: Mutex<Vec<Capture>> = Mutex::new(Vec::new());
static TAKEN: AtomicUsize = AtomicUsize::new(0);

/// A two-pass draw recorded now is one the dump takes: from shown frame `frame` on, until
/// `draws` draws are taken.
pub(super) fn due() -> bool {
    settings().is_some_and(|s| super::present::presents() >= s.frame && TAKEN.load(Ordering::Relaxed) < s.draws)
}

/// Where `r` is, and whether the GPU's bytes can be copied from there.
fn place(u: &crate::gpu::uma::Uma, mapped: &[(usize, usize)], r: &Region) -> (String, bool) {
    let window = (r.view - u.base()) / crate::gpu::uma::WINDOW;
    let mut text = format!("window {window}");
    if r.address > u32::MAX as u64 && r.address as usize != r.view {
        let _ = write!(text, ", NOT in unified memory: the shader reads {:#x} instead", r.view);
    }
    if !u.is_sparse_window(window) {
        return (text + ", coherent", true);
    }
    text.push_str(if u.sparse_reservation(r.view).is_some() { ", sparse, the title's memory" } else { ", sparse, NO reservation of the title" });
    let tile_mapped = |p: usize| mapped.get(mapped.partition_point(|&(_, end)| end <= p)).is_some_and(|&(start, _)| start <= p);
    let on_gpu = tile_mapped(r.view) && tile_mapped(r.view + r.len - 1);
    text.push_str(if on_gpu { ", tile mapped" } else { ", tile NOT mapped (the GPU has no memory here)" });
    (text, on_gpu)
}

/// After a draw's compute pass, in the title's list: copy what the pass read (see the module
/// docs) to a readback buffer. `what` names the draw, `io` holds its SGPRs, `roots` its root
/// arguments (`bind::root_arguments`).
pub(super) unsafe fn capture(this: *mut c_void, list: &ID3D12GraphicsCommandList, what: String, io: &[u32], roots: &[(usize, Root)]) {
    let Some(s) = settings() else { return };
    let (Some(u), Some(dev)) = (crate::gpu::uma::get(), super::real::list_device(this)) else { return };
    if TAKEN.fetch_add(1, Ordering::Relaxed) >= s.draws {
        return;
    }
    let cpu = |p: usize, n: usize| crate::gpu::shader::readable(p, n).then(|| std::slice::from_raw_parts(p as *const u8, n).to_vec());
    let mut regions = plan(u.base(), roots, &s, &cpu);
    let mapped = super::sparse::mapped_ranges();
    // (window buffer, offset in it, offset in the readback buffer, bytes)
    let mut copies: Vec<(usize, u64, u64, u64)> = Vec::new();
    let mut size = 0u64;
    for r in &mut regions {
        let (text, on_gpu) = place(u, &mapped, r);
        r.place = text;
        if let Some((res, off)) = super::real::uma_window_buffer(r.view, r.len).filter(|_| on_gpu) {
            copies.push((res, off, size, r.len as u64));
            r.at = Some(size);
            size += r.len as u64;
        }
    }
    let mut buffer: Option<ID3D12Resource> = None;
    if size > 0 {
        let desc = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
            Alignment: 0,
            Width: size,
            Height: 1,
            DepthOrArraySize: 1,
            MipLevels: 1,
            Format: DXGI_FORMAT_UNKNOWN,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            Flags: D3D12_RESOURCE_FLAG_NONE,
        };
        let made = dev.CreateCommittedResource(&D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_READBACK, ..Default::default() }, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, None, &mut buffer);
        if made.is_err() {
            buffer = None;
        }
    }
    if let Some(buffer) = &buffer {
        // the compute pass bound every window as a root UAV: promoted from COMMON to
        // UNORDERED_ACCESS, as the record buffer `mesh_dump` copies
        copies.sort_unstable();
        for window in copies.chunk_by(|a, b| a.0 == b.0) {
            let raw = window[0].0 as *mut c_void;
            let Some(res) = ID3D12Resource::from_raw_borrowed(&raw) else { continue };
            super::bind::emit_transition(list, res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            for &(_, src, dst, n) in window {
                list.CopyBufferRegion(buffer, dst, res, src, n);
            }
            super::bind::emit_transition(list, res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    } else {
        regions.iter_mut().for_each(|r| r.at = None);
    }
    PENDING.lock().unwrap().push(Capture { frame: super::present::presents(), buffer, what, io: io.to_vec(), regions });
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
    let mut body = String::new();
    let mut seen = HashSet::new();
    let mut totals = Totals::default();
    for (k, c) in ready.iter().enumerate() {
        let size = c.regions.iter().filter_map(|r| Some(r.at? as usize + r.len)).max().unwrap_or(0);
        let mut gpu = Vec::new();
        if let Some(buffer) = c.buffer.as_ref().filter(|_| size > 0) {
            let mut p: *mut c_void = std::ptr::null_mut();
            if buffer.Map(0, None, Some(&mut p)).is_ok() && !p.is_null() {
                gpu = std::slice::from_raw_parts(p as *const u8, size).to_vec();
                buffer.Unmap(0, None);
            }
        }
        let _ = writeln!(body, "\ndraw {k}: {}", c.what);
        let _ = writeln!(body, "  s0..s31: {}", c.io.iter().take(32).map(|w| format!("{w:08x}")).collect::<Vec<_>>().join(" "));
        body.push_str(&report(&c.regions, &gpu, &mut seen, &mut totals));
    }
    let text = format!("mesh inputs of shown frame {frame}: {} two-pass draw(s)\n{}\n{body}", ready.len(), totals.text());
    let path = boot.logs_dir().join(format!("mesh_inputs_{frame}.txt"));
    match std::fs::write(&path, text) {
        Ok(()) => crate::xlog_info!("mesh input dump: frame {frame}, {} draw(s) written to {} ({})", ready.len(), path.display(), totals.text()),
        Err(e) => crate::xlog_error!("mesh input dump: {}: {e}", path.display()),
    }
}

/// What the regions of all draws showed.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct Totals {
    /// table slots whose GPU bytes are not the CPU's
    slots_differ: usize,
    /// buffers whose first bytes are all zero on the GPU, and not on the CPU
    gpu_zero: usize,
    /// … differ otherwise
    differ: usize,
    /// … are the same
    same: usize,
    /// … are all zero on both sides
    both_zero: usize,
    /// addresses the GPU has no memory for (not copied)
    not_on_gpu: usize,
    /// addresses that are not in unified memory
    outside: usize,
}

impl Totals {
    fn text(&self) -> String {
        format!(
            "table slots GPU != CPU: {}; buffers: {} same, {} zero on the GPU only, {} different, {} zero on both, {} without GPU memory, {} at an address outside unified memory",
            self.slots_differ, self.same, self.gpu_zero, self.differ, self.both_zero, self.not_on_gpu, self.outside
        )
    }
}

fn hex(b: &[u8]) -> String {
    dwords(b).iter().map(|w| format!("{w:08x}")).collect::<Vec<_>>().join(" ")
}

/// A reading of a draw's regions with the GPU's bytes `gpu` (the readback buffer). A buffer
/// whose address and bytes were written for an earlier draw is named only (`seen`).
fn report(regions: &[Region], gpu: &[u8], seen: &mut HashSet<(u64, Option<Vec<u8>>, Option<Vec<u8>>)>, totals: &mut Totals) -> String {
    let mut out = String::new();
    let mut repeated = Vec::new();
    for r in regions {
        let g = r.at.and_then(|at| gpu.get(at as usize..at as usize + r.len));
        let outside = r.place.contains("NOT in unified memory");
        totals.outside += outside as usize;
        totals.not_on_gpu += (g.is_none() && !outside) as usize;
        if r.kind == Kind::Table {
            let _ = writeln!(out, "  {} {:#x} [{}], {} slots:", r.label, r.address, r.place, r.len / SLOT);
            let (Some(c), Some(g)) = (r.cpu.as_deref(), g) else {
                let _ = writeln!(out, "    {}", if r.cpu.is_none() { "the CPU has nothing readable there" } else { "not copied from the GPU" });
                continue;
            };
            let mut empty = Vec::new();
            for (k, (cs, gs)) in c.chunks_exact(SLOT).zip(g.chunks_exact(SLOT)).enumerate() {
                let differs = cs != gs;
                totals.slots_differ += differs as usize;
                if cs.iter().all(|&b| b == 0) && !differs {
                    empty.push(k.to_string());
                    continue;
                }
                let v = vsharp(cs);
                if v.is_none() && !differs {
                    continue;
                }
                let _ = write!(out, "    slot {k} (+{:#x}): CPU {}", k * SLOT, hex(&cs[..16]));
                if differs {
                    let _ = write!(out, " | GPU {} DIFFERENT", hex(&gs[..16]));
                }
                match v {
                    Some(v) => {
                        let _ = writeln!(out, " -> buffer {:#x}, stride {}, {} records", v.address, v.stride, v.records);
                    }
                    None => out.push('\n'),
                }
            }
            let _ = writeln!(out, "    empty slots (all zero, CPU and GPU): {}", if empty.is_empty() { "none".into() } else { empty.join(" ") });
            continue;
        }
        let verdict = match (r.cpu.as_deref(), g) {
            (Some(c), Some(g)) if c == g && c.iter().all(|&b| b == 0) => {
                totals.both_zero += 1;
                "all zero, CPU and GPU"
            }
            (Some(c), Some(g)) if c == g => {
                totals.same += 1;
                "GPU = CPU"
            }
            (Some(_), Some(g)) if g.iter().all(|&b| b == 0) => {
                totals.gpu_zero += 1;
                "ZERO ON THE GPU, not on the CPU"
            }
            (Some(_), Some(_)) => {
                totals.differ += 1;
                "GPU DIFFERENT from the CPU"
            }
            (None, Some(_)) => "the CPU has nothing readable there",
            (_, None) => "not copied from the GPU",
        };
        if !seen.insert((r.address, r.cpu.clone(), g.map(|g| g.to_vec()))) {
            repeated.push(r.label.clone());
            continue;
        }
        let _ = writeln!(out, "  {} {:#x} [{}]: {verdict}", r.label, r.address, r.place);
        if let Some(c) = &r.cpu {
            let _ = writeln!(out, "    CPU {}", hex(c));
        }
        if let Some(g) = g {
            if r.cpu.as_deref() != Some(g) {
                let _ = writeln!(out, "    GPU {}", hex(g));
            }
            // as the title's mesh programs read them (see the module docs)
            match (r.kind, &dwords(g)[..]) {
                (Kind::DrawWord, &[w]) => {
                    let _ = writeln!(out, "    read as the draw's word on the GPU: {} meshlets at most (word >> 15), list {:#x}", w >> 15, w & 0x7FFF);
                }
                (Kind::Buffer, &[count, _, _, first, ..]) => {
                    let _ = writeln!(out, "    read as meshlets on the GPU: {} meshlets, the first of {} vertices and {} triangles", count & 0x1FFFF, (first & 0x7F) + 1, ((first >> 7) & 0x7F) + 1);
                }
                _ => {}
            }
        }
    }
    if !repeated.is_empty() {
        let _ = writeln!(out, "  as written for an earlier draw (same address, same bytes): {}", repeated.join(", "));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    const BASE: usize = 0x4_0000_0000;

    #[test]
    fn the_dump_is_off_unless_a_frame_is_named() {
        assert_eq!(schedule(None, Some("3"), None, None), None);
        assert_eq!(schedule(Some("x"), None, None, None), None);
        assert_eq!(schedule(Some(" 1800 "), None, None, None), Some(Settings { frame: 1800, draws: 24, slots: 96, bytes: 64 }));
        assert_eq!(schedule(Some("0"), Some("600"), Some("0"), Some("30")), Some(Settings { frame: 0, draws: 600, slots: 96, bytes: 32 }));
    }

    #[test]
    fn a_slot_is_a_buffer_when_it_holds_a_v_sharp() {
        let slot = |w: [u32; 4]| w.iter().flat_map(|w| w.to_le_bytes()).chain([0; 16]).collect::<Vec<u8>>();
        let v = crate::gpu::desc::buffer_words(0x6_3F0F_0000, 0, 4096, 22);
        assert_eq!(vsharp(&slot(v)), Some(Vsharp { address: 0x6_3F0F_0000, stride: 0, records: 4096 }));
        assert_eq!(vsharp(&slot(crate::gpu::desc::cbv_words(0x4_4000_0100, 256))), Some(Vsharp { address: 0x4_4000_0100, stride: 16, records: 16 }));
        assert_eq!(vsharp(&slot([0; 4])), None, "an empty V#");
        assert_eq!(vsharp(&slot([7, 0x100, 0x3FF, 0x9000_0FAC])), None, "an image");
        assert_eq!(vsharp(&slot([0x3F80_0000; 4])), None, "floats past the end of a table");
    }

    /// A table whose slot 5 names a buffer the title wrote and the GPU has zeros for, and a
    /// root buffer at an address that is not unified memory.
    #[test]
    fn a_draws_tables_and_buffers_are_read_and_compared() {
        let s = Settings { frame: 0, draws: 1, slots: 8, bytes: 16 };
        let meshlets = crate::gpu::desc::buffer_words(0x5_0001_0000, 0, 4096, 22);
        let cpu = |p: usize, n: usize| -> Option<Vec<u8>> {
            match p {
                0x4_4D54_4460 => {
                    let mut t = vec![0u8; n];
                    t[5 * SLOT..5 * SLOT + 16].copy_from_slice(&meshlets.iter().flat_map(|w| w.to_le_bytes()).collect::<Vec<u8>>());
                    Some(t)
                }
                0x5_0001_0000 => Some([3u32, 0, 0, 63 | 99 << 7].iter().flat_map(|w| w.to_le_bytes()).collect()),
                _ => None,
            }
        };
        let roots = [(1, Root::Table(0x4D54_4460)), (3, Root::Buffer(0xE_3F0F_0000)), (4, Root::Constant(0x2800_0021)), (5, Root::Constant(0x21))];
        let mut regions = plan(BASE, &roots, &s, &cpu);
        let labels: Vec<&str> = regions.iter().map(|r| r.label.as_str()).collect();
        assert_eq!(labels, ["p1 table", "p1 slot 5 (+0xa0) buffer", "p1 slot 5 (+0xa0) buffer, the draw's word at +0x84", "p3 root buffer"]);
        assert_eq!(regions[0].view, 0x4_4D54_4460, "a 32-bit table address is in window 0");
        assert_eq!((regions[2].view, regions[2].len), (0x5_0001_0084, 4), "4 x the low 27 bits of the constant with bit 27");
        // (the draw's word is read at the end)
        let word = regions.remove(2);
        assert_eq!(word.kind, Kind::DrawWord);
        assert_eq!(regions[2].view, 0x6_3F0F_0000, "bits 32..33 of an address pick the window");
        // the GPU has the table as the CPU wrote it, and zeros where the meshlets are
        let mut gpu = regions[0].cpu.clone().unwrap();
        gpu.extend([0u8; 16]);
        (regions[0].at, regions[1].at) = (Some(0), Some(8 * SLOT as u64));
        regions[0].place = "window 0, sparse, the title's memory, tile mapped".into();
        regions[2].place = "window 2, NOT in unified memory: the shader reads 0x63f0f0000 instead".into();
        let (mut seen, mut totals) = (HashSet::new(), Totals::default());
        let text = report(&regions, &gpu, &mut seen, &mut totals);
        assert!(text.contains("slot 5 (+0xa0): CPU 00010000 00000005 00001000 31016fac -> buffer 0x500010000, stride 0, 4096 records"), "{text}");
        assert!(text.contains("empty slots (all zero, CPU and GPU): 0 1 2 3 4 6 7"), "{text}");
        assert!(text.contains("p1 slot 5 (+0xa0) buffer 0x500010000 []: ZERO ON THE GPU, not on the CPU"), "{text}");
        assert!(text.contains("CPU 00000003 00000000 00000000 000031bf"), "{text}");
        assert!(text.contains("read as meshlets on the GPU: 0 meshlets, the first of 1 vertices and 1 triangles"), "{text}");
        assert!(text.contains("p3 root buffer 0xe3f0f0000 [window 2, NOT in unified memory: the shader reads 0x63f0f0000 instead]: not copied from the GPU"), "{text}");
        assert_eq!(totals, Totals { gpu_zero: 1, outside: 1, ..Default::default() });
        // the GPU's table differs in slot 5; the same buffer in a second draw is not written again
        gpu[5 * SLOT] = 0xFF;
        let text = report(&regions, &gpu, &mut seen, &mut totals);
        assert!(text.contains("| GPU 000100ff 00000005 00001000 31016fac DIFFERENT"), "{text}");
        assert!(text.contains("as written for an earlier draw (same address, same bytes): p1 slot 5 (+0xa0) buffer, p3 root buffer"), "{text}");
        assert_eq!(totals.slots_differ, 1);
        let word = Region { at: Some(0), ..word };
        let text = report(&[word], &(3u32 << 15 | 0x7FFF).to_le_bytes(), &mut seen, &mut totals);
        assert!(text.contains("read as the draw's word on the GPU: 3 meshlets at most (word >> 15), list 0x7fff"), "{text}");
    }
}
