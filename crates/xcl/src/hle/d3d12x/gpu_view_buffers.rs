//! Buffers in a GPU view of the title's CPU memory, where translated shaders can read them.
//!
//! A retail title reserves its streaming memory twice (`kernelx::mirrors`): a CPU view it maps
//! physical pages into, and a GPU view of the same size it places resources in – textures, and
//! buffers too (mesh data read by its mesh shaders). On the console the GPU view shows the
//! pages the title maps under it (queue `CopyPageMappingsBatchX`). Here it is 16 to 24 GiB of
//! empty address space outside the unified-memory windows: a shader reaches memory through
//! `mem`, whose window is `(address >> 32) & 3` (`gpu::uma`), so a V# with a GPU-view address
//! read some other window's bytes, and a buffer view of a resource placed there got an empty
//! V#. Textures have `title_textures` (uploads from where the pages are); buffers had nothing.
//!
//! A range of a GPU view that a buffer view, a root descriptor or an index / vertex buffer
//! names gets a *mirror*: address space of the same size in a sparse window, whose 64 KiB
//! pages are committed (and so given video memory, `sparse`) only while the page under the
//! GPU view holds something. Each page is copied from where its bytes are
//! (`title_textures::copy_gpu_view`: the physical page the page-mapping method put there, else
//! the CPU view) when the mirror is made, when the method changes the page or its physical
//! page moves (`pages_changed`), and when the CPU writes the source (`written`, from the write
//! watch of the title's submits). The mirror's address goes into the V#; it stays the same for
//! as long as the GPU view lives, like the console's.
//!
//! What the GPU writes into a mirror goes back to the title's pages. A mirror an unordered
//! access view or a memory write names is *GPU-written* (`mark_written`): after each of the
//! title's submissions its pages are copied back from video memory (`written_ranges`, by
//! `sparse::readback_after_execute`, which also holds the title's fence signals back until the
//! copy has landed), and what the GPU changed is written to where the page's bytes are
//! (`landed`). Both directions go by 64-byte chunks against the page's last common content
//! (`Written::base`), so a copy one way never carries the other side's stale bytes: a chunk
//! the CPU wrote goes to the mirror, a chunk the GPU wrote goes to the title's page, and where
//! both wrote the CPU's stays (it wrote after the submission).
//!
//! `XCL_GPU_VIEW_BUFFERS=0` turns the mirrors off; `XCL_GPU_VIEW_BUFFERS_MB=n` is the address
//! space they may take (4096). `XCL_GPU_VIEW_WRITEBACK=0` turns the copies back off;
//! `XCL_GPU_VIEW_WRITEBACK_MB=n` is the most a submission copies back (64).

use std::collections::BTreeMap;
use std::ffi::c_void;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;

use windows_sys::Win32::System::Memory::{VirtualAlloc, VirtualFree, MEM_COMMIT, MEM_DECOMMIT, PAGE_READWRITE};

/// The console's 64 KiB pages: what the page-mapping method maps, and a sparse window's tiles.
const PAGE: usize = 0x1_0000;
/// The range a root descriptor in a GPU view gets (it has no size): pages cost nothing until
/// the title maps something under them.
const ROOT_RANGE: usize = 1 << 20;
/// The largest mirror that may come from a committed window when no sparse window has room.
const COMMITTED_MOST: usize = 4 << 20;
/// What the two directions of a GPU-written page are compared by (`sparse` sends the CPU's
/// writes to video memory by the same chunks).
const CHUNK: usize = 64;

/// A live page of a GPU-written mirror.
struct Written {
    /// what the mirror's page and the title's page both held last
    base: Box<[u8]>,
    /// per chunk: `SEQ` when the CPU's bytes went to the mirror last
    cpu_seq: Box<[u64]>,
    /// `LOOK` when the page was filled: until a submit's look at the CPU's writes that began
    /// after it is done (`in_video_memory`), video memory does not hold these bytes, and what
    /// comes back from there is not the GPU's writing
    since: u64,
}

impl Written {
    unsafe fn of(here: usize) -> Written {
        Written {
            base: std::slice::from_raw_parts(here as *const u8, PAGE).into(),
            cpu_seq: vec![SEQ.load(Ordering::Acquire); PAGE / CHUNK].into(),
            since: LOOK[0].load(Ordering::Acquire),
        }
    }

    /// Whether the page as it was filled has gone to video memory.
    fn in_video_memory(&self) -> bool {
        LOOK[1].load(Ordering::Acquire) > self.since
    }
}

struct Mirror {
    /// bytes (a multiple of `PAGE`)
    len: usize,
    /// its address in unified memory
    at: usize,
    /// in a sparse window (pages committed one by one), else a block of a committed window
    sparse: bool,
    /// per page: where its first byte was copied from last (0: an unmapped page)
    sources: Vec<usize>,
    /// per page: committed here – its source held something
    live: Vec<bool>,
    /// the GPU may write it (`mark_written`): per live page, what is needed to copy back
    gpu: Option<Vec<Option<Written>>>,
}

#[derive(Default)]
struct State {
    /// GPU-view address (64 KiB aligned) → its mirror
    mirrors: BTreeMap<usize, Mirror>,
    /// where a source page starts → the mirror pages copied from it: (mirror's GPU-view
    /// address, page index)
    by_source: BTreeMap<usize, Vec<(usize, u32)>>,
    /// a mirror's address in unified memory → its GPU-view address
    by_at: BTreeMap<usize, usize>,
    /// address space the mirrors take
    bytes: usize,
    /// pages committed
    live: usize,
}

static STATE: Mutex<Option<State>> = Mutex::new(None);
/// number of mirrors (0: nothing to do for a submit)
static MIRRORS: AtomicU64 = AtomicU64::new(0);
/// changes with the set of source pages (`source_ranges`)
static GEN: AtomicU64 = AtomicU64::new(0);
/// pages copied again: after CPU writes, after a page-mapping change
static REFRESHED: [AtomicU64; 2] = [AtomicU64::new(0), AtomicU64::new(0)];
/// pages copied from memory no write watch covers: what the CPU writes there later is not seen
/// until the page is mapped again
static UNWATCHED: AtomicU64 = AtomicU64::new(0);
/// number of GPU-written mirrors (0: nothing is copied back)
static GPU_MIRRORS: AtomicU64 = AtomicU64::new(0);
/// counts the copies back from video memory (`next_seq`): which of a chunk's two sides is newer
static SEQ: AtomicU64 = AtomicU64::new(0);
/// the looks of the title's submits at the CPU's writes (`title_textures::refresh_once`): how
/// many began, and the number of the latest one that is done
static LOOK: [AtomicU64; 2] = [AtomicU64::new(0), AtomicU64::new(0)];
/// copies back: pages landed, chunks written to the title's pages, chunks where the CPU's
/// bytes stayed, pages with nowhere to write
static BACK: [AtomicU64; 4] = [AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0)];

/// Whether CPU writes at `source` are seen (`title_textures::cached_watch_ranges`: the
/// unified-memory windows and the title's watched reservations).
fn watched(source: usize) -> bool {
    crate::gpu::uma::get().is_some_and(|u| u.contains(source)) || crate::hle::kernelx::in_graphics_reservation(source)
}

fn enabled() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var("XCL_GPU_VIEW_BUFFERS").map_or(true, |v| v.trim() != "0"))
}

/// address space the mirrors may take (`XCL_GPU_VIEW_BUFFERS_MB`)
fn budget() -> usize {
    static MB: std::sync::OnceLock<usize> = std::sync::OnceLock::new();
    *MB.get_or_init(|| std::env::var("XCL_GPU_VIEW_BUFFERS_MB").ok().and_then(|v| v.trim().parse().ok()).unwrap_or(4096)) << 20
}

fn writeback() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var("XCL_GPU_VIEW_WRITEBACK").map_or(true, |v| v.trim() != "0"))
}

/// the most one submission copies back (`XCL_GPU_VIEW_WRITEBACK_MB`)
fn writeback_most() -> usize {
    static MB: std::sync::OnceLock<usize> = std::sync::OnceLock::new();
    *MB.get_or_init(|| std::env::var("XCL_GPU_VIEW_WRITEBACK_MB").ok().and_then(|v| v.trim().parse().ok()).unwrap_or(64)) << 20
}

/// A submit of the title's begins its look at what the CPU wrote: its number.
pub fn look_begins() -> u64 {
    LOOK[0].fetch_add(1, Ordering::AcqRel) + 1
}

/// The look `number` is done: what the mirrors held when it began is on its way to video
/// memory, ahead of the submit.
pub fn look_done(number: u64) {
    LOOK[1].fetch_max(number, Ordering::AcqRel);
}

/// The number of a copy back from video memory that is about to be submitted
/// (`sparse::readback_after_execute`): `landed` gets it with the copy.
pub(super) fn next_seq() -> u64 {
    SEQ.fetch_add(1, Ordering::AcqRel) + 1
}

/// Generation of the mirrors' source pages: `source_ranges` changed.
pub(super) fn gen() -> u64 {
    GEN.load(Ordering::Acquire)
}

/// The memory the mirrors are copied from, `(start, end)` each: the write watch of the title's
/// submits looks there too (`written`).
pub(super) fn source_ranges() -> Vec<(usize, usize)> {
    STATE.lock().unwrap().as_ref().map_or(Vec::new(), |s| s.by_source.keys().map(|&p| (p, p + PAGE)).collect())
}

impl State {
    fn unlink(&mut self, source: usize, of: (usize, u32)) {
        if let Some(v) = self.by_source.get_mut(&source) {
            v.retain(|&m| m != of);
            if v.is_empty() {
                self.by_source.remove(&source);
                GEN.fetch_add(1, Ordering::Release);
            }
        }
    }

    /// Page `index` of the mirror of `gva` again from where its bytes are now. `true`: it holds
    /// something here (its bytes may have changed).
    unsafe fn sync_page(&mut self, gva: usize, index: usize) -> bool {
        let Some(m) = self.mirrors.get_mut(&gva) else { return false };
        let (page_va, here) = (gva + index * PAGE, m.at + index * PAGE);
        let source = super::title_textures::gpu_view_source(page_va).unwrap_or(0);
        let key = source;
        let old = std::mem::replace(&mut m.sources[index], key);
        let holds = source != 0 && crate::gpu::shader::readable(source, 1);
        let was = m.live[index];
        let sparse = m.sparse;
        let mut live = was;
        if holds {
            if sparse && !was {
                if VirtualAlloc(here as *const c_void, PAGE, MEM_COMMIT, PAGE_READWRITE).is_null() {
                    m.sources[index] = old;
                    return false;
                }
                super::sparse::committed(here, PAGE);
            }
            live = true;
            // (another page under it, or the first copy: all of it; else, for a page the GPU
            // writes, only what the CPU changed – the rest of the mirror may be the GPU's)
            let merge = was && old == key;
            match m.gpu.as_mut().map(|g| &mut g[index]) {
                Some(Some(w)) if merge => {
                    let mut now = vec![0u8; PAGE];
                    super::title_textures::copy_gpu_view(page_va, now.as_mut_ptr(), PAGE);
                    let seq = SEQ.load(Ordering::Acquire);
                    for (c, (new, base)) in now.chunks_exact(CHUNK).zip(w.base.chunks_exact_mut(CHUNK)).enumerate() {
                        if new != base {
                            std::ptr::copy_nonoverlapping(new.as_ptr(), (here + c * CHUNK) as *mut u8, CHUNK);
                            base.copy_from_slice(new);
                            w.cpu_seq[c] = seq;
                        }
                    }
                }
                Some(slot) => {
                    super::title_textures::copy_gpu_view(page_va, here as *mut u8, PAGE);
                    *slot = Some(Written::of(here));
                }
                None => {
                    super::title_textures::copy_gpu_view(page_va, here as *mut u8, PAGE);
                }
            }
            if !watched(source) {
                UNWATCHED.fetch_add(1, Ordering::Relaxed);
            }
        } else if was {
            if sparse {
                // sparse first, as `kernelx::VirtualFree` does: no copy to video memory reads the
                // page while it goes
                super::sparse::decommitted(here, PAGE);
                VirtualFree(here as *mut c_void, PAGE, MEM_DECOMMIT);
            } else {
                std::ptr::write_bytes(here as *mut u8, 0, PAGE);
            }
            live = false;
            if let Some(g) = m.gpu.as_mut() {
                g[index] = None;
            }
        }
        m.live[index] = live;
        if live != was {
            if live { self.live += 1 } else { self.live -= 1 }
        }
        if old != key {
            let of = (gva, index as u32);
            if old != 0 {
                self.unlink(old, of);
            }
            if key != 0 {
                let v = self.by_source.entry(key).or_default();
                if v.is_empty() {
                    GEN.fetch_add(1, Ordering::Release);
                }
                v.push(of);
            }
        }
        live
    }

    /// The mirror holding `[va, va + bytes)`, if one does: its GPU-view address.
    fn holding(&self, va: usize, bytes: usize) -> Option<usize> {
        // (mirrors may overlap: the nearest few that start at or before `va`)
        self.mirrors.range(..=va).rev().take(8).find(|(&g, m)| va + bytes <= g + m.len).map(|(&g, _)| g)
    }

    unsafe fn drop_mirror(&mut self, gva: usize) {
        let Some(m) = self.mirrors.remove(&gva) else { return };
        for (i, &s) in m.sources.iter().enumerate() {
            if s != 0 {
                self.unlink(s, (gva, i as u32));
            }
        }
        self.live -= m.live.iter().filter(|&&l| l).count();
        self.bytes -= m.len;
        self.by_at.remove(&m.at);
        if m.gpu.is_some() {
            GPU_MIRRORS.fetch_sub(1, Ordering::Release);
        }
        if m.sparse {
            super::sparse::free(m.at);
        } else if let Some(u) = crate::gpu::uma::get() {
            u.free(m.at);
        }
        MIRRORS.fetch_sub(1, Ordering::Release);
    }
}

/// Where translated shaders read `[va, va + bytes)` when the whole range lies in a GPU view of
/// the title's CPU memory: its place in a mirror, made (and filled) the first time. `None`: not
/// in a GPU view, switched off, or no room.
pub unsafe fn address(va: usize, bytes: usize, what: &str) -> Option<usize> {
    if bytes == 0 || !crate::hle::kernelx::mirrors::in_some_view(va) || !enabled() {
        return None;
    }
    crate::hle::kernelx::mirrors::source_range(va, bytes)?;
    let mut g = STATE.lock().unwrap();
    let s = g.get_or_insert_with(State::default);
    if let Some(gva) = s.holding(va, bytes) {
        return Some(s.mirrors[&gva].at + (va - gva));
    }
    let start = va & !(PAGE - 1);
    let len = (va + bytes).checked_next_multiple_of(PAGE)? - start;
    if s.bytes + len > budget() {
        static FULL: std::sync::Once = std::sync::Once::new();
        FULL.call_once(|| crate::xlog_warn!("D3D12.X: {} MB of address space for buffers in GPU views is taken (XCL_GPU_VIEW_BUFFERS_MB) – further ones ({what} at {va:#x}, {bytes} bytes) read nothing", budget() >> 20));
        return None;
    }
    let u = crate::gpu::uma::get()?;
    let (at, sparse) = match u.sparse_alloc(len, PAGE) {
        Some((at, _)) => (at, true),
        // (no sparse window, or they are full: a small one from the coherent window, which is
        // committed memory and the title's GPU heaps')
        None if len <= COMMITTED_MOST => (u.alloc(len, PAGE)? as usize, false),
        None => {
            static NO_ROOM: std::sync::Once = std::sync::Once::new();
            NO_ROOM.call_once(|| crate::xlog_warn!("D3D12.X: no address space in the sparse windows for {what} at {va:#x} ({bytes} bytes) in a GPU view – shaders read nothing there"));
            return None;
        }
    };
    let pages = len / PAGE;
    s.mirrors.insert(start, Mirror { len, at, sparse, sources: vec![0; pages], live: vec![false; pages], gpu: None });
    s.by_at.insert(at, start);
    s.bytes += len;
    let n = MIRRORS.fetch_add(1, Ordering::Release) + 1;
    let before = s.live;
    for i in 0..pages {
        s.sync_page(start, i);
    }
    if n <= 4 || n.is_power_of_two() {
        let mapped = (0..pages).filter(|i| crate::hle::kernelx::mirrors::mapped_source(start + i * PAGE).is_some()).count();
        crate::xlog_info!(
            "D3D12.X: {what} at {va:#x} ({bytes} bytes) lies in a GPU view of the title's CPU memory: shaders read it at {:#x} – {} of its {pages} pages hold something, {mapped} were mapped by CopyPageMappingsBatchX, its first byte is read at {:#x} ({n} such ranges, {} MB of address space, {} MB of pages with data so far; {} page copies came from memory without a write watch)",
            at + (va - start),
            s.live - before,
            super::title_textures::gpu_view_source(va).unwrap_or(0),
            s.bytes >> 20,
            (s.live * PAGE) >> 20,
            UNWATCHED.load(Ordering::Relaxed)
        );
    }
    Some(at + (va - start))
}

/// The GPU may write `[addr, addr + bytes)` – an unordered access view of it was made, or a
/// memory write names it: when it is in a mirror (`addr`: its GPU-view address or the mirror's
/// own), the mirror's pages are copied back to the title's pages after each submission.
pub unsafe fn mark_written(addr: usize, bytes: usize, what: &str) {
    if MIRRORS.load(Ordering::Acquire) == 0 || bytes == 0 || !writeback() {
        return;
    }
    let mut g = STATE.lock().unwrap();
    let Some(s) = g.as_mut() else { return };
    let gva = if crate::hle::kernelx::mirrors::in_some_view(addr) {
        s.holding(addr, bytes)
    } else {
        s.by_at.range(..=addr).next_back().map(|(_, &gva)| gva).filter(|gva| s.mirrors.get(gva).is_some_and(|m| addr + bytes <= m.at + m.len))
    };
    let Some(gva) = gva else { return };
    let Some(m) = s.mirrors.get_mut(&gva).filter(|m| m.gpu.is_none()) else { return };
    m.gpu = Some((0..m.len / PAGE).map(|i| m.live[i].then(|| Written::of(m.at + i * PAGE))).collect());
    let n = GPU_MIRRORS.fetch_add(1, Ordering::AcqRel) + 1;
    if n <= 4 || n.is_power_of_two() {
        let pages: usize = s.mirrors.values().filter_map(|m| m.gpu.as_ref()).map(|g| g.iter().flatten().count()).sum();
        crate::xlog_info!(
            "D3D12.X: {what} at {addr:#x} ({bytes} bytes) lets the GPU write a buffer in a GPU view ({gva:#x}): its pages are copied back to the title's after each submission ({n} such ranges, {} KB of pages so far)",
            pages * PAGE >> 10
        );
    }
}

/// The pages of GPU-written mirrors, `(address, bytes)` each: what a submission of the title's
/// may have written (`sparse::readback_after_execute` copies them back and calls `landed`).
pub fn written_ranges() -> Vec<(usize, usize)> {
    if GPU_MIRRORS.load(Ordering::Acquire) == 0 {
        return Vec::new();
    }
    let g = STATE.lock().unwrap();
    let Some(s) = g.as_ref() else { return Vec::new() };
    let mut out: Vec<(usize, usize)> = Vec::new();
    let most = writeback_most() / PAGE;
    for m in s.mirrors.values() {
        let Some(gpu) = m.gpu.as_ref() else { continue };
        for (i, _) in gpu.iter().enumerate().filter(|(_, w)| w.as_ref().is_some_and(Written::in_video_memory)) {
            if out.len() >= most {
                static OVER: std::sync::Once = std::sync::Once::new();
                OVER.call_once(|| crate::xlog_warn!("D3D12.X: more than {} MB of buffers in GPU views that the GPU may write – a submission copies back only that much (XCL_GPU_VIEW_WRITEBACK_MB)", most * PAGE >> 20));
                return out;
            }
            out.push((m.at + i * PAGE, PAGE));
        }
    }
    out
}

/// `len` bytes at `at` written if the memory takes them (a page the title decommits meanwhile
/// fails the call instead of faulting here).
unsafe fn write_title(at: usize, bytes: &[u8]) -> bool {
    use windows_sys::Win32::System::Diagnostics::Debug::WriteProcessMemory;
    use windows_sys::Win32::System::Threading::GetCurrentProcess;
    let mut done = 0usize;
    WriteProcessMemory(GetCurrentProcess(), at as *const c_void, bytes.as_ptr() as *const c_void, bytes.len(), &mut done) != 0 && done == bytes.len()
}

/// A copy back from video memory has landed in the CPU's view of `parts` (address, offset in
/// the copy, bytes); it was submitted as number `seq` (`next_seq`). For the pages of GPU-written
/// mirrors among them, chunk by chunk: what the GPU changed goes to where the title's page is,
/// unless the CPU wrote that chunk after the copy was submitted or has written it at the
/// source since – then the CPU's bytes stay (and are in the mirror again, for video memory).
pub(super) unsafe fn landed(parts: &[(usize, usize, usize)], seq: u64) {
    if GPU_MIRRORS.load(Ordering::Acquire) == 0 {
        return;
    }
    let mut g = STATE.lock().unwrap();
    let Some(s) = g.as_mut() else { return };
    let mut pages: Vec<(usize, usize)> = Vec::new();
    for &(va, _, len) in parts {
        let Some((&at, &gva)) = s.by_at.range(..=va).next_back() else { continue };
        let Some(m) = s.mirrors.get(&gva).filter(|m| m.gpu.is_some() && va < at + m.len) else { continue };
        pages.extend(((va - at) / PAGE..(va + len - at).div_ceil(PAGE).min(m.len / PAGE)).map(|i| (gva, i)));
    }
    pages.sort_unstable();
    pages.dedup();
    let (mut back, mut kept, mut nowhere) = (0u64, 0u64, 0u64);
    let mut now = vec![0u8; PAGE];
    for &(gva, i) in &pages {
        let Some(m) = s.mirrors.get_mut(&gva) else { continue };
        let (page_va, here) = (gva + i * PAGE, m.at + i * PAGE);
        if !m.live[i] {
            continue;
        }
        let Some(Some(w)) = m.gpu.as_mut().map(|g| &mut g[i]) else { continue };
        // video memory did not hold the page yet: what came back is not it. From the source
        // again, as when it was filled.
        if !w.in_video_memory() {
            super::title_textures::copy_gpu_view(page_va, here as *mut u8, PAGE);
            *w = Written::of(here);
            continue;
        }
        // where the title's page is now, and what it holds
        let source = super::title_textures::gpu_view_source(page_va).filter(|&a| a != 0 && a == m.sources[i]);
        if source.is_some() {
            super::title_textures::copy_gpu_view(page_va, now.as_mut_ptr(), PAGE);
        }
        let mirror = std::slice::from_raw_parts_mut(here as *mut u8, PAGE);
        let cur = SEQ.load(Ordering::Acquire);
        let mut unwritten = false;
        for c in 0..PAGE / CHUNK {
            let r = c * CHUNK..(c + 1) * CHUNK;
            let base = &mut w.base[r.clone()];
            if w.cpu_seq[c] >= seq {
                // the CPU's bytes went to the mirror after this copy was taken: they stay
                if mirror[r.clone()] != *base {
                    mirror[r].copy_from_slice(base);
                    kept += 1;
                }
                continue;
            }
            if mirror[r.clone()] == *base {
                continue;
            }
            match source {
                // the CPU wrote the chunk at the source since: its bytes, here too
                Some(_) if now[r.clone()] != *base => {
                    base.copy_from_slice(&now[r.clone()]);
                    mirror[r].copy_from_slice(base);
                    w.cpu_seq[c] = cur;
                    kept += 1;
                }
                Some(a) => {
                    if write_title(a + r.start, &mirror[r.clone()]) {
                        back += 1;
                    } else {
                        unwritten = true;
                    }
                    base.copy_from_slice(&mirror[r]);
                }
                None => {
                    unwritten = true;
                    base.copy_from_slice(&mirror[r]);
                }
            }
        }
        nowhere += unwritten as u64;
    }
    let before = BACK[0].fetch_add(pages.len() as u64, Ordering::Relaxed);
    let b = BACK[1].fetch_add(back, Ordering::Relaxed) + back;
    let k = BACK[2].fetch_add(kept, Ordering::Relaxed) + kept;
    let n = BACK[3].fetch_add(nowhere, Ordering::Relaxed) + nowhere;
    let after = before + pages.len() as u64;
    if !pages.is_empty() && (before == 0 || after.leading_zeros() != before.leading_zeros()) {
        crate::xlog_info!("D3D12.X: buffers in GPU views the GPU may write: {after} pages copied back from video memory so far; {b} chunks of 64 bytes written to the title's pages, {k} kept as the CPU wrote them, {n} pages had nowhere to go");
    }
}

/// `address` for a root descriptor (an address without a size): the address itself when it is
/// not in a GPU view.
pub(super) unsafe fn root_address(va: u64) -> u64 {
    let a = va as usize;
    if !crate::hle::kernelx::mirrors::in_some_view(a) {
        return va;
    }
    // as much of `ROOT_RANGE` as the view has from there on
    let mut bytes = ROOT_RANGE - a % PAGE;
    while bytes > PAGE && crate::hle::kernelx::mirrors::source_range(a, bytes).is_none() {
        bytes -= PAGE;
    }
    let held = STATE.lock().unwrap().as_ref().and_then(|s| s.holding(a, 1).map(|g| s.mirrors[&g].at + (a - g)));
    match held.or_else(|| address(a, bytes, "a root descriptor")) {
        Some(p) => p as u64,
        // (null rather than its own address, which is another window's memory to the shader)
        None => {
            static N: AtomicU64 = AtomicU64::new(0);
            let n = N.fetch_add(1, Ordering::Relaxed) + 1;
            if n.is_power_of_two() {
                crate::xlog_warn!("D3D12.X: a root descriptor at {va:#x} in a GPU view has no mirror – bound as null ({n} so far)");
            }
            0
        }
    }
}

/// The queue's page-mapping method changed what the GPU-view pages of `ranges` show, or their
/// physical page moved (`kernelx::mirrors::take_changed`): the mirrors' pages there are copied
/// again.
pub(super) unsafe fn pages_changed(ranges: &[(usize, usize)]) {
    if MIRRORS.load(Ordering::Acquire) == 0 || ranges.is_empty() {
        return;
    }
    let mut g = STATE.lock().unwrap();
    let Some(s) = g.as_mut() else { return };
    let mut todo: Vec<(usize, usize)> = Vec::new();
    for &(a, b) in ranges {
        for (&gva, m) in s.mirrors.range(..b) {
            let (lo, hi) = (a.max(gva), b.min(gva + m.len));
            if lo < hi {
                todo.extend(((lo - gva) / PAGE..(hi - gva).div_ceil(PAGE)).map(|i| (gva, i)));
            }
        }
    }
    todo.sort_unstable();
    todo.dedup();
    for &(gva, i) in &todo {
        s.sync_page(gva, i);
    }
    note_refreshed(1, todo.len() as u64, s);
}

/// A GPU view gained or lost its backing, or physical pages were mapped or unmapped in one
/// (`kernelx::gpu_view_map_gen`): every mirror page whose source is another now is copied
/// again, and the mirrors of a view that is gone go.
pub(super) unsafe fn views_changed() {
    if MIRRORS.load(Ordering::Acquire) == 0 {
        return;
    }
    let mut g = STATE.lock().unwrap();
    let Some(s) = g.as_mut() else { return };
    let gone: Vec<usize> = s.mirrors.keys().copied().filter(|&gva| crate::hle::kernelx::mirrors::source(gva).is_none()).collect();
    for gva in gone {
        s.drop_mirror(gva);
    }
    let mut todo: Vec<(usize, usize)> = Vec::new();
    for (&gva, m) in &s.mirrors {
        for (i, &old) in m.sources.iter().enumerate() {
            let now = super::title_textures::gpu_view_source(gva + i * PAGE).unwrap_or(0);
            if now != old {
                todo.push((gva, i));
            }
        }
    }
    for &(gva, i) in &todo {
        s.sync_page(gva, i);
    }
    note_refreshed(1, todo.len() as u64, s);
}

/// The CPU wrote the pages `dirty` (sorted, `page` bytes each) since the last submit: the
/// mirror pages copied from them are copied again. Their own pages are added to `out` – they
/// were written after this submit's look at the write watch, and go to video memory with it.
pub(super) unsafe fn written(dirty: &[usize], page: usize, out: &mut Vec<usize>) {
    if MIRRORS.load(Ordering::Acquire) == 0 || dirty.is_empty() {
        return;
    }
    let mut g = STATE.lock().unwrap();
    let Some(s) = g.as_mut() else { return };
    if s.by_source.is_empty() {
        return;
    }
    let step = page.max(0x1000);
    let mut todo: Vec<(usize, u32)> = Vec::new();
    // (the first and last source page bound the walk: most dirty pages are other memory)
    let (Some((&first, _)), Some((&last, _))) = (s.by_source.first_key_value(), s.by_source.last_key_value()) else { return };
    let from = dirty.partition_point(|&p| p + step <= first);
    for &p in &dirty[from..] {
        if p >= last + PAGE {
            break;
        }
        // the source pages that hold some of `[p, p + step)`
        for (_, v) in s.by_source.range((p + step).saturating_sub(PAGE + step - 1)..p + step) {
            todo.extend_from_slice(v);
        }
    }
    if todo.is_empty() {
        return;
    }
    todo.sort_unstable();
    todo.dedup();
    for &(gva, i) in &todo {
        if s.sync_page(gva, i as usize) {
            let here = s.mirrors[&gva].at + i as usize * PAGE;
            out.extend((here..here + PAGE).step_by(step));
        }
    }
    note_refreshed(0, todo.len() as u64, s);
}

fn note_refreshed(why: usize, pages: u64, s: &State) {
    if pages == 0 {
        return;
    }
    let before = REFRESHED[0].load(Ordering::Relaxed) + REFRESHED[1].load(Ordering::Relaxed);
    REFRESHED[why].fetch_add(pages, Ordering::Relaxed);
    let after = before + pages;
    // (at each power of two passed)
    if before == 0 || after.leading_zeros() != before.leading_zeros() {
        crate::xlog_info!(
            "D3D12.X: buffers in GPU views: {} pages copied again after CPU writes, {} after their page mappings changed; {} ranges, {} MB of pages with data; {} page copies came from memory without a write watch",
            REFRESHED[0].load(Ordering::Relaxed),
            REFRESHED[1].load(Ordering::Relaxed),
            s.mirrors.len(),
            (s.live * PAGE) >> 20,
            UNWATCHED.load(Ordering::Relaxed)
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::hle::kernelx::mirrors::{self, apply_batches};

    /// A batch of the queue's page-mapping method (`kernelx::mirrors::apply_batches`).
    #[repr(C)]
    struct PageBatch {
        dest: u64,
        ranges_n: u32,
        _pad: u32,
        ranges: usize,
    }
    fn page_batch(dest: usize, r: &[[u32; 3]]) -> PageBatch {
        PageBatch { dest: dest as u64, ranges_n: r.len() as u32, _pad: 0, ranges: r.as_ptr() as usize }
    }

    fn committed(at: usize) -> bool {
        crate::gpu::shader::readable(at, PAGE)
    }

    /// A buffer in a GPU view is read by shaders at an address in unified memory that holds the
    /// pages the queue's page-mapping method put under it – the pool pages, not the CPU view's
    /// same offset – and follows them: remapped, unmapped, written by the CPU. (Without the
    /// mirror the V# carried the GPU-view address, which is no window's memory.)
    #[test]
    fn a_buffer_in_a_gpu_view_is_read_from_the_pages_mapped_under_it() {
        let Some(u) = crate::gpu::uma::get() else { return };
        // (addresses far above anything Windows hands out, and a size no other test uses)
        let (cpu, gpu, size) = (0x71A0_0000_0000usize, 0x71A6_0000_0000usize, 0x12B * PAGE);
        mirrors::reserved(cpu, size, 0x2040_2000, 0x04);
        mirrors::reserved(gpu, size, 0x2040_2000, 0x801);
        let mut pool: Vec<u8> = (0..4 * PAGE).map(|i| (i / PAGE * 0x40 + i % 7 + 1) as u8).collect();
        let p = pool.as_ptr() as usize;
        unsafe {
            // pool pages 1 and 2 under pages 2 and 3; page 4 unmapped; page 5 never mapped
            let r = [[0, 1, 2], [2, 0, 1]];
            assert!(apply_batches(1, [page_batch(gpu + 2 * PAGE, &r)].as_ptr() as usize, p, 4).is_some());
            // a buffer from the middle of page 2 to the middle of page 5
            let (va, bytes) = (gpu + 2 * PAGE + PAGE / 2, 3 * PAGE);
            let at = address(va, bytes, "a test buffer").expect("a mirror");
            assert!(u.contains(at) && u.is_title_address(at), "{at:#x} is not the title's unified memory");
            assert_eq!(address(va + 0x100, 0x1000, "a view inside it"), Some(at + 0x100), "one mirror for the range");
            assert_eq!(root_address((va + 0x40) as u64), (at + 0x40) as u64, "a root descriptor in it");
            assert_eq!(root_address(cpu as u64), cpu as u64, "not a GPU view: as it is");
            assert_eq!(address(cpu, 16, "the CPU view"), None);
            let read = |off: usize, n: usize| std::slice::from_raw_parts((at + off) as *const u8, n).to_vec();
            assert_eq!(read(0, PAGE + PAGE / 2), &pool[PAGE + PAGE / 2..3 * PAGE], "the pool pages under it");
            let page4 = at - PAGE / 2 + 2 * PAGE;
            assert!(!committed(page4) && !committed(page4 + PAGE), "an unmapped page and an empty CPU view take no memory");

            // the CPU writes a pool page: the write watch names it at the next submit
            pool[2 * PAGE + 0x30] = 0xE7;
            let mut out = Vec::new();
            written(&[p + 2 * PAGE], 0x1000, &mut out);
            assert_eq!(read(PAGE / 2 + 0x30, 1), [0xE7], "the CPU's write reaches the mirror");
            assert_eq!(out.len(), 16, "the mirror's page goes to video memory with this submit");
            assert!(out.contains(&(at + PAGE / 2)));
            written(&[p + 3 * PAGE + 0x1000], 0x1000, &mut out);
            assert_eq!(out.len(), 16, "a pool page under no mirror");

            // the title maps pool page 3 under page 4 and unmaps page 3
            let r = [[2, 0, 1], [0, 3, 1]];
            assert!(apply_batches(1, [page_batch(gpu + 3 * PAGE, &r)].as_ptr() as usize, p, 4).is_some());
            // (not `take_changed`: a test running beside this one may take the ranges first)
            pages_changed(&[(gpu + 3 * PAGE, gpu + 5 * PAGE)]);
            assert_eq!(read(2 * PAGE - PAGE / 2, PAGE), &pool[3 * PAGE..], "the page mapped later");
            assert!(!committed(at + PAGE / 2), "the page unmapped gives its memory back");
            assert_eq!(read(0, PAGE / 2), &pool[PAGE + PAGE / 2..2 * PAGE], "the others stay");
            assert!(source_ranges().contains(&(p + 3 * PAGE, p + 4 * PAGE)) && !source_ranges().contains(&(p + 2 * PAGE, p + 3 * PAGE)));

            // the view goes: so does the mirror
            mirrors::released(cpu);
            views_changed();
            assert!(!source_ranges().iter().any(|&(a, _)| a >= p && a < p + 4 * PAGE));
            assert_eq!(address(va, bytes, "a test buffer"), None, "no GPU view any more");
            assert_eq!(root_address(va as u64), va as u64, "no view: the address as it is");
        }
    }

    /// What the GPU writes into a buffer in a GPU view goes back to the title's page, chunk by
    /// chunk – and neither direction carries the other's stale bytes: a chunk the CPU wrote
    /// after the copy back was taken, or has written at the source since, keeps the CPU's
    /// bytes; a copy of the CPU's writes into the mirror leaves the GPU's chunks alone.
    /// (`landed` is given the mirror as `sparse::copy_back` leaves it.)
    #[test]
    fn what_the_gpu_writes_into_a_gpu_view_buffer_goes_back_to_the_titles_page() {
        if crate::gpu::uma::get().is_none() {
            return;
        }
        let (cpu, gpu, size) = (0x71C0_0000_0000usize, 0x71C6_0000_0000usize, 0x12F * PAGE);
        mirrors::reserved(cpu, size, 0x2040_2000, 0x04);
        mirrors::reserved(gpu, size, 0x2040_2000, 0x801);
        let mut pool: Vec<u8> = (0..3 * PAGE).map(|i| (i % 251 + 1) as u8).collect();
        let p = pool.as_ptr() as usize;
        unsafe {
            // pool page 1 under the view's page 2, and a buffer on it
            let r = [[0, 1, 1]];
            assert!(apply_batches(1, [page_batch(gpu + 2 * PAGE, &r)].as_ptr() as usize, p, 3).is_some());
            let at = address(gpu + 2 * PAGE, PAGE, "a test buffer").expect("a mirror");
            assert!(!written_ranges().contains(&(at, PAGE)), "read only so far: nothing to copy back");
            mark_written(at, PAGE, "a test view");
            assert!(!written_ranges().contains(&(at, PAGE)), "not before video memory holds the page");
            look_done(look_begins());
            assert!(written_ranges().contains(&(at, PAGE)));
            let mirror = |off: usize, n: usize| std::slice::from_raw_parts((at + off) as *const u8, n).to_vec();
            // what the GPU's copy back puts into the mirror's CPU memory
            let gpu_wrote = |off: usize, value: u8| std::ptr::write_bytes((at + off) as *mut u8, value, CHUNK);
            let before = pool.clone();

            // 1. the GPU writes a chunk: it reaches the pool page, and only it
            let seq = next_seq();
            gpu_wrote(0x100, 0xAB);
            landed(&[(at, 0, PAGE)], seq);
            assert_eq!(&pool[PAGE + 0x100..PAGE + 0x140], &[0xAB; CHUNK], "the GPU's chunk is in the title's page");
            assert!(pool[..PAGE + 0x100] == before[..PAGE + 0x100] && pool[PAGE + 0x140..] == before[PAGE + 0x140..], "nothing else changed");

            // 2. the CPU's later write must not be clobbered: a copy back is taken, then the CPU
            // writes a chunk and it goes to the mirror, then the copy lands with the old bytes
            let seq = next_seq();
            let stale = mirror(0x200, CHUNK);
            pool[PAGE + 0x200..PAGE + 0x240].fill(0x11);
            let mut out = Vec::new();
            written(&[(p + PAGE + 0x200) & !0xFFF], 0x1000, &mut out);
            assert_eq!(mirror(0x200, CHUNK), [0x11; CHUNK]);
            assert_eq!(mirror(0x100, CHUNK), [0xAB; CHUNK], "the copy of the CPU's write leaves the GPU's chunk");
            std::ptr::copy_nonoverlapping(stale.as_ptr(), (at + 0x200) as *mut u8, CHUNK);
            landed(&[(at, 0, PAGE)], seq);
            assert_eq!(&pool[PAGE + 0x200..PAGE + 0x240], &[0x11; CHUNK], "the CPU's later write stays in the title's page");
            assert_eq!(mirror(0x200, CHUNK), [0x11; CHUNK], "and is in the mirror again");

            // 3. the CPU wrote the source and no submit has looked yet: its bytes stay too
            let seq = next_seq();
            pool[PAGE + 0x300..PAGE + 0x340].fill(0x22);
            gpu_wrote(0x300, 0x77);
            gpu_wrote(0x400, 0x55);
            landed(&[(at + 0x300, 0, 0x200)], seq);
            assert_eq!(&pool[PAGE + 0x300..PAGE + 0x340], &[0x22; CHUNK]);
            assert_eq!(mirror(0x300, CHUNK), [0x22; CHUNK]);
            assert_eq!(&pool[PAGE + 0x400..PAGE + 0x440], &[0x55; CHUNK], "the chunk only the GPU wrote goes back");

            // 4. the page is unmapped: nothing is copied back for it any more
            let r = [[2, 0, 1]];
            assert!(apply_batches(1, [page_batch(gpu + 2 * PAGE, &r)].as_ptr() as usize, p, 3).is_some());
            pages_changed(&[(gpu + 2 * PAGE, gpu + 3 * PAGE)]);
            assert!(!written_ranges().contains(&(at, PAGE)));
            mirrors::released(cpu);
            views_changed();
        }
    }
}
