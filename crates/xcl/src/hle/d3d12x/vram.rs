//! GPU-owned textures in title memory, aliased as on the console.
//!
//! A console title places render targets, depth buffers and UAV textures at addresses of its
//! own memory and reuses that memory: transient targets of a frame share it, and it creates
//! several resource objects over the same address expecting one memory (seen on XDefiant: 16
//! placements at an occupied address in its first seconds, also of other formats and sizes).
//! A committed PC resource per placement breaks that – what the GPU writes through one never
//! shows in the other. So these textures are placed in *arenas*: D3D12 default heaps (video
//! memory) standing for ranges of the title's address space (32 MiB+), at the offset of their
//! address in it. Resources at one address then share memory as on the console (the same
//! description: the same PC layout, so the same texels). The CPU never sees these textures
//! (on the console it could, but GPU-owned targets are not CPU-written).
//!
//! Arenas cost video memory for whole ranges, and a placement straddling two made a third over
//! both while the two lived on with their resources: a retail game's streaming targets, packed
//! back to back over 3.5 GB of its addresses, made 15 GB of arenas in two seconds – past the
//! card's memory and the system's commit, and the device was removed. So the first choice is
//! the console's own model (`Pages`): a 64 KiB page of video memory for each 64 KiB page of
//! the title's address a target covers, and each target a reserved (tiled) resource whose tiles
//! are mapped onto the pages of its addresses. Targets at one address share pages, a page
//! exists once however many targets cover it, and what no target covers takes nothing.
//! Arenas remain for what cannot be tiled (an address not 64 KiB aligned, a resource the
//! driver does not make reserved: D32S8, MSAA): heaps of the placement's size, shared by
//! placements inside them, let go 2 s after their last resource (256 MiB ones: 2.8 GB of D32S8).
//!
//! The pages come from pool heaps of 64 MiB. A retail game's world loading took 159 of them
//! (10.2 GB) on a 16 GB card and none was ever given back. So the pools have a budget of video
//! memory (`Budget`): past it, new pool heaps are made in system memory the GPU reads (`spilled`)
//! instead of video memory the card does not have, and a heap none of whose tiles is in use is
//! released once the GPU is past everything that could have used it (`Retiring`).
//!
//! The budget is on with `XCL_VRAM_BUDGET` (`budget_mode`; without the switch, when the title's
//! GPU heaps are in video memory, `XCL_HEAPS_VRAM_MB`: they and the pages together are more than
//! a 16 GB card has, and what Windows then moves out of video memory is not its to choose well –
//! the GPU hung). At the budget, before a page goes to system memory: CPU-filled textures no view
//! heap names give their tiles back (`evict_lru`, mapped and uploaded again when used); large
//! block-compressed textures placed then leave their top mips unmapped, their views clamped to
//! the mips they have (`trim_mips`); and pool heaps none of whose textures was used for
//! `DEMOTE_AGE` get a low residency priority, so that Windows takes those first (`demote`).
//! Nothing is evicted with `ID3D12Device::Evict`: a shader reaches a texture through the shared
//! view heap without XCL seeing it, and a page that is not resident there is a GPU page fault.
//! An unmapped tile of a reserved texture reads as zero instead (tiled resources tier 2).
//!
//! Provenance: the spilled heap's properties (a CUSTOM heap in memory pool L0 the CPU does not
//! map, `spill_properties`) and the fall back to it when video memory runs out follow
//! OpenScarlett's "L0 system-RAM spill" (https://github.com/californiaAlmonds/OpenScarlett, MIT,
//! commit 4808ad8, `DeviceX::CreatePlacedResourceX` in `src/gpu/d3d12_xs/device_x.cpp`), adapted
//! 2026-11-24 from committed resources to the pool heaps here. The budget, the release of
//! heaps and their fences are XCL's own.

use std::collections::HashSet;
use std::ffi::c_void;
use std::sync::Mutex;

use windows::core::Interface;
use windows::Win32::Graphics::Direct3D12::*;

use crate::util::HRESULT;

/// A heap of video memory for placements at title addresses `[base, base + size)`.
struct Arena<H> {
    base: usize,
    size: usize,
    /// the heap's alignment (a placement that needs more does not go in)
    align: usize,
    heap: H,
    /// resources placed in it
    users: u32,
    /// when the last of them went
    idle_since: Option<std::time::Instant>,
    id: u64,
}

/// The arenas, apart from D3D12 (`H`: the heap; numbers in the tests).
struct Arenas<H> {
    list: Vec<Arena<H>>,
    next_id: u64,
}

impl<H: Clone> Arenas<H> {
    const fn new() -> Self {
        Arenas { list: Vec::new(), next_id: 0 }
    }

    /// Arenas no resource used for `IDLE` go (their heap with the last reference).
    fn sweep(&mut self, now: std::time::Instant) {
        self.list.retain(|a| a.users > 0 || a.idle_since.is_none_or(|t| now.duration_since(t) < IDLE));
    }

    /// Where a placement of `bytes` at `va` (alignment `align`) goes: the newest arena holding
    /// it at an offset `align` allows, else a new arena of its size, `make(size, align)`.
    /// (heap, offset, arena id); None: no heap.
    fn take(&mut self, va: usize, bytes: usize, align: usize, now: std::time::Instant, make: impl FnOnce(usize, usize) -> Option<H>) -> Option<(H, usize, u64)> {
        self.sweep(now);
        let fits = |a: &Arena<H>| a.align >= align && a.base <= va && va + bytes <= a.base + a.size && (va - a.base) % align == 0;
        let i = match self.list.iter().rposition(fits) {
            Some(i) => i,
            None => {
                let size = bytes.div_ceil(align) * align;
                let heap = make(size, align)?;
                self.next_id += 1;
                self.list.push(Arena { base: va, size, align, heap, users: 0, idle_since: None, id: self.next_id });
                self.list.len() - 1
            }
        };
        let a = &mut self.list[i];
        a.users += 1;
        a.idle_since = None;
        Some((a.heap.clone(), va - a.base, a.id))
    }

    /// A resource in arena `id` released.
    fn release(&mut self, id: u64, now: std::time::Instant) {
        if let Some(a) = self.list.iter_mut().find(|a| a.id == id) {
            a.users = a.users.saturating_sub(1);
            if a.users == 0 {
                a.idle_since = Some(now);
            }
        }
    }

    fn bytes(&self) -> usize { self.list.iter().map(|a| a.size).sum() }
}

// D3D12 heaps are free-threaded
unsafe impl Send for Arenas<ID3D12Heap> {}

static ARENAS: Mutex<Arenas<ID3D12Heap>> = Mutex::new(Arenas::new());
/// resource → its arena's id
static ARENA_OF: Mutex<Option<std::collections::HashMap<usize, u64>>> = Mutex::new(None);
/// bytes of every arena made
static MADE_BYTES: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

/// (bytes of the arenas in use, bytes of all arenas made) – `XCL_ACTIVITY`
pub fn arena_bytes() -> (usize, usize) {
    (ARENAS.lock().unwrap().bytes(), MADE_BYTES.load(std::sync::atomic::Ordering::Relaxed))
}
static PLACED_TEXTURES: Mutex<Option<HashSet<usize>>> = Mutex::new(None);

/// A texture backed by one of the aliasable D3D12 heaps.
pub(super) fn is_placed_texture(resource: usize) -> bool {
    PLACED_TEXTURES.lock().unwrap().as_ref().is_some_and(|textures| textures.contains(&resource))
}

pub(super) fn forget(resource: usize) {
    if let Some(id) = SMALL_OF.lock().unwrap().as_mut().and_then(|m| m.remove(&resource)) {
        SMALL.lock().unwrap().release(id, std::time::Instant::now());
        return;
    }
    let placed = PLACED_TEXTURES.lock().unwrap().as_mut().is_some_and(|textures| textures.remove(&resource));
    if placed {
        super::sparse::target_gone(resource);
        forget_pages(resource);
        if let Some(id) = ARENA_OF.lock().unwrap().as_mut().and_then(|m| m.remove(&resource)) {
            ARENAS.lock().unwrap().release(id, std::time::Instant::now());
        }
    }
}

/// Whether a console texture with these D3D12_RESOURCE_FLAGS is the GPU's (render target,
/// depth buffer, UAV).
pub fn gpu_owned(flags: u32) -> bool {
    flags & 0x7 != 0
}

/// A new arena's heap: `size` bytes of video memory for title addresses from `va`.
unsafe fn arena_heap(dev: &ID3D12Device, va: usize, size: usize, align: usize) -> Option<ID3D12Heap> {
    let desc = D3D12_HEAP_DESC {
        SizeInBytes: size as u64,
        Properties: D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() },
        // 64 KiB, or 4 MiB for MSAA targets
        Alignment: align as u64,
        Flags: D3D12_HEAP_FLAG_NONE,
    };
    let mut heap: Option<ID3D12Heap> = None;
    if let Err(e) = dev.CreateHeap(&desc, &mut heap) {
        crate::xlog_warn!("D3D12.X: video-memory arena for {va:#x}: CreateHeap: {e}");
        return None;
    }
    let made = MADE_BYTES.fetch_add(size, std::sync::atomic::Ordering::Relaxed) + size;
    static N: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
    let n = N.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    if n < 16 || n.is_power_of_two() {
        crate::xlog_info!("D3D12.X: video-memory arena #{n} for title addresses {va:#x}..{:#x} ({made} bytes of arenas made so far)", va + size);
    }
    heap
}

/// bytes of a heap of small textures (`SmallHeaps`)
const SMALL_HEAP: usize = 16 << 20;
/// bytes of a slot of one: the alignment the PC allows a small texture
const SMALL_SLOT: usize = 0x1000;
const SMALL_SLOTS: u32 = (SMALL_HEAP / SMALL_SLOT) as u32;
/// the most a texture in one takes, on the console and on the PC: half a page
const SMALL_MAX: usize = PAGE / 2;

/// A heap of small textures: which of its slots are taken.
struct SmallHeap<H> {
    heap: H,
    /// bit i: slot i is taken or cooling
    used: Vec<u64>,
    free: u32,
    /// no slot below this one is free
    low: u32,
}

impl<H> SmallHeap<H> {
    /// The first run of `n` free slots.
    fn find(&self, n: u32) -> Option<u32> {
        let mut run = 0;
        for i in self.low..SMALL_SLOTS {
            if self.used[(i / 64) as usize] >> (i % 64) & 1 != 0 {
                run = 0;
            } else {
                run += 1;
                if run == n {
                    return Some(i + 1 - n);
                }
            }
        }
        None
    }

    fn set(&mut self, first: u32, n: u32, taken: bool) {
        for i in first..first + n {
            let (w, b) = ((i / 64) as usize, 1u64 << (i % 64));
            if taken { self.used[w] |= b } else { self.used[w] &= !b }
        }
        if taken {
            self.free -= n;
            if first == self.low {
                self.low = first + n;
            }
        } else {
            self.free += n;
            self.low = self.low.min(first);
        }
    }
}

/// Small sampled textures of the title, packed in heaps of video memory. On the pages a
/// texture takes whole 64 KiB tiles, and a retail title places thousands of textures of a few
/// KiB at addresses of their own (13,000 of 3.3 KiB on average when its world starts loading:
/// 0.8 GB of pages for 43 MB of texels). Here each takes its PC size, in 4 KiB slots. They
/// share no memory: a texture is kept in step with the title's memory on its own, like a
/// committed one. Apart from D3D12 (`H`: the heap; numbers in the tests).
struct SmallHeaps<H> {
    heaps: Vec<Option<SmallHeap<H>>>,
    /// placement id → (heap, first slot, slots)
    places: std::collections::BTreeMap<u64, (usize, u32, u32)>,
    /// slots of released textures, reusable after `COOL` (the GPU may still be at work on what
    /// the title released)
    cooling: std::collections::VecDeque<(usize, u32, u32, std::time::Instant)>,
    next_id: u64,
}

impl<H: Clone> SmallHeaps<H> {
    const fn new() -> Self {
        SmallHeaps { heaps: Vec::new(), places: std::collections::BTreeMap::new(), cooling: std::collections::VecDeque::new(), next_id: 0 }
    }

    /// The slots released `COOL` ago are free again, and a heap with nothing in it goes (one
    /// empty heap is kept for the next textures).
    fn sweep(&mut self, now: std::time::Instant) {
        while self.cooling.front().is_some_and(|c| now.saturating_duration_since(c.3) >= COOL) {
            let Some((h, first, n, _)) = self.cooling.pop_front() else { break };
            if let Some(heap) = self.heaps[h].as_mut() {
                heap.set(first, n, false);
            }
        }
        let mut kept = false;
        for h in self.heaps.iter_mut() {
            if h.as_ref().is_some_and(|x| x.free == SMALL_SLOTS) {
                if kept {
                    *h = None;
                }
                kept = true;
            }
        }
    }

    /// Slots for a texture of `bytes` (at most a heap's): in the first heap with room, else in
    /// a new one, `make()`. (heap, offset in it, placement id); None: no heap.
    fn take(&mut self, bytes: usize, now: std::time::Instant, make: impl FnOnce() -> Option<H>) -> Option<(H, usize, u64)> {
        self.sweep(now);
        let n = bytes.max(1).div_ceil(SMALL_SLOT) as u32;
        if n > SMALL_SLOTS {
            return None;
        }
        let found = self.heaps.iter().enumerate().find_map(|(h, x)| x.as_ref().filter(|x| x.free >= n).and_then(|x| x.find(n)).map(|first| (h, first)));
        let (h, first) = match found {
            Some(x) => x,
            None => {
                let heap = SmallHeap { heap: make()?, used: vec![0; SMALL_SLOTS as usize / 64], free: SMALL_SLOTS, low: 0 };
                let h = match self.heaps.iter().position(|x| x.is_none()) {
                    Some(h) => {
                        self.heaps[h] = Some(heap);
                        h
                    }
                    None => {
                        self.heaps.push(Some(heap));
                        self.heaps.len() - 1
                    }
                };
                (h, 0)
            }
        };
        let heap = self.heaps[h].as_mut()?;
        heap.set(first, n, true);
        self.next_id += 1;
        self.places.insert(self.next_id, (h, first, n));
        Some((heap.heap.clone(), first as usize * SMALL_SLOT, self.next_id))
    }

    /// The texture of placement `id` was released.
    fn release(&mut self, id: u64, now: std::time::Instant) {
        if let Some((h, first, n)) = self.places.remove(&id) {
            self.cooling.push_back((h, first, n, now));
        }
    }

    /// (textures, bytes of their slots, bytes of the heaps)
    fn counts(&self) -> (usize, usize, usize) {
        let slots: usize = self.places.values().map(|p| p.2 as usize).sum();
        (self.places.len(), slots * SMALL_SLOT, self.heaps.iter().flatten().count() * SMALL_HEAP)
    }
}

// D3D12 heaps are free-threaded
unsafe impl Send for SmallHeaps<ID3D12Heap> {}

static SMALL: Mutex<SmallHeaps<ID3D12Heap>> = Mutex::new(SmallHeaps::new());
/// resource → its placement in `SMALL`
static SMALL_OF: Mutex<Option<std::collections::HashMap<usize, u64>>> = Mutex::new(None);

/// (small textures in heaps of their own, bytes of their slots, bytes of those heaps) – `XCL_ACTIVITY`
pub fn small_counts() -> (usize, usize, usize) {
    SMALL.lock().unwrap().counts()
}

/// Whether small sampled textures go to heaps of their own (`XCL_SMALL_TEXTURE_HEAPS`; without
/// the switch, when the title's GPU heaps are in video memory: `sparse::saving`).
fn small_heaps() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| super::sparse::saving("XCL_SMALL_TEXTURE_HEAPS"))
}

/// A sampled texture of the title of at most `SMALL_MAX` bytes (`console_bytes` there, and as
/// the PC places it with the alignment of small textures), placed in a heap of small textures.
/// None: not such a texture, or the driver does not place it so.
#[allow(clippy::too_many_arguments)]
unsafe fn place_small(dev: &ID3D12Device, this: *mut c_void, desc: &D3D12_RESOURCE_DESC, state: u32, clear: *const c_void, console_bytes: Option<u64>, riid: *const windows_sys::core::GUID, ppv: *mut *mut c_void) -> Option<HRESULT> {
    if console_bytes? > SMALL_MAX as u64 || desc.SampleDesc.Count != 1 || gpu_owned(desc.Flags.0 as u32) {
        return None;
    }
    let small = D3D12_RESOURCE_DESC { Alignment: SMALL_SLOT as u64, ..*desc };
    let info = dev.GetResourceAllocationInfo(0, &[small]);
    if info.SizeInBytes == 0 || info.SizeInBytes > SMALL_MAX as u64 || info.Alignment != SMALL_SLOT as u64 {
        return None;
    }
    let now = std::time::Instant::now();
    let make = || {
        let desc = D3D12_HEAP_DESC {
            SizeInBytes: SMALL_HEAP as u64,
            Properties: D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() },
            Alignment: 0,
            Flags: D3D12_HEAP_FLAG_DENY_BUFFERS | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES,
        };
        let mut heap: Option<ID3D12Heap> = None;
        if let Err(e) = dev.CreateHeap(&desc, &mut heap) {
            crate::xlog_warn!("D3D12.X: no video memory for a heap of small title textures ({e})");
        }
        heap
    };
    let (heap, offset, id) = SMALL.lock().unwrap().take(info.SizeInBytes as usize, now, make)?;
    let hr = super::real::dev_create_placed_resource(this, heap.as_raw() as usize, offset, &small as *const _ as usize, state as usize, clear as usize, riid, ppv);
    if hr < 0 || (*ppv).is_null() {
        SMALL.lock().unwrap().release(id, now);
        return None;
    }
    SMALL_OF.lock().unwrap().get_or_insert_with(std::collections::HashMap::new).insert(*ppv as usize, id);
    Some(hr)
}

/// bytes of a page – of the title's addresses and of a tile
const PAGE: usize = 0x1_0000;
/// pages of a video-memory pool heap (64 MiB)
const POOL_PAGES: u32 = 1024;
const POOL_BYTES: u64 = POOL_PAGES as u64 * PAGE as u64;
/// how long the pools' budget of video memory is taken as used up once it was
const FULL: std::time::Duration = std::time::Duration::from_millis(250);
/// how often the pages no target uses are looked for outside a placement
const SETTLE: std::time::Duration = std::time::Duration::from_secs(1);
/// a page no target uses any more is kept this long for the next target at its address (with
/// what it holds – a title may make a target again over memory it rendered to)
const IDLE: std::time::Duration = std::time::Duration::from_secs(2);
/// a pool tile given back is not reused before this (the GPU may still be at work on what the
/// title released)
const COOL: std::time::Duration = std::time::Duration::from_secs(2);
/// Page budget (`XCL_PAGE_BUDGET_MB`): a texture the CPU fills is not evicted before this
/// long after it was placed, remapped or used in a transition.
const EVICT_MIN_AGE: std::time::Duration = std::time::Duration::from_secs(5);
/// Page budget: textures smaller than this are never evicted (UI, fonts, small lookups).
const EVICT_MIN_TILES: u32 = 4;
/// Page budget: tiles freed per eviction round once the pools are at the budget.
const EVICT_BATCH: usize = 512;

/// Page budget: pool heaps the pages may take, from `XCL_PAGE_BUDGET_MB` (0: no limit).
fn pool_cap() -> usize {
    static CAP: std::sync::OnceLock<usize> = std::sync::OnceLock::new();
    *CAP.get_or_init(|| {
        let mb: usize = std::env::var("XCL_PAGE_BUDGET_MB").ok().and_then(|v| v.trim().parse().ok()).unwrap_or(0);
        if mb == 0 {
            return 0;
        }
        let pools = (mb << 20).div_ceil(POOL_PAGES as usize * PAGE).max(1);
        crate::xlog_info!("D3D12.X: page budget {mb} MB ({pools} pools of 64 MB): CPU-filled textures are evicted least recently used first beyond it");
        pools
    })
}

/// Page budget: textures whose tiles were evicted; the next upload or transition maps
/// them again and uploads them whole.
static EVICTED: Mutex<Option<HashSet<usize>>> = Mutex::new(None);
static EVICTIONS: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
static REMAPS: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
/// Page budget: textures with no T# in any view heap (while the heaps are looked at) may be
/// evicted this long after they were placed or last seen, small ones too.
const EVICT_UNSEEN_AGE: std::time::Duration = std::time::Duration::from_secs(2);
/// whether the console view heaps are looked at (`seen`): only then does "no T#" mean unused
static HEAPS_LOOKED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
/// Page budget: after a round that gave back fewer tiles than wanted (the rest of the
/// candidates' pages are shared), the next is not tried for this long: every tile taken at the
/// budget would otherwise sort and walk the same textures again.
const EVICT_RETRY: std::time::Duration = std::time::Duration::from_secs(1);

/// A pool tile: (pool heap, tile in it).
type Tile = (u32, u32);

struct Page {
    tile: Tile,
    /// targets using the page
    users: u32,
    /// when the last of them went
    idle_since: Option<std::time::Instant>,
}

/// A page of `Pages::pages` (its title address) or of `Pages::extra_pages` (its key there).
#[derive(Clone, Copy)]
enum PageKey {
    Own(usize),
    Extra(usize, usize),
}

/// The pages of `idle` – those whose last user went, oldest first, each with when – that no
/// resource has used for `IDLE` go back to the pools: out of their map, their tile to `cooling`.
/// An entry whose page was used again since (in use now, or idle again from a later moment, with
/// a later entry) is dropped. Only the queue is looked at: every page of both maps, looked at at
/// every placement, grew with the square of what a title streams.
fn sweep_idle(
    idle: &mut std::collections::VecDeque<(PageKey, std::time::Instant)>,
    pages: &mut std::collections::HashMap<usize, Page>,
    extra_pages: &mut std::collections::HashMap<(usize, usize), Page>,
    cooling: &mut std::collections::VecDeque<(Tile, std::time::Instant)>,
    now: std::time::Instant,
) {
    while idle.front().is_some_and(|&(_, since)| now.duration_since(since) >= IDLE) {
        let Some((key, since)) = idle.pop_front() else { break };
        let still_idle = |p: &Page| p.idle_since == Some(since);
        let page = match key {
            PageKey::Own(a) if pages.get(&a).is_some_and(still_idle) => pages.remove(&a),
            PageKey::Extra(va, i) if extra_pages.get(&(va, i)).is_some_and(still_idle) => extra_pages.remove(&(va, i)),
            _ => None,
        };
        if let Some(p) = page {
            cooling.push_back((p.tile, now));
        }
    }
}

/// A pool heap: 64 MiB of video memory, or (`spilled`) of system memory the GPU reads.
struct Pool<H> {
    heap: H,
    spilled: bool,
    /// its tiles no page has
    free: Vec<u32>,
    /// has a low residency priority (`Pages::demote`)
    demoted: bool,
}

/// The pool heaps and their tiles, apart from D3D12 (`H`: the heap; numbers in the tests).
struct Pools<H> {
    /// by heap number (a tile's first half); None: released, the number is used again
    heaps: Vec<Option<Pool<H>>>,
    /// tiles given back, reusable after `COOL`
    cooling: std::collections::VecDeque<(Tile, std::time::Instant)>,
    /// heaps left without a tile in use stay (no budget: as before there was one)
    keep_empty: bool,
}

impl<H> Pools<H> {
    const fn new() -> Self {
        Pools { heaps: Vec::new(), cooling: std::collections::VecDeque::new(), keep_empty: false }
    }

    /// A free tile of video memory (`spilled`: of system memory): of the lowest heap that has
    /// one, so that the higher heaps empty and can go.
    fn take(&mut self, spilled: bool) -> Option<Tile> {
        self.heaps.iter_mut().enumerate().find_map(|(h, p)| {
            let p = p.as_mut().filter(|p| p.spilled == spilled)?;
            p.free.pop().map(|t| (h as u32, t))
        })
    }

    /// A new heap: its first tile (the others are free).
    fn add(&mut self, heap: H, spilled: bool) -> Tile {
        let pool = Some(Pool { heap, spilled, free: (1..POOL_PAGES).rev().collect(), demoted: false });
        let h = match self.heaps.iter().position(Option::is_none) {
            Some(h) => {
                self.heaps[h] = pool;
                h
            }
            None => {
                self.heaps.push(pool);
                self.heaps.len() - 1
            }
        };
        (h as u32, 0)
    }

    /// Tiles given back `COOL` ago are free again. A heap whose tiles are all free then leaves
    /// the pools – except one of video memory, kept for the next targets (a heap made and
    /// released with every transient target otherwise). The heaps that left: no page has a
    /// tile of them and none will.
    fn settle(&mut self, now: std::time::Instant) -> Vec<H> {
        let mut cooled = false;
        while self.cooling.front().is_some_and(|(_, at)| now.duration_since(*at) >= COOL) {
            let Some(((h, t), _)) = self.cooling.pop_front() else { break };
            if let Some(p) = self.heaps.get_mut(h as usize).and_then(Option::as_mut) {
                p.free.push(t);
                cooled = true;
            }
        }
        let mut gone = Vec::new();
        if cooled && !self.keep_empty {
            let mut spare = false;
            for slot in &mut self.heaps {
                let Some(p) = slot else { continue };
                if p.free.len() < POOL_PAGES as usize {
                    continue;
                }
                if !p.spilled && !spare {
                    spare = true;
                    continue;
                }
                gone.extend(slot.take().map(|p| p.heap));
            }
            while self.heaps.last().is_some_and(Option::is_none) {
                self.heaps.pop();
            }
        }
        gone
    }

    /// (heaps of video memory, heaps of system memory)
    fn count(&self) -> (usize, usize) {
        self.heaps.iter().flatten().fold((0, 0), |(v, s), p| if p.spilled { (v, s + 1) } else { (v + 1, s) })
    }

    fn free_tiles(&self) -> usize {
        self.heaps.iter().flatten().map(|p| p.free.len()).sum()
    }

    /// tiles of the heaps of system memory that are not free (in use or cooling)
    fn spilled_tiles(&self) -> usize {
        self.heaps.iter().flatten().filter(|p| p.spilled).map(|p| POOL_PAGES as usize - p.free.len()).sum()
    }
}

/// What the pool heaps may take of video memory.
enum Budget {
    /// all they need: no budget is known, or video memory is system memory (an integrated GPU)
    Unlimited,
    /// this many bytes (`XCL_VRAM_POOL_MB`)
    Fixed(u64),
    /// what the adapter's budget for the process leaves – of `BUDGET_PERCENT` of it, or of
    /// this many bytes of local video memory (`XCL_VRAM_BUDGET_MB`)
    Adapter(windows::Win32::Graphics::Dxgi::IDXGIAdapter3, Option<u64>),
}

/// Whether the pages keep to a budget of video memory (`XCL_VRAM_BUDGET`: `0` off, anything else
/// on; without the switch, on when the title's GPU heaps are in video memory –
/// `sparse::saving`). In the tests: on.
fn budget_mode() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    cfg!(test) || *ON.get_or_init(|| super::sparse::saving("XCL_VRAM_BUDGET"))
}

/// Whether textures placed at the budget leave their top mips unmapped (`XCL_VRAM_TRIM=0`: no).
fn trimming() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| !std::env::var("XCL_VRAM_TRIM").is_ok_and(|v| v.trim() == "0"))
}

/// The local video memory the process may reach with a new pool heap: `limit`
/// (`XCL_VRAM_BUDGET_MB`, at most what the adapter grants), else `BUDGET_PERCENT` of the
/// adapter's `budget`.
fn local_limit(budget: u64, limit: Option<u64>) -> u64 {
    limit.map_or(budget / 100 * BUDGET_PERCENT, |l| l.min(budget))
}

/// The share of the adapter's budget of local video memory the process may reach with a new
/// pool heap: the rest is for what is not counted when the heap is made (the driver's own, the
/// swap chain, committed textures on their way).
const BUDGET_PERCENT: u64 = 90;

/// Bytes the pool heaps may take of video memory when they have `in_video` and the adapter
/// reports `budget` for the process, `usage` of it in use (the pool heaps among it); `limit`:
/// `local_limit`.
fn adapter_cap(budget: u64, limit: Option<u64>, usage: u64, in_video: u64) -> u64 {
    in_video + local_limit(budget, limit).saturating_sub(usage)
}

impl Budget {
    unsafe fn of(dev: &ID3D12Device) -> Budget {
        use windows::Win32::Graphics::Dxgi::*;
        if let Some(mb) = std::env::var("XCL_VRAM_POOL_MB").ok().and_then(|v| v.trim().parse::<u64>().ok()) {
            return Budget::Fixed(mb << 20);
        }
        if !budget_mode() {
            return Budget::Unlimited;
        }
        let limit = std::env::var("XCL_VRAM_BUDGET_MB").ok().and_then(|v| v.trim().parse::<u64>().ok()).filter(|&mb| mb != 0).map(|mb| mb << 20);
        let mut arch = D3D12_FEATURE_DATA_ARCHITECTURE::default();
        if dev.CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &mut arch as *mut _ as *mut c_void, std::mem::size_of_val(&arch) as u32).is_ok() && arch.UMA.as_bool() {
            return Budget::Unlimited;
        }
        let adapter = CreateDXGIFactory1::<IDXGIFactory4>().and_then(|f| f.EnumAdapterByLuid::<IDXGIAdapter3>(dev.GetAdapterLuid()));
        adapter.map_or(Budget::Unlimited, |a| Budget::Adapter(a, limit))
    }

    /// Bytes the pool heaps may take of video memory, of which they have `in_video` (None: no limit).
    unsafe fn cap(&self, in_video: u64) -> Option<u64> {
        use windows::Win32::Graphics::Dxgi::*;
        match self {
            Budget::Unlimited => None,
            Budget::Fixed(bytes) => Some(*bytes),
            Budget::Adapter(adapter, limit) => {
                let mut i = DXGI_QUERY_VIDEO_MEMORY_INFO::default();
                adapter.QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mut i).ok()?;
                (i.Budget != 0).then(|| adapter_cap(i.Budget, *limit, i.CurrentUsage, in_video))
            }
        }
    }

    /// Bytes the process holds of video memory past the budget (the pool heaps `in_video`):
    /// what the pools have to give back.
    unsafe fn excess(&self, in_video: u64) -> u64 {
        use windows::Win32::Graphics::Dxgi::*;
        match self {
            Budget::Unlimited => 0,
            Budget::Fixed(bytes) => in_video.saturating_sub(*bytes),
            Budget::Adapter(adapter, limit) => {
                let mut i = DXGI_QUERY_VIDEO_MEMORY_INFO::default();
                if adapter.QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mut i).is_err() || i.Budget == 0 {
                    return 0;
                }
                i.CurrentUsage.saturating_sub(local_limit(i.Budget, *limit))
            }
        }
    }
}

/// The properties of a pool heap in system memory: memory pool L0, which a discrete GPU reads
/// and writes over the bus (shared GPU memory, not the card's), not mapped for the CPU.
/// After OpenScarlett's L0 spill (see the module's comment).
fn spill_properties() -> D3D12_HEAP_PROPERTIES {
    D3D12_HEAP_PROPERTIES {
        Type: D3D12_HEAP_TYPE_CUSTOM,
        CPUPageProperty: D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE,
        MemoryPoolPreference: D3D12_MEMORY_POOL_L0,
        ..Default::default()
    }
}

/// A pool heap none of whose tiles is in use, kept until the GPU is past everything that could
/// have used them.
struct Retiring {
    /// released with this
    _heap: ID3D12Heap,
    /// `Pages::retire_epoch` when it left the pools
    epoch: u64,
    /// `Pages::value` then: the mappings made before
    mapped: u64,
    since: std::time::Instant,
}

/// A fence of XCL's on a queue the title's work runs on, and the last `Pages::retire_epoch`
/// the queue signalled on it.
struct Mark {
    fence: ID3D12Fence,
    signalled: u64,
}

/// Whether a pool heap that left the pools at `epoch`, `waited` ago, can be released. Its tiles
/// were free then: every target on them was released or unmapped at least `COOL` before. It
/// goes when those unmappings are done on the GPU (`mapped`) and each queue (the epoch it last
/// signalled, the epoch its fence has reached) has finished the work it had at `epoch` – or,
/// for a queue that has submitted nothing since and so signalled nothing, after `COOL` more.
fn retired_can_go(epoch: u64, waited: std::time::Duration, mapped: bool, queues: impl IntoIterator<Item = (u64, u64)>) -> bool {
    mapped && queues.into_iter().all(|(signalled, reached)| if signalled >= epoch { reached >= epoch } else { waited >= COOL })
}

struct Target {
    va: usize,
    console_bytes: Option<u64>,
    total: u32,
    mapped: bool,
    used: Vec<usize>,
    used_extra: Vec<(usize, usize)>,
    /// a texture the CPU fills (its texels can be uploaded again from the title's memory)
    evictable: bool,
    /// placed as a GPU-owned target (`place`): never evictable
    gpu_owned: bool,
    /// placed, remapped, transitioned or its T# seen in a shader-visible heap last
    touched: std::time::Instant,
    /// where the last look at the view heaps found its T# (`seen`)
    seen: Seen,
    /// its top mips left unmapped (`trim_mips`): how many, and their tiles (first, count)
    trimmed: u32,
    skip: Vec<(u32, u32)>,
}

/// The tile of a texture that is left unmapped (`Target::skip`).
const NO_TILE: Tile = (u32::MAX, u32::MAX);

/// Budget: a pool heap none of whose textures was used for this long gets a low residency
/// priority.
const DEMOTE_AGE: std::time::Duration = std::time::Duration::from_secs(10);

/// Budget: whether `t` keeps its pool heaps at their normal priority at `now`. A GPU-owned
/// target is always in use for all XCL sees (it is bound without a view); a texture is while
/// its T# is in a shader-visible heap (`seen` makes it recent every look) or it was placed,
/// uploaded or named by a transition within `DEMOTE_AGE`.
fn in_use(t: &Target, now: std::time::Instant) -> bool {
    t.mapped && (t.gpu_owned || t.seen == Seen::Visible || now.saturating_duration_since(t.touched) < DEMOTE_AGE)
}

/// Budget: how many top mips a sampled texture placed at the budget leaves unmapped: two from
/// 2048 texels on its shorter side (15/16 of its memory), one from 1024 (3/4) – never a packed
/// mip (`standard`: its mips with tiles of their own), never its last. Block-compressed 2D
/// textures only: the streamed material textures, which nothing renders or copies into.
fn trim_mips(desc: &D3D12_RESOURCE_DESC, standard: u32) -> u32 {
    let bc = matches!(desc.Format.0, 70..=84 | 94..=99);
    if !bc || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 {
        return 0;
    }
    let want = match desc.Width.min(desc.Height as u64) {
        2048.. => 2,
        1024.. => 1,
        _ => 0,
    };
    want.min(standard.saturating_sub(1)).min((desc.MipLevels as u32).saturating_sub(1))
}

/// The tiles (first, count) of the mips below `trim` of a texture of `mips` mips a slice, from
/// its subresource tilings (D3D12 order: every mip of slice 0, then of slice 1 …).
fn trimmed_tiles(tilings: &[D3D12_SUBRESOURCE_TILING], mips: u32, trim: u32) -> Vec<(u32, u32)> {
    tilings
        .iter()
        .enumerate()
        .filter(|(s, t)| mips != 0 && (*s as u32 % mips) < trim && t.StartTileIndexInOverallResource != u32::MAX)
        .map(|(_, t)| (t.StartTileIndexInOverallResource, t.WidthInTiles * t.HeightInTiles as u32 * t.DepthInTiles as u32))
        .filter(|r| r.1 != 0)
        .collect()
}

/// A view of a texture whose `trim` top mips are unmapped: `view` (None: the default view of
/// `desc`) with its LOD clamped to the mips the texture has. Views of other dimensions stay.
pub(super) fn clamp_view(desc: &D3D12_RESOURCE_DESC, view: Option<D3D12_SHADER_RESOURCE_VIEW_DESC>, trim: u32) -> Option<D3D12_SHADER_RESOURCE_VIEW_DESC> {
    let mut v = match view {
        Some(v) => v,
        None if desc.DepthOrArraySize > 1 => D3D12_SHADER_RESOURCE_VIEW_DESC {
            Format: desc.Format,
            ViewDimension: D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
            Shader4ComponentMapping: D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            Anonymous: D3D12_SHADER_RESOURCE_VIEW_DESC_0 { Texture2DArray: D3D12_TEX2D_ARRAY_SRV { MipLevels: u32::MAX, ArraySize: u32::MAX, ..Default::default() } },
        },
        None => D3D12_SHADER_RESOURCE_VIEW_DESC {
            Format: desc.Format,
            ViewDimension: D3D12_SRV_DIMENSION_TEXTURE2D,
            Shader4ComponentMapping: D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            Anonymous: D3D12_SHADER_RESOURCE_VIEW_DESC_0 { Texture2D: D3D12_TEX2D_SRV { MipLevels: u32::MAX, ..Default::default() } },
        },
    };
    let lod = trim as f32;
    // SAFETY: the union member the view dimension names
    unsafe {
        let clamp = match v.ViewDimension {
            D3D12_SRV_DIMENSION_TEXTURE2D => &mut v.Anonymous.Texture2D.ResourceMinLODClamp,
            D3D12_SRV_DIMENSION_TEXTURE2DARRAY => &mut v.Anonymous.Texture2DArray.ResourceMinLODClamp,
            D3D12_SRV_DIMENSION_TEXTURECUBE => &mut v.Anonymous.TextureCube.ResourceMinLODClamp,
            D3D12_SRV_DIMENSION_TEXTURECUBEARRAY => &mut v.Anonymous.TextureCubeArray.ResourceMinLODClamp,
            _ => return Some(v),
        };
        *clamp = clamp.max(lod);
    }
    Some(v)
}

/// texture → its top mips left unmapped (`Target::trimmed`), for the views made of it
static TRIMMED: Mutex<Option<std::collections::HashMap<usize, u32>>> = Mutex::new(None);
static TRIMMED_ANY: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
/// tiles the evictions gave back, and the times a pool heap got a low priority – `XCL_ACTIVITY`
static EVICTED_TILES: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
static DEMOTIONS: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

/// How many top mips of `res` are unmapped (0: none; `clamp_view` for its views).
pub(super) fn trimmed_mips(res: usize) -> u32 {
    if !TRIMMED_ANY.load(std::sync::atomic::Ordering::Relaxed) {
        return 0;
    }
    TRIMMED.lock().unwrap().as_ref().and_then(|m| m.get(&res).copied()).unwrap_or(0)
}

/// Where a texture's T# was at the last look at the console view heaps.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Seen {
    None,
    CpuOnly,
    Visible,
}

/// The title's pages of video memory for tiled textures (see the module's comment).
struct Pages {
    queue: ID3D12CommandQueue,
    /// signalled after each mapping; title queues wait for it before their next work
    fence: ID3D12Fence,
    value: u64,
    pools: Pools<ID3D12Heap>,
    budget: Budget,
    /// when the budget last had no room for a pool heap, or none could be made: not asked
    /// again for `FULL` (the tiles of one texture come one by one)
    video_full: Option<std::time::Instant>,
    /// pool heaps that left the pools, until the GPU is past them
    retiring: Vec<Retiring>,
    /// counts the times pool heaps left the pools
    retire_epoch: u64,
    /// queue → its fence for `retiring`
    marks: std::collections::HashMap<usize, Mark>,
    /// pool heaps released since the start
    released: usize,
    /// when `sweep` last ran
    swept: std::time::Instant,
    /// when `keep_budget` last ran
    looked: std::time::Instant,
    /// at the budget: no room for another pool heap of video memory, or pages in system
    /// memory (`keep_budget`, `take_tile`)
    over: bool,
    /// title page address → its video memory
    pages: std::collections::HashMap<usize, Page>,
    /// extra / sub-64 KiB tiles keyed by `(va, tile_index)` so repeated placements at the same
    /// address share them without aliasing unrelated addresses
    extra_pages: std::collections::HashMap<(usize, usize), Page>,
    /// pages of both maps whose last user went, oldest first, with when (`sweep_idle`)
    idle: std::collections::VecDeque<(PageKey, std::time::Instant)>,
    /// resource → its mapping metadata and currently held pages / extra keys
    targets: std::collections::HashMap<usize, Target>,
    /// queue → the fence value it last waited for
    waited: std::collections::HashMap<usize, u64>,
    /// no eviction round before this (`EVICT_RETRY`)
    evict_after: Option<std::time::Instant>,
}

// D3D12 objects are free-threaded
unsafe impl Send for Pages {}

/// device raw pointer → its pages (None inside: tiled tier < 2 on this device)
static PAGES: Mutex<std::collections::BTreeMap<usize, Option<Pages>>> = Mutex::new(std::collections::BTreeMap::new());

impl Pages {
    unsafe fn new(dev: &ID3D12Device) -> Option<Pages> {
        let mut o = D3D12_FEATURE_DATA_D3D12_OPTIONS::default();
        dev.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &mut o as *mut _ as *mut c_void, std::mem::size_of_val(&o) as u32).ok()?;
        // tier 2: unmapped tiles read as zero, writes to them go nowhere
        if o.TiledResourcesTier.0 < D3D12_TILED_RESOURCES_TIER_2.0 {
            crate::xlog_info!("D3D12.X: tiled resources tier {}: GPU-owned textures in arenas", o.TiledResourcesTier.0);
            return None;
        }
        let queue: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC::default()).ok()?;
        let fence: ID3D12Fence = dev.CreateFence(0, D3D12_FENCE_FLAG_NONE).ok()?;
        crate::xlog_info!("D3D12.X: textures in title memory: tiled resources on 64 KiB pages of video memory, one per page of the title's");
        let budget = Budget::of(dev);
        match budget.cap(0) {
            Some(cap) => crate::xlog_info!(
                "D3D12.X: the pages may take {} MB of video memory now (XCL_VRAM_BUDGET); at the budget unused textures give their pages back, large ones are placed without their top mips{}, idle pool heaps get a low priority, and further pages are in system memory",
                cap >> 20,
                if trimming() { "" } else { " (off: XCL_VRAM_TRIM=0)" }
            ),
            None => crate::xlog_info!("D3D12.X: no budget of video memory for the pages"),
        }
        let mut pools = Pools::new();
        pools.keep_empty = matches!(budget, Budget::Unlimited);
        Some(Pages {
            queue,
            fence,
            value: 0,
            pools,
            budget,
            video_full: None,
            retiring: Vec::new(),
            retire_epoch: 0,
            marks: std::collections::HashMap::new(),
            released: 0,
            swept: std::time::Instant::now(),
            looked: std::time::Instant::now(),
            over: false,
            pages: std::collections::HashMap::new(),
            extra_pages: std::collections::HashMap::new(),
            idle: std::collections::VecDeque::new(),
            targets: std::collections::HashMap::new(),
            waited: std::collections::HashMap::new(),
            evict_after: None,
        })
    }

    /// Pages no resource used for `IDLE` go back to the pools, and pool heaps left with no
    /// tile in use go.
    unsafe fn sweep(&mut self, now: std::time::Instant) {
        self.swept = now;
        sweep_idle(&mut self.idle, &mut self.pages, &mut self.extra_pages, &mut self.pools.cooling, now);
        let gone = self.pools.settle(now);
        if !gone.is_empty() {
            self.retire_epoch += 1;
            let (epoch, mapped) = (self.retire_epoch, self.value);
            self.retiring.extend(gone.into_iter().map(|heap| Retiring { _heap: heap, epoch, mapped, since: now }));
        }
        self.release_retired(now);
    }

    /// The retiring heaps the GPU is past are released (`retired_can_go`).
    unsafe fn release_retired(&mut self, now: std::time::Instant) {
        if self.retiring.is_empty() {
            return;
        }
        let done = self.fence.GetCompletedValue();
        let queues: Vec<(u64, u64)> = self.marks.values().map(|m| (m.signalled, m.fence.GetCompletedValue())).collect();
        let before = self.retiring.len();
        self.retiring.retain(|r| !retired_can_go(r.epoch, now.duration_since(r.since), done >= r.mapped, queues.iter().copied()));
        self.released += before - self.retiring.len();
    }

    /// A new pool heap, of video memory or (`spilled`) of system memory.
    unsafe fn pool_heap(dev: &ID3D12Device, spilled: bool) -> windows::core::Result<ID3D12Heap> {
        let desc = D3D12_HEAP_DESC {
            SizeInBytes: POOL_BYTES,
            Properties: if spilled { spill_properties() } else { D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() } },
            Alignment: 0,
            // every kind of texture (resource heap tier 2: the only flags for both)
            Flags: D3D12_HEAP_FLAG_NONE,
        };
        let mut heap: Option<ID3D12Heap> = None;
        super::title_textures::own_work(|| dev.CreateHeap(&desc, &mut heap))?;
        heap.ok_or_else(|| windows::core::Error::from(windows::Win32::Foundation::E_FAIL))
    }

    /// A tile for a new page: a free one of video memory, a new heap of video memory while the
    /// budget has room for it, else one of system memory.
    unsafe fn take_tile(&mut self, dev: &ID3D12Device) -> Option<Tile> {
        if let Some(t) = self.pools.take(false) {
            return Some(t);
        }
        // page budget (`XCL_PAGE_BUDGET_MB`): at it, textures give their tiles back instead
        let fixed = pool_cap();
        let heaps = self.pools.count();
        if fixed > 0 && heaps.0 + heaps.1 >= fixed {
            // a round only when the last one's tiles are used up: the tiles of one texture are
            // taken one by one, and a round for each took 512 tiles more every time
            if self.pools.cooling.is_empty() {
                self.evict_lru(EVICT_BATCH);
            }
            // at the budget a tile is reused before it cooled: a draw still in flight may read
            // the newer texels there for a frame
            if let Some((t, _)) = self.pools.cooling.pop_front() {
                return Some(t);
            }
        }
        let in_video = self.pools.count().0 as u64 * POOL_BYTES;
        let (mut cap, mut refused) = (None, None);
        if !self.video_full.is_some_and(|at| at.elapsed() < FULL) {
            cap = self.budget.cap(in_video);
            if cap.is_none_or(|cap| in_video + POOL_BYTES <= cap) {
                match Self::pool_heap(dev, false) {
                    Ok(heap) => return Some(self.pools.add(heap, false)),
                    // under the budget and yet none: system memory, like past the budget
                    Err(e) => refused = Some(e),
                }
            }
            self.video_full = Some(std::time::Instant::now());
        }
        // at the budget: textures nothing uses give their tiles back before a page goes to
        // system memory (a round when the last one's tiles are used up, as above)
        if budget_mode() {
            self.over = true;
            if self.pools.cooling.is_empty() {
                self.evict_lru(EVICT_BATCH);
            }
            if let Some((t, _)) = self.pools.cooling.pop_front() {
                return Some(t);
            }
        }
        if let Some(t) = self.pools.take(true) {
            return Some(t);
        }
        match Self::pool_heap(dev, true) {
            Ok(heap) => {
                static FIRST: std::sync::Once = std::sync::Once::new();
                FIRST.call_once(|| match &refused {
                    Some(e) => crate::xlog_warn!("D3D12.X: no video memory for more pages of title textures ({e}): further ones in system memory (slower for the GPU)"),
                    None => crate::xlog_warn!("D3D12.X: the pages of title textures have their budget of video memory ({} MB of {} MB): further ones in system memory (slower for the GPU; XCL_VRAM_POOL_MB sets the budget)", in_video >> 20, cap.unwrap_or(0) >> 20),
                });
                Some(self.pools.add(heap, true))
            }
            Err(spill) => {
                if let Some(e) = refused {
                    crate::xlog_warn!("D3D12.X: no video memory for more pages of title textures ({e}), and none of system memory ({spill})");
                    return None;
                }
                // the driver makes no such heap: video memory past the budget, as without one
                crate::xlog_warn!("D3D12.X: no pool heap in system memory ({spill}): the pages of title textures take video memory past their budget");
                self.budget = Budget::Unlimited;
                match Self::pool_heap(dev, false) {
                    Ok(heap) => Some(self.pools.add(heap, false)),
                    Err(e) => {
                        crate::xlog_warn!("D3D12.X: no video memory for more pages of title textures ({e})");
                        None
                    }
                }
            }
        }
    }

    /// Map every tile of `res` (`tiles[i]`: overall tile i) – one call per pool heap.
    unsafe fn map(&mut self, res: &ID3D12Resource, tiles: &[Tile]) {
        let mut heaps: Vec<u32> = tiles.iter().filter(|&&t| t != NO_TILE).map(|t| t.0).collect();
        heaps.sort_unstable();
        heaps.dedup();
        for h in heaps {
            let ranges = heap_ranges(h, tiles);
            let flags: Vec<D3D12_TILE_RANGE_FLAGS> = ranges.iter().map(|r| if r.0 { D3D12_TILE_RANGE_FLAG_NONE } else { D3D12_TILE_RANGE_FLAG_SKIP }).collect();
            let offsets: Vec<u32> = ranges.iter().map(|r| r.1).collect();
            let counts: Vec<u32> = ranges.iter().map(|r| r.2).collect();
            let coord = D3D12_TILED_RESOURCE_COORDINATE::default();
            let region = D3D12_TILE_REGION_SIZE { NumTiles: tiles.len() as u32, UseBox: false.into(), ..Default::default() };
            // (a tile in use is of a heap in the pools)
            let Some(pool) = self.pools.heaps.get(h as usize).and_then(Option::as_ref) else { continue };
            self.queue.UpdateTileMappings(
                res,
                1,
                Some(&coord),
                Some(&region),
                &pool.heap,
                ranges.len() as u32,
                Some(flags.as_ptr()),
                Some(offsets.as_ptr()),
                Some(counts.as_ptr()),
                D3D12_TILE_MAPPING_FLAG_NONE,
            );
        }
        self.value += 1;
        let _ = self.queue.Signal(&self.fence, self.value);
    }

    /// Unmap every tile of `res` (`D3D12_TILE_RANGE_FLAG_NULL`) so its unshared tiles can return
    /// to the pool even while the title keeps the COM object alive.
    unsafe fn unmap(&mut self, res: &ID3D12Resource, total: u32) {
        let coord = D3D12_TILED_RESOURCE_COORDINATE::default();
        let region = D3D12_TILE_REGION_SIZE { NumTiles: total, UseBox: false.into(), ..Default::default() };
        let flag = D3D12_TILE_RANGE_FLAG_NULL;
        self.queue.UpdateTileMappings(
            res,
            1,
            Some(&coord),
            Some(&region),
            None,
            1,
            Some(&flag),
            None,
            Some(&total),
            D3D12_TILE_MAPPING_FLAG_NONE,
        );
        self.value += 1;
        let _ = self.queue.Signal(&self.fence, self.value);
    }

    fn release_tiles(&mut self, used: Vec<usize>, used_extra: Vec<(usize, usize)>, now: std::time::Instant, immediate_cool: bool) {
        for a in used {
            if let Some(p) = self.pages.get_mut(&a) {
                p.users = p.users.saturating_sub(1);
                if p.users == 0 {
                    if immediate_cool {
                        if let Some(p) = self.pages.remove(&a) {
                            self.pools.cooling.push_back((p.tile, now));
                        }
                    } else {
                        p.idle_since = Some(now);
                        self.idle.push_back((PageKey::Own(a), now));
                    }
                }
            }
        }
        for k in used_extra {
            if let Some(p) = self.extra_pages.get_mut(&k) {
                p.users = p.users.saturating_sub(1);
                if p.users == 0 {
                    if immediate_cool {
                        if let Some(p) = self.extra_pages.remove(&k) {
                            self.pools.cooling.push_back((p.tile, now));
                        }
                    } else {
                        p.idle_since = Some(now);
                        self.idle.push_back((PageKey::Extra(k.0, k.1), now));
                    }
                }
            }
        }
    }

    /// Page budget: unmap CPU-filled textures, least recently touched first, until `want`
    /// tiles came back (pages other mapped textures still share stay). Each goes to `EVICTED`.
    /// After a round that fell short, none for `EVICT_RETRY`.
    unsafe fn evict_lru(&mut self, want: usize) {
        let now = std::time::Instant::now();
        if self.evict_after.is_some_and(|t| now < t) {
            return;
        }
        let cooling = self.pools.cooling.len();
        // with the view heaps looked at, a texture whose T# is in none cannot be sampled: it
        // goes first, small or not, once `EVICT_UNSEEN_AGE` old
        let heaps = HEAPS_LOOKED.load(std::sync::atomic::Ordering::Relaxed);
        let mut order: Vec<(bool, std::time::Instant, usize)> = self
            .targets
            .iter()
            .filter_map(|(&k, t)| {
                if !t.evictable || !t.mapped {
                    return None;
                }
                let unseen = heaps && t.seen == Seen::None;
                let age = now.duration_since(t.touched);
                let ok = if unseen { age >= EVICT_UNSEEN_AGE } else { t.total >= EVICT_MIN_TILES && age >= EVICT_MIN_AGE };
                ok.then_some((!unseen, t.touched, k))
            })
            .collect();
        order.sort_unstable();
        let mut evicted = Vec::new();
        let mut shared = 0u64;
        for (_, _, key) in order {
            if self.pools.cooling.len() - cooling >= want {
                break;
            }
            let Some(t) = self.targets.get(&key) else { continue };
            // a texture whose pages all are other mapped textures' too gives nothing back
            let alone = t.used.iter().any(|a| self.pages.get(a).is_some_and(|p| p.users <= 1))
                || t.used_extra.iter().any(|k| self.extra_pages.get(k).is_some_and(|p| p.users <= 1));
            if !alone {
                shared += 1;
                continue;
            }
            let Some(t) = self.targets.get_mut(&key) else { continue };
            let total = t.total;
            let used = std::mem::take(&mut t.used);
            let used_extra = std::mem::take(&mut t.used_extra);
            t.mapped = false;
            let raw = key as *mut c_void;
            if let Some(res) = ID3D12Resource::from_raw_borrowed(&raw) {
                self.unmap(res, total);
            }
            self.release_tiles(used, used_extra, now, true);
            evicted.push(key);
        }
        let freed = self.pools.cooling.len() - cooling;
        self.evict_after = (freed < want).then(|| now + EVICT_RETRY);
        if !evicted.is_empty() {
            let n = EVICTIONS.fetch_add(evicted.len() as u64, std::sync::atomic::Ordering::Relaxed) + evicted.len() as u64;
            static LOGGED: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
            let step = LOGGED.load(std::sync::atomic::Ordering::Relaxed);
            if n >= step {
                LOGGED.store(n.max(1) * 2, std::sync::atomic::Ordering::Relaxed);
                crate::xlog_info!("D3D12.X: page budget: {} textures evicted ({} MB back) this round, {n} so far; {shared} passed over, all their pages shared", evicted.len(), (freed * PAGE) >> 20);
            }
            EVICTED_TILES.fetch_add(freed as u64, std::sync::atomic::Ordering::Relaxed);
            EVICTED.lock().unwrap().get_or_insert_with(HashSet::new).extend(evicted);
        }
    }

    /// Acquire/share all `total` tiles for `res` at `va` and map them on the `Pages` queue.
    /// The tiles in `skip` (first, count) stay unmapped and take no page.
    unsafe fn bind(&mut self, dev: &ID3D12Device, res: &ID3D12Resource, va: usize, console_bytes: Option<u64>, total: u32, skip: &[(u32, u32)]) -> Option<(Vec<usize>, Vec<(usize, usize)>)> {
        let (first, own) = (total > 0).then(|| owned_pages(va, console_bytes, total as usize)).flatten()?;
        let now = std::time::Instant::now();
        self.sweep(now);
        let mut tiles = Vec::with_capacity(total as usize);
        let mut used = Vec::with_capacity(own);
        let mut used_extra = Vec::new();
        for i in 0..total as usize {
            if skip.iter().any(|&(s, n)| (s..s.saturating_add(n)).contains(&(i as u32))) {
                tiles.push(NO_TILE);
                continue;
            }
            let tile = if i < own {
                let a = first + i * PAGE;
                let tile = match self.pages.get_mut(&a) {
                    Some(p) => {
                        p.users += 1;
                        p.idle_since = None;
                        Some(p.tile)
                    }
                    None => self.take_tile(dev).inspect(|&t| {
                        self.pages.insert(a, Page { tile: t, users: 1, idle_since: None });
                    }),
                };
                if tile.is_some() {
                    used.push(a);
                }
                tile
            } else {
                let k = (va, i);
                let tile = match self.extra_pages.get_mut(&k) {
                    Some(p) => {
                        p.users += 1;
                        p.idle_since = None;
                        Some(p.tile)
                    }
                    None => self.take_tile(dev).inspect(|&t| {
                        self.extra_pages.insert(k, Page { tile: t, users: 1, idle_since: None });
                    }),
                };
                if tile.is_some() {
                    used_extra.push(k);
                }
                tile
            };
            let Some(t) = tile else { break };
            tiles.push(t);
        }
        if tiles.len() < total as usize {
            self.release_tiles(used, used_extra, now, true);
            return None;
        }
        self.map(res, &tiles);
        Some((used, used_extra))
    }

    /// Budget, about once a second before a submission: whether the pools are at it (`over`);
    /// past it – what else the process holds in video memory grew – textures nothing uses give
    /// their tiles back, so that their pool heaps empty and go; and the priorities (`demote`).
    unsafe fn keep_budget(&mut self, dev: &ID3D12Device, now: std::time::Instant) {
        self.looked = now;
        let heaps = self.pools.count();
        let in_video = heaps.0 as u64 * POOL_BYTES;
        let Some(cap) = self.budget.cap(in_video) else {
            self.over = false;
            return;
        };
        self.over = heaps.1 > 0 || in_video + POOL_BYTES > cap;
        let excess = self.budget.excess(in_video);
        if excess > 0 {
            self.evict_lru(((excess / PAGE as u64) as usize).clamp(1, 4 * EVICT_BATCH));
        }
        self.demote(dev, now);
    }

    /// The pool heaps (their numbers) with a tile of a texture in use at `now` (`in_use`).
    fn heaps_in_use(&self, now: std::time::Instant) -> HashSet<u32> {
        let mut hot = HashSet::new();
        for t in self.targets.values().filter(|t| in_use(t, now)) {
            hot.extend(t.used.iter().filter_map(|a| self.pages.get(a)).map(|p| p.tile.0));
            hot.extend(t.used_extra.iter().filter_map(|k| self.extra_pages.get(k)).map(|p| p.tile.0));
        }
        hot
    }

    /// Budget: at it, a pool heap with tiles in use none of whose textures is in use
    /// (`heaps_in_use`) gets a low residency priority – what Windows moves out of video memory
    /// first when it is short – and its normal one back when one is, or the pools are under
    /// the budget again. Only while the view heaps are looked at: without them no texture is
    /// ever seen in use. (Not `Evict`: see the module's comment.)
    unsafe fn demote(&mut self, dev: &ID3D12Device, now: std::time::Instant) {
        if !self.over && !self.pools.heaps.iter().flatten().any(|p| p.demoted) {
            return;
        }
        let looked = HEAPS_LOOKED.load(std::sync::atomic::Ordering::Relaxed);
        let hot = self.heaps_in_use(now);
        let Ok(dev1) = dev.cast::<ID3D12Device1>() else { return };
        for (h, p) in self.pools.heaps.iter_mut().enumerate() {
            let Some(p) = p else { continue };
            let cold = self.over && looked && p.free.len() < POOL_PAGES as usize && !hot.contains(&(h as u32));
            if cold == p.demoted {
                continue;
            }
            let Ok(pageable) = p.heap.cast::<ID3D12Pageable>() else { continue };
            let priority = if cold { D3D12_RESIDENCY_PRIORITY_LOW } else { D3D12_RESIDENCY_PRIORITY_NORMAL };
            if dev1.SetResidencyPriority(1, &Some(pageable), &priority).is_ok() {
                p.demoted = cold;
                if cold {
                    DEMOTIONS.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                }
            }
        }
    }
}

/// The ranges of one UpdateTileMappings call for pool heap `heap` over a resource whose overall
/// tile i goes to `tiles[i]`: (mapped, tile offset in the heap, count) – runs of consecutive
/// tiles of the heap, and skipped runs for tiles of other heaps.
fn heap_ranges(heap: u32, tiles: &[Tile]) -> Vec<(bool, u32, u32)> {
    let mut out: Vec<(bool, u32, u32)> = Vec::new();
    for &(h, t) in tiles {
        let mine = h == heap;
        match out.last_mut() {
            Some(r) if mine && r.0 && r.1 + r.2 == t => r.2 += 1,
            Some(r) if !mine && !r.0 => r.2 += 1,
            _ => out.push((mine, if mine { t } else { 0 }, 1)),
        }
    }
    out
}

/// 64 KiB page boundaries `P` with `va <= P < va + bytes`, capped at `total` tiles:
/// `(first_page, count)`. Because every `P` lies strictly inside `[va, va + bytes)`, two
/// non-overlapping ranges never share a page here even when `va` is only 4 KiB aligned.
fn owned_pages(va: usize, console_bytes: Option<u64>, total: usize) -> Option<(usize, usize)> {
    match console_bytes {
        Some(b) => {
            let bytes = usize::try_from(b).ok()?;
            let first = va.checked_next_multiple_of(PAGE)?;
            let end = va.saturating_add(bytes);
            let n = if first < end { (end - first).div_ceil(PAGE) } else { 0 };
            Some((first, n.min(total)))
        }
        None => (va % PAGE == 0).then_some((va, total)),
    }
}

/// A reserved texture needs TILED in the format's support (`support2`) and, multisampled, a
/// tiled quality level at its sample count.
fn can_reserve(support2: i32, samples: u32, tiled_msaa_levels: u32) -> bool {
    support2 & D3D12_FORMAT_SUPPORT2_TILED.0 != 0 && (samples <= 1 || tiled_msaa_levels > 0)
}

/// Whether the device can make `desc` a reserved texture, asked before CreateReservedResource:
/// refused, it is an error in the debug layer (#940: R32G8X24_TYPELESS depth, 1 and 4 samples).
/// A query that fails says yes, and creation decides.
unsafe fn reservable(dev: &ID3D12Device, desc: &D3D12_RESOURCE_DESC) -> bool {
    let mut f = D3D12_FEATURE_DATA_FORMAT_SUPPORT { Format: desc.Format, ..Default::default() };
    if dev.CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &mut f as *mut _ as *mut c_void, std::mem::size_of_val(&f) as u32).is_err() {
        return true;
    }
    // a format without TILED is not reservable whatever the sample count's query says
    if !can_reserve(f.Support2.0, 1, 0) {
        return false;
    }
    let samples = desc.SampleDesc.Count;
    let mut m = D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS { Format: desc.Format, SampleCount: samples, Flags: D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE, NumQualityLevels: 0 };
    if samples > 1 && dev.CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &mut m as *mut _ as *mut c_void, std::mem::size_of_val(&m) as u32).is_err() {
        return true;
    }
    can_reserve(f.Support2.0, samples, m.NumQualityLevels)
}

/// A texture at `va` as a reserved resource on the title's shared 64 KiB pages
/// (`console_bytes`: its size on the console; tiles past the owned page boundaries are shared by
/// `(va, tile_index)` so repeated placements at the same address do not leak). None: it cannot be.
#[allow(clippy::too_many_arguments)]
unsafe fn place_on_pages(
    dev: &ID3D12Device,
    this: *mut c_void,
    va: usize,
    desc: &D3D12_RESOURCE_DESC,
    state: u32,
    clear: *const c_void,
    console_bytes: Option<u64>,
    require_aligned: bool,
    riid: *const windows_sys::core::GUID,
    ppv: *mut *mut c_void,
) -> Option<HRESULT> {
    if desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN || desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D {
        return None;
    }
    if (require_aligned || console_bytes.is_none()) && va % PAGE != 0 {
        return None;
    }
    // reserved textures have the PC's 64 KiB tiles
    let tiled = D3D12_RESOURCE_DESC { Layout: D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, ..*desc };
    static N: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
    let log = |why: &str| {
        if require_aligned && N.fetch_add(1, std::sync::atomic::Ordering::Relaxed) < 4 {
            crate::xlog_info!("D3D12.X: GPU-owned texture at {va:#x} ({}x{}, format {}, {} samples) not reserved ({why}): in an arena", desc.Width, desc.Height, desc.Format.0, desc.SampleDesc.Count);
        }
    };
    if !reservable(dev, &tiled) {
        log("no tiled support for its format or sample count");
        return None;
    }
    let mut g = PAGES.lock().unwrap();
    let pages = g.entry(dev.as_raw() as usize).or_insert_with(|| Pages::new(dev)).as_mut()?;
    let hr = super::real::dev_create_reserved_resource(this, &tiled as *const _ as usize, state as usize, clear as usize, riid, ppv);
    if hr < 0 || (*ppv).is_null() {
        log(&format!("{hr:#x}"));
        return None;
    }
    let raw = *ppv;
    let res = ID3D12Resource::from_raw_borrowed(&raw)?;
    let mut total = 0u32;
    dev.GetResourceTiling(res, Some(&mut total), None, None, None, 0, std::ptr::null_mut());
    // at the budget a large sampled texture is placed without its top mips
    let (mut trimmed, mut skip) = (0, Vec::new());
    if !require_aligned && pages.over && trimming() {
        let made = res.GetDesc();
        let mut subs = made.MipLevels as u32 * made.DepthOrArraySize as u32;
        let mut tilings = vec![D3D12_SUBRESOURCE_TILING::default(); subs as usize];
        let mut packed = D3D12_PACKED_MIP_INFO::default();
        dev.GetResourceTiling(res, None, Some(&mut packed), None, Some(&mut subs), 0, tilings.as_mut_ptr());
        trimmed = trim_mips(&made, packed.NumStandardMips as u32);
        skip = trimmed_tiles(&tilings[..(subs as usize).min(tilings.len())], made.MipLevels as u32, trimmed);
        if skip.is_empty() {
            trimmed = 0;
        }
    }
    let Some((used, used_extra)) = pages.bind(dev, res, va, console_bytes, total, &skip) else {
        drop(g);
        drop(windows::core::IUnknown::from_raw(raw));
        *ppv = std::ptr::null_mut();
        return None;
    };
    if trimmed > 0 {
        TRIMMED.lock().unwrap().get_or_insert_with(Default::default).insert(raw as usize, trimmed);
        TRIMMED_ANY.store(true, std::sync::atomic::Ordering::Relaxed);
    }
    pages.targets.insert(raw as usize, Target { va, console_bytes, total, mapped: true, used, used_extra, evictable: false, gpu_owned: require_aligned, touched: std::time::Instant::now(), seen: Seen::None, trimmed, skip });
    drop(g);
    PLACED_TEXTURES.lock().unwrap().get_or_insert_with(HashSet::new).insert(raw as usize);
    Some(hr)
}

/// Unmap a placed texture superseded by a placement with a different address range, returning its
/// unshared tiles (`extra_pages` and unshared tail pages) to the cooling pool immediately.
pub(super) unsafe fn unmap_superseded(res: &ID3D12Resource) {
    let key = res.as_raw() as usize;
    let mut g = PAGES.lock().unwrap();
    let now = std::time::Instant::now();
    for pages in g.values_mut().flatten() {
        let Some(t) = pages.targets.get_mut(&key) else { continue };
        if !t.mapped {
            return;
        }
        let total = t.total;
        let used = std::mem::take(&mut t.used);
        let used_extra = std::mem::take(&mut t.used_extra);
        t.mapped = false;
        pages.unmap(res, total);
        pages.release_tiles(used, used_extra, now, true);
        return;
    }
}

/// Re-map a previously unmapped placed texture when the newer placement shadowing it is released.
pub(super) unsafe fn remap_if_unmapped(res: &ID3D12Resource) -> bool {
    let mut dev: Option<ID3D12Device> = None;
    if res.GetDevice(&mut dev).is_err() {
        return false;
    }
    let Some(dev) = dev else { return false };
    let key = res.as_raw() as usize;
    let mut g = PAGES.lock().unwrap();
    let Some(Some(pages)) = g.get_mut(&(dev.as_raw() as usize)) else { return false };
    let Some((va, console_bytes, total, mapped, skip)) = pages.targets.get(&key).map(|t| (t.va, t.console_bytes, t.total, t.mapped, t.skip.clone())) else {
        return false;
    };
    if mapped {
        return true;
    }
    let Some((used, used_extra)) = pages.bind(&dev, res, va, console_bytes, total, &skip) else {
        return false;
    };
    if let Some(t) = pages.targets.get_mut(&key) {
        t.used = used;
        t.used_extra = used_extra;
        t.mapped = true;
        t.touched = std::time::Instant::now();
    }
    true
}

/// Page budget: `res`, taken from the evicted, could not be mapped again: evicted still.
pub(super) fn evicted_again(res: usize) {
    if is_placed_texture(res) {
        EVICTED.lock().unwrap().get_or_insert_with(HashSet::new).insert(res);
        REMAPS.fetch_sub(1, std::sync::atomic::Ordering::Relaxed);
    }
}

/// Page budget: `res` was evicted by the page budget; true once, and the caller maps it
/// again (`remap_if_unmapped`) and uploads it whole.
pub(super) fn take_evicted(res: usize) -> bool {
    let mut g = EVICTED.lock().unwrap();
    let hit = g.as_mut().is_some_and(|s| s.remove(&res));
    if hit {
        REMAPS.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    }
    hit
}

/// Page budget: whether any texture is evicted (callers skip the lookup otherwise).
pub(super) fn any_evicted() -> bool {
    EVICTED.lock().unwrap().as_ref().is_some_and(|s| !s.is_empty())
}

/// Whether the page budget follows the use of textures (always in the tests).
pub(super) fn tracking() -> bool {
    cfg!(test) || pool_cap() != 0 || budget_mode()
}

/// Page budget: the T# of the textures `visible` is in a shader-visible view heap (they may be
/// sampled: recent now), that of `cpu_only` only in CPU-only heaps. Returns the evicted ones of
/// `visible`, which the caller maps again and uploads before the title's next work.
pub(super) fn seen(visible: &[usize], cpu_only: &[usize]) -> Vec<usize> {
    let now = std::time::Instant::now();
    HEAPS_LOOKED.store(true, std::sync::atomic::Ordering::Relaxed);
    let mut g = PAGES.lock().unwrap();
    for pages in g.values_mut().flatten() {
        for t in pages.targets.values_mut() {
            t.seen = Seen::None;
        }
        for r in visible {
            if let Some(t) = pages.targets.get_mut(r) {
                t.touched = now;
                t.seen = Seen::Visible;
            }
        }
        for r in cpu_only {
            if let Some(t) = pages.targets.get_mut(r) {
                t.seen = Seen::CpuOnly;
            }
        }
    }
    drop(g);
    let mut e = EVICTED.lock().unwrap();
    let Some(e) = e.as_mut().filter(|s| !s.is_empty()) else { return Vec::new() };
    let back: Vec<usize> = visible.iter().copied().filter(|r| e.remove(r)).collect();
    REMAPS.fetch_add(back.len() as u64, std::sync::atomic::Ordering::Relaxed);
    back
}

/// Page budget: the textures `used` were used (a transition the title submitted, an upload of
/// what the CPU wrote, a view made): later to be evicted.
pub(super) fn touch(used: &[usize]) {
    if !tracking() || used.is_empty() {
        return;
    }
    let now = std::time::Instant::now();
    let mut g = PAGES.lock().unwrap();
    for pages in g.values_mut().flatten() {
        for res in used {
            if let Some(t) = pages.targets.get_mut(res) {
                t.touched = now;
            }
        }
    }
}

/// Page budget: `res` is kept in step with the title's memory (`title_textures::register` took
/// it): its texels can be uploaded again, so it may be evicted. A texture on the pages that is
/// not kept in step never is: nothing would bring it back.
pub(super) fn filled_from_memory(res: usize) {
    if !tracking() {
        return;
    }
    let mut g = PAGES.lock().unwrap();
    for pages in g.values_mut().flatten() {
        if let Some(t) = pages.targets.get_mut(&res) {
            t.evictable = !t.gpu_owned;
            return;
        }
    }
}

/// Page budget: the GPU wrote `res`; its texels are not in the title's memory, so it is
/// never evicted.
pub(super) fn pin(res: usize) {
    if !tracking() {
        return;
    }
    let mut g = PAGES.lock().unwrap();
    for pages in g.values_mut().flatten() {
        if let Some(t) = pages.targets.get_mut(&res) {
            t.evictable = false;
            return;
        }
    }
}

/// Page budget: (textures evicted, textures mapped again) so far.
pub fn eviction_counts() -> (u64, u64) {
    (EVICTIONS.load(std::sync::atomic::Ordering::Relaxed), REMAPS.load(std::sync::atomic::Ordering::Relaxed))
}

/// A resource released: its pages are no longer its.
fn forget_pages(resource: usize) {
    let mut g = PAGES.lock().unwrap();
    let now = std::time::Instant::now();
    for pages in g.values_mut().flatten() {
        let Some(t) = pages.targets.remove(&resource) else { continue };
        if t.mapped {
            pages.release_tiles(t.used, t.used_extra, now, false);
        }
        drop(g);
        if let Some(s) = EVICTED.lock().unwrap().as_mut() {
            s.remove(&resource);
        }
        if t.trimmed > 0 {
            if let Some(m) = TRIMMED.lock().unwrap().as_mut() {
                m.remove(&resource);
            }
        }
        return;
    }
}

/// Before GPU work on `queue`: it waits for the pages mapped since its last work on that device.
pub(super) unsafe fn before_execute(queue: &ID3D12CommandQueue) {
    let mut dev: Option<ID3D12Device> = None;
    if queue.GetDevice(&mut dev).is_err() {
        return;
    }
    let Some(dev) = dev else { return };
    let mut g = PAGES.lock().unwrap();
    let Some(Some(pages)) = g.get_mut(&(dev.as_raw() as usize)) else { return };
    let key = queue.as_raw() as usize;
    if pages.waited.get(&key).copied().unwrap_or(0) < pages.value {
        let _ = queue.Wait(&pages.fence, pages.value);
        pages.waited.insert(key, pages.value);
    }
    // pool heaps that left the pools wait for the work the queue has so far
    if !pages.marks.contains_key(&key) {
        if let Ok(fence) = dev.CreateFence::<ID3D12Fence>(0, D3D12_FENCE_FLAG_NONE) {
            pages.marks.insert(key, Mark { fence, signalled: 0 });
        }
    }
    let epoch = pages.retire_epoch;
    if let Some(m) = pages.marks.get_mut(&key) {
        if m.signalled < epoch && queue.Signal(&m.fence, epoch).is_ok() {
            m.signalled = epoch;
        }
    }
    let now = std::time::Instant::now();
    if now.duration_since(pages.swept) >= SETTLE {
        pages.sweep(now);
    } else {
        pages.release_retired(now);
    }
    if budget_mode() && now.duration_since(pages.looked) >= SETTLE {
        pages.keep_budget(&dev, now);
    }
}

/// The pages' fences (tile mappings of the title's textures), for the log of a GPU wait that
/// timed out: completed / signalled, and the value each queue last waited for. A lock another
/// thread holds is not waited for. `name` names a queue.
pub(super) fn fence_summary(name: &dyn Fn(usize) -> String) -> String {
    let Ok(g) = PAGES.try_lock() else { return "pages busy".into() };
    let v: Vec<String> = g
        .values()
        .flatten()
        .map(|p| {
            let mut waited: Vec<String> = p.waited.iter().map(|(&q, &v)| format!("{}>={v}", name(q))).collect();
            waited.sort();
            format!("pages {}/{} (queues wait for {})", unsafe { p.fence.GetCompletedValue() }, p.value, waited.join(" "))
        })
        .collect();
    if v.is_empty() {
        "no pages".into()
    } else {
        v.join(", ")
    }
}

#[cfg(test)]
pub(super) static VRAM_TEST_LOCK: Mutex<()> = Mutex::new(());

#[cfg(test)]
pub(super) fn page_bytes_for(device: usize) -> (usize, usize) {
    let g = PAGES.lock().unwrap();
    let Some(Some(p)) = g.get(&device) else { return (0, 0) };
    let heaps = p.pools.count();
    ((p.pages.len() + p.extra_pages.len()) * PAGE, (heaps.0 + heaps.1) * POOL_BYTES as usize)
}

/// (tiled textures on pages, pool heaps, free tiles, cooling tiles) – `XCL_ACTIVITY`
pub fn page_counts() -> [usize; 4] {
    let g = PAGES.lock().unwrap();
    g.values().flatten().fold([0; 4], |a, p| {
        let heaps = p.pools.count();
        [a[0] + p.targets.len(), a[1] + heaps.0 + heaps.1, a[2] + p.pools.free_tiles(), a[3] + p.pools.cooling.len()]
    })
}

/// The pools' budget – `XCL_ACTIVITY`: bytes they may take of video memory now (None: no
/// limit), and [pool heaps in system memory, their tiles not free, pool heaps released, pool
/// heaps waiting for the GPU before their release].
pub fn page_budget() -> (Option<u64>, [usize; 4]) {
    let g = PAGES.lock().unwrap();
    g.values().flatten().fold((None, [0; 4]), |(cap, a), p| {
        let heaps = p.pools.count();
        let own = unsafe { p.budget.cap(heaps.0 as u64 * POOL_BYTES) };
        (own.map(|c| c + cap.unwrap_or(0)).or(cap), [a[0] + heaps.1, a[1] + p.pools.spilled_tiles(), a[2] + p.released, a[3] + p.retiring.len()])
    })
}

/// What the budget did – `XCL_ACTIVITY`: [textures evicted so far, bytes their tiles gave back,
/// textures mapped again, textures on the pages without their top mips now, bytes of the tiles
/// those leave unmapped, pool heaps with a low priority now, times a pool heap got one,
/// 1 when the pools are at their budget].
pub fn budget_counts() -> [u64; 8] {
    use std::sync::atomic::Ordering::Relaxed;
    let g = PAGES.lock().unwrap();
    let mut c = [EVICTIONS.load(Relaxed), EVICTED_TILES.load(Relaxed) * PAGE as u64, REMAPS.load(Relaxed), 0, 0, 0, DEMOTIONS.load(Relaxed), 0];
    for p in g.values().flatten() {
        for t in p.targets.values().filter(|t| t.trimmed > 0) {
            c[3] += 1;
            c[4] += t.skip.iter().map(|r| r.1 as u64).sum::<u64>() * PAGE as u64;
        }
        c[5] += p.pools.heaps.iter().flatten().filter(|h| h.demoted).count() as u64;
        c[7] |= p.over as u64;
    }
    c
}

/// (bytes of pages in use by tiled textures, bytes of pool heaps) – `XCL_ACTIVITY`
pub fn page_bytes() -> (usize, usize) {
    let g = PAGES.lock().unwrap();
    g.values().flatten().fold((0, 0), |(used, pools), p| {
        let heaps = p.pools.count();
        (used + (p.pages.len() + p.extra_pages.len()) * PAGE, pools + (heaps.0 + heaps.1) * POOL_BYTES as usize)
    })
}

/// (bytes of pages at the title's addresses, bytes of the extra tiles of textures whose PC
/// layout takes more tiles than their addresses have pages) – `XCL_ACTIVITY`
pub fn page_kinds() -> (usize, usize) {
    let g = PAGES.lock().unwrap();
    g.values().flatten().fold((0, 0), |(own, extra), p| (own + p.pages.len() * PAGE, extra + p.extra_pages.len() * PAGE))
}

/// Heaps of every kind of resource in one (resource heap tier 2) – else no arenas.
fn tier2(dev: &ID3D12Device) -> bool {
    let mut o = D3D12_FEATURE_DATA_D3D12_OPTIONS::default();
    unsafe { dev.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &mut o as *mut _ as *mut c_void, std::mem::size_of_val(&o) as u32).is_ok() && o.ResourceHeapTier.0 >= 2 }
}

/// A sampled texture of the title at `va` (`desc`: the PC description, `console_bytes` its size on
/// the console) backed by shared 64 KiB video-memory pages; None when reserved placement is not
/// available (the caller falls back to a committed resource).
#[allow(clippy::too_many_arguments)]
pub unsafe fn place_pages(
    this: *mut c_void,
    va: usize,
    desc: &D3D12_RESOURCE_DESC,
    state: u32,
    clear: *const c_void,
    console_bytes: Option<u64>,
    riid: *const windows_sys::core::GUID,
    ppv: *mut *mut c_void,
) -> Option<HRESULT> {
    let dev = ID3D12Device::from_raw_borrowed(&this)?;
    if !tier2(dev) || std::env::var_os("XCL_VRAM_ARENAS").is_some() {
        return None;
    }
    if small_heaps() {
        if let Some(hr) = place_small(dev, this, desc, state, clear, console_bytes, riid, ppv) {
            return Some(hr);
        }
    }
    place_on_pages(dev, this, va, desc, state, clear, console_bytes, false, riid, ppv)
}

/// A GPU-owned texture of the title at `va` (`desc`: the PC description, `console_bytes` its
/// size on the console): on the title's pages there, else in the arena for that address (None:
/// neither – the caller makes a resource of its own).
#[allow(clippy::too_many_arguments)]
pub unsafe fn place(this: *mut c_void, va: usize, desc: &D3D12_RESOURCE_DESC, state: u32, clear: *const c_void, console_bytes: Option<u64>, riid: *const windows_sys::core::GUID, ppv: *mut *mut c_void) -> Option<HRESULT> {
    let dev = ID3D12Device::from_raw_borrowed(&this)?;
    if !tier2(dev) {
        return None;
    }
    if std::env::var_os("XCL_VRAM_ARENAS").is_none() {
        if let Some(hr) = place_on_pages(dev, this, va, desc, state, clear, console_bytes, true, riid, ppv) {
            return Some(hr);
        }
    }
    let info = dev.GetResourceAllocationInfo(0, &[*desc]);
    if info.SizeInBytes == u64::MAX || info.SizeInBytes == 0 || va as u64 % info.Alignment.max(1) != 0 {
        return None;
    }
    let align = (info.Alignment as usize).max(PAGE);
    let now = std::time::Instant::now();
    let (heap, offset, id) = ARENAS.lock().unwrap().take(va, info.SizeInBytes as usize, align, now, |size, align| arena_heap(dev, va, size, align))?;
    let hr = super::real::dev_create_placed_resource(this, heap.as_raw() as usize, offset, desc as *const _ as usize, state as usize, clear as usize, riid, ppv);
    if hr < 0 || (*ppv).is_null() {
        ARENAS.lock().unwrap().release(id, now);
        return None;
    }
    PLACED_TEXTURES.lock().unwrap().get_or_insert_with(HashSet::new).insert(*ppv as usize);
    ARENA_OF.lock().unwrap().get_or_insert_with(std::collections::HashMap::new).insert(*ppv as usize, id);
    Some(hr)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{Duration, Instant};

    const MIB: usize = 1 << 20;

    /// One call per heap: its tiles in runs, the other heaps' skipped.
    #[test]
    fn tile_ranges_per_heap() {
        let tiles = [(0, 5), (0, 6), (1, 0), (1, 1), (0, 9), (0, 10), (0, 11)];
        assert_eq!(heap_ranges(0, &tiles), [(true, 5, 2), (false, 0, 2), (true, 9, 3)]);
        assert_eq!(heap_ranges(1, &tiles), [(false, 0, 2), (true, 0, 2), (false, 0, 3)]);
        // shared pages out of order: runs break where the tiles do
        assert_eq!(heap_ranges(0, &[(0, 3), (0, 1), (0, 2)]), [(true, 3, 1), (true, 1, 2)]);
    }

    /// Idle pages go back after `IDLE`, and a sweep looks at the queue of idle pages alone: one
    /// used again stays, one idle again later goes when that is `IDLE` ago, and the sweeps of
    /// 20 000 placements beside 200 000 pages in use take no time to speak of (a look at every
    /// page at each of them: 4 000 000 000 looks).
    #[test]
    fn idle_pages_are_swept_from_their_queue() {
        use std::collections::{HashMap, VecDeque};
        let t0 = Instant::now();
        let at = |ms: u64| t0 + Duration::from_millis(ms);
        let mut idle: VecDeque<(PageKey, Instant)> = VecDeque::new();
        let mut pages: HashMap<usize, Page> = (0..200_000usize).map(|i| (i * PAGE, Page { tile: (0, i as u32), users: 1, idle_since: None })).collect();
        let mut extra: HashMap<(usize, usize), Page> = HashMap::from([((7 * PAGE, 3), Page { tile: (1, 0), users: 1, idle_since: None })]);
        let mut cooling: VecDeque<(Tile, Instant)> = VecDeque::new();
        // what `release_tiles` and `bind` do to a page
        fn release(p: &mut Page, idle: &mut VecDeque<(PageKey, Instant)>, key: PageKey, when: Instant) {
            (p.users, p.idle_since) = (0, Some(when));
            idle.push_back((key, when));
        }
        fn use_again(p: &mut Page) {
            (p.users, p.idle_since) = (1, None);
        }
        // pages 1, 2, 3 idle at 0 ms and an extra page at 100 ms; page 2 is used again at
        // 500 ms; page 3 too, and is idle again at 1500 ms
        for a in [PAGE, 2 * PAGE, 3 * PAGE] {
            release(pages.get_mut(&a).unwrap(), &mut idle, PageKey::Own(a), at(0));
        }
        release(extra.get_mut(&(7 * PAGE, 3)).unwrap(), &mut idle, PageKey::Extra(7 * PAGE, 3), at(100));
        use_again(pages.get_mut(&(2 * PAGE)).unwrap());
        use_again(pages.get_mut(&(3 * PAGE)).unwrap());
        release(pages.get_mut(&(3 * PAGE)).unwrap(), &mut idle, PageKey::Own(3 * PAGE), at(1500));

        sweep_idle(&mut idle, &mut pages, &mut extra, &mut cooling, at(1900));
        assert!(cooling.is_empty() && pages.len() == 200_000 && idle.len() == 5, "nothing is idle for IDLE yet");
        sweep_idle(&mut idle, &mut pages, &mut extra, &mut cooling, at(2050));
        assert_eq!(cooling.iter().map(|c| c.0).collect::<Vec<_>>(), [(0, 1)], "page 1 alone: 2 is in use, 3 idle since 1500 ms");
        assert!(!pages.contains_key(&PAGE) && pages.contains_key(&(2 * PAGE)) && pages.contains_key(&(3 * PAGE)) && extra.len() == 1);
        sweep_idle(&mut idle, &mut pages, &mut extra, &mut cooling, at(2150));
        assert_eq!(cooling.iter().map(|c| c.0).collect::<Vec<_>>(), [(0, 1), (1, 0)], "the extra page, idle since 100 ms");
        assert!(extra.is_empty() && pages.len() == 199_999);
        sweep_idle(&mut idle, &mut pages, &mut extra, &mut cooling, at(3499));
        assert!(pages.contains_key(&(3 * PAGE)));
        sweep_idle(&mut idle, &mut pages, &mut extra, &mut cooling, at(3500));
        assert_eq!(cooling.iter().map(|c| c.0).collect::<Vec<_>>(), [(0, 1), (1, 0), (0, 3)]);
        assert!(idle.is_empty() && pages.len() == 199_998 && pages[&(2 * PAGE)].users == 1);

        // every placement sweeps: with nothing idle that is nothing, whatever is in use
        let t = Instant::now();
        for k in 0..20_000u64 {
            sweep_idle(&mut idle, &mut pages, &mut extra, &mut cooling, at(4000 + k));
        }
        assert!(t.elapsed() < Duration::from_secs(2), "{:?} for 20 000 sweeps", t.elapsed());
        assert_eq!(pages.len(), 199_998);
    }

    /// Any 64 KiB page boundary inside `[va, va + bytes)` belongs to that range alone, so
    /// non-overlapping ranges never share an owned page even when only 4 KiB aligned.
    #[test]
    fn owned_pages_never_alias_disjoint_ranges() {
        assert_eq!(owned_pages(0x1_0000, Some(0x1_0000), 4), Some((0x1_0000, 1)));
        assert_eq!(owned_pages(0x1_0000, Some(0x1_4000), 4), Some((0x1_0000, 2)));
        assert_eq!(owned_pages(0x1_4000, Some(0x4000), 4), Some((0x2_0000, 0)));
        assert_eq!(owned_pages(0x1_4000, Some(0x1_0000), 4), Some((0x2_0000, 1)));
        assert_eq!(owned_pages(0x1_4000, Some(0x4_0000), 4), Some((0x2_0000, 4)));
        assert_eq!(owned_pages(0x1_4000, None, 4), None);
        // Exhaustively check 4 KiB-aligned disjoint pairs over 256 KiB.
        const K4: usize = 0x1000;
        for a in (0..16).map(|i| 0x10_0000 + i * K4) {
            for sa in [K4, 4 * K4, 16 * K4, 20 * K4] {
                let (fa, na) = owned_pages(a, Some(sa as u64), 8).unwrap();
                let pa: Vec<usize> = (0..na).map(|i| fa + i * PAGE).collect();
                for b in (0..16).map(|i| 0x10_0000 + i * K4) {
                    for sb in [K4, 4 * K4, 16 * K4, 20 * K4] {
                        if a + sa <= b || b + sb <= a {
                            let (fb, nb) = owned_pages(b, Some(sb as u64), 8).unwrap();
                            for j in 0..nb {
                                assert!(!pa.contains(&(fb + j * PAGE)), "disjoint [{a:#x}..{:#x}) and [{b:#x}..{:#x}) shared page", a + sa, b + sb);
                            }
                        }
                    }
                }
            }
        }
    }

    /// Reserved only with TILED support, and for MSAA a tiled sample count (#940: the driver
    /// refused R32G8X24_TYPELESS reserved, at 1 and 4 samples, and the debug layer said so).
    #[test]
    fn reserved_needs_tiled_format_and_tiled_msaa() {
        let tiled = D3D12_FORMAT_SUPPORT2_TILED.0;
        assert!(can_reserve(tiled, 1, 0));
        assert!(!can_reserve(0, 1, 0), "format without TILED");
        assert!(can_reserve(tiled, 4, 1));
        assert!(!can_reserve(tiled, 4, 0), "no tiled quality level at 4 samples");
        assert!(!can_reserve(0, 4, 1));
    }

    /// The device's answer agrees with CreateReservedResource: what `reservable` turns away the
    /// driver refuses, what it lets through the driver makes (WARP or a hardware adapter).
    #[test]
    fn reservable_agrees_with_the_driver() {
        let _vram_guard = VRAM_TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
        use windows::Win32::Graphics::Dxgi::Common::*;
        unsafe {
            let mut dev: Option<ID3D12Device> = None;
            if D3D12CreateDevice(None, D3D_FEATURE_LEVEL_11_0, &mut dev).is_err() {
                let factory: windows::Win32::Graphics::Dxgi::IDXGIFactory4 = windows::Win32::Graphics::Dxgi::CreateDXGIFactory1().expect("DXGI factory");
                let Ok(warp) = factory.EnumWarpAdapter::<windows::Win32::Graphics::Dxgi::IDXGIAdapter>() else { return };
                let _ = D3D12CreateDevice(&warp, D3D_FEATURE_LEVEL_11_0, &mut dev);
            }
            let Some(dev) = dev else { return };
            let mut o = D3D12_FEATURE_DATA_D3D12_OPTIONS::default();
            if dev.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &mut o as *mut _ as *mut c_void, std::mem::size_of_val(&o) as u32).is_err() || o.TiledResourcesTier.0 < D3D12_TILED_RESOURCES_TIER_2.0 {
                return;
            }
            for (format, samples) in [(DXGI_FORMAT_R32G8X24_TYPELESS, 1), (DXGI_FORMAT_R32G8X24_TYPELESS, 4), (DXGI_FORMAT_R32_TYPELESS, 1), (DXGI_FORMAT_R8G8B8A8_UNORM, 1)] {
                let flags = if format == DXGI_FORMAT_R8G8B8A8_UNORM { D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET } else { D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL };
                let desc = D3D12_RESOURCE_DESC {
                    Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D, Width: 128, Height: 128, DepthOrArraySize: 1, MipLevels: 1, Format: format,
                    SampleDesc: DXGI_SAMPLE_DESC { Count: samples, Quality: 0 }, Layout: D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, Flags: flags, ..Default::default()
                };
                let mut r: Option<ID3D12Resource> = None;
                let made = dev.CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, None, &mut r).is_ok() && r.is_some();
                assert_eq!(reservable(&dev, &desc), made, "format {} x{samples}: reservable vs CreateReservedResource", format.0);
            }
        }
    }

    /// A pool heap goes when its tiles are all free again, not before; one of video memory is
    /// kept; new pages take the lowest heaps' tiles.
    #[test]
    fn pool_heaps_go_when_none_of_their_tiles_is_in_use() {
        let t = Instant::now();
        let mut p: Pools<u32> = Pools::new();
        // three heaps of video memory and one of system memory, every tile taken
        let mut tiles = Vec::new();
        for (heap, spilled) in [(10, false), (11, false), (12, false), (13, true)] {
            tiles.push(p.add(heap, spilled));
            while let Some(tile) = p.take(spilled) {
                tiles.push(tile);
            }
        }
        assert_eq!(tiles.len(), 4 * POOL_PAGES as usize);
        assert_eq!((p.count(), p.free_tiles(), p.spilled_tiles()), ((3, 1), 0, POOL_PAGES as usize));
        // all given back but one tile of the second heap: nothing goes while they cool
        let kept = (1, 7);
        for &tile in tiles.iter().filter(|&&tile| tile != kept) {
            p.cooling.push_back((tile, t));
        }
        assert!(p.settle(t + COOL - Duration::from_millis(1)).is_empty());
        assert_eq!((p.count(), p.free_tiles()), ((3, 1), 0));
        // cooled: the heap with a tile in use stays, one empty heap of video memory is kept,
        // the other and the one of system memory go
        assert_eq!(p.settle(t + COOL), [12, 13]);
        assert_eq!((p.count(), p.free_tiles(), p.spilled_tiles()), ((2, 0), 2 * POOL_PAGES as usize - 1, 0));
        assert!(p.settle(t + COOL * 2).is_empty(), "the kept heaps stay");
        // new pages: the lowest heap's tiles; none of system memory is left; a new heap takes
        // a number that is free
        assert_eq!(p.take(false).map(|tile| tile.0), Some(0));
        assert_eq!(p.take(true), None);
        assert_eq!(p.add(14, true), (2, 0));
        // the last tile of the second heap given back: it is now the empty heap that is kept
        // (the first has a tile in use again, the new one of system memory too)
        p.cooling.push_back((kept, t + COOL * 2));
        assert_eq!(p.settle(t + COOL * 3), [] as [u32; 0], "one empty heap of video memory is kept");
        assert_eq!(p.count(), (2, 1));
    }

    /// A heap that left the pools is released when the GPU is past it: the unmappings done, and
    /// each queue's fence at the retirement – or 2 s more for a queue that submits nothing.
    #[test]
    fn a_retired_pool_heap_waits_for_the_gpu() {
        let none: [(u64, u64); 0] = [];
        assert!(retired_can_go(2, Duration::ZERO, true, none));
        assert!(!retired_can_go(2, COOL * 10, false, none), "its unmappings are not done");
        // a queue that signalled the retirement: until its fence is there, however long
        assert!(!retired_can_go(2, COOL * 10, true, [(2, 1)]));
        assert!(!retired_can_go(2, COOL * 10, true, [(3, 3), (2, 1)]));
        assert!(retired_can_go(2, Duration::ZERO, true, [(2, 2), (3, 3)]));
        // one that has submitted nothing since
        assert!(!retired_can_go(2, COOL - Duration::from_millis(1), true, [(2, 2), (1, 1)]));
        assert!(retired_can_go(2, COOL, true, [(2, 2), (1, 1)]));
    }

    #[test]
    fn the_pools_budget_is_what_the_adapters_leaves() {
        const MIB64: u64 = 1 << 20;
        // 90 % of 10,000 MiB; 4,000 in use, 2,000 of them the pools': 5,000 more for them
        assert_eq!(adapter_cap(10_000 * MIB64, None, 4_000 * MIB64, 2_000 * MIB64), 7_000 * MIB64);
        // at the budget or past it (other programs took video memory): what they have, no more
        assert_eq!(adapter_cap(10_000 * MIB64, None, 9_000 * MIB64, 2_000 * MIB64), 2_000 * MIB64);
        assert_eq!(adapter_cap(4_000 * MIB64, None, 9_000 * MIB64, 2_000 * MIB64), 2_000 * MIB64);
    }

    /// On the GPU: with the pools at their budget of video memory, new pages are in system
    /// memory, and a target on them is written and read by the GPU like any other (a clear, a
    /// copy into it, a copy out of it) and shares them with a second target at its address.
    /// Pool heaps left without a tile in use are released; those of the targets still alive
    /// are not, and the targets keep what they hold. Needs a hardware adapter with tiled
    /// resources tier 2.
    #[test]
    fn page_pools_keep_to_their_budget_spill_and_shrink() {
        let _vram_guard = VRAM_TEST_LOCK.lock().unwrap();
        use crate::gpu::recomp::runner::skip_test;
        use windows::core::Interface;
        use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
        use windows::Win32::Graphics::Dxgi::Common::*;
        use windows::Win32::Graphics::Dxgi::*;
        unsafe {
            let factory: IDXGIFactory4 = CreateDXGIFactory1().expect("DXGI factory");
            let Ok(adapter) = factory.EnumAdapters1(0) else { return skip_test("no GPU") };
            if adapter.GetDesc1().is_ok_and(|d| d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE.0 as u32 != 0) {
                return skip_test("no GPU");
            }
            let mut dev: Option<ID3D12Device> = None;
            let _ = D3D12CreateDevice(&adapter, D3D_FEATURE_LEVEL_11_0, &mut dev);
            let Some(dev) = dev.filter(|d| tier2(d)) else { return skip_test("no GPU") };
            super::super::real::adopt(dev.as_raw(), super::super::real::Kind::Device);
            let key = dev.as_raw() as usize;
            let memory: IDXGIAdapter3 = adapter.cast().unwrap();
            let non_local = || {
                let mut i = DXGI_QUERY_VIDEO_MEMORY_INFO::default();
                let _ = memory.QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &mut i);
                i.CurrentUsage
            };

            // 2048x2048 RGBA8 render targets: 16 MiB, 256 pages each
            const SIDE: usize = 2048;
            let sample = DXGI_SAMPLE_DESC { Count: 1, Quality: 0 };
            let desc = D3D12_RESOURCE_DESC { Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D, Width: SIDE as u64, Height: SIDE as u32, DepthOrArraySize: 1, MipLevels: 1, Format: DXGI_FORMAT_R8G8B8A8_UNORM, SampleDesc: sample, Flags: D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, ..Default::default() };
            let riid = &ID3D12Resource::IID as *const windows::core::GUID as *const windows_sys::core::GUID;
            let place_at = |va: usize| {
                let mut p = std::ptr::null_mut();
                let hr = place(dev.as_raw(), va, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET.0 as u32, std::ptr::null(), None, riid, &mut p);
                assert!(hr.is_some_and(|hr| hr >= 0) && !p.is_null(), "render target at {va:#x}");
                ID3D12Resource::from_raw(p)
            };
            // (tiles of video memory, tiles of system memory) of a target; None: not on pages
            let tiles_of = |r: &ID3D12Resource| -> Option<(usize, usize)> {
                let g = PAGES.lock().unwrap();
                let p = g.get(&key)?.as_ref()?;
                let t = p.targets.get(&(r.as_raw() as usize))?;
                let tiles = t.used.iter().map(|a| p.pages[a].tile).chain(t.used_extra.iter().map(|k| p.extra_pages[k].tile));
                Some(tiles.fold((0, 0), |(v, s), (h, _)| if p.pools.heaps[h as usize].as_ref().expect("a tile in use is of a heap in the pools").spilled { (v, s + 1) } else { (v + 1, s) }))
            };
            // ((heaps of video memory, of system memory), heaps released, heaps waiting for it)
            let pools = || {
                let g = PAGES.lock().unwrap();
                let p = g.get(&key).and_then(Option::as_ref).unwrap();
                (p.pools.count(), p.released, p.retiring.len())
            };

            const BASE: usize = 0x60_0000_0000;
            const STEP: usize = SIDE * SIDE * 4;
            let in_video = place_at(BASE);
            if tiles_of(&in_video).is_none() {
                return skip_test("no GPU");
            }
            // the budget: the pool heaps of video memory there are and one more
            let ((video0, spilled0), ..) = pools();
            let budget = (video0 as u64 + 1) * POOL_BYTES;
            PAGES.lock().unwrap().get_mut(&key).and_then(Option::as_mut).unwrap().budget = Budget::Fixed(budget);
            let non_local0 = non_local();
            // targets until one is all in system memory and there is a second heap there
            let mut targets = Vec::new();
            let mut spilled_at = None;
            for k in 1..128 {
                let t = place_at(BASE + k * STEP);
                let (v, s) = tiles_of(&t).unwrap();
                assert_eq!(v + s, STEP / PAGE, "a page per 64 KiB");
                targets.push(t);
                assert!(pools().0 .0 <= video0 + 1, "{} pool heaps of video memory with a budget for {}", pools().0 .0, video0 + 1);
                if spilled_at.is_none() && v == 0 {
                    spilled_at = Some(k);
                }
                if spilled_at.is_some() && pools().0 .1 >= spilled0 + 2 {
                    break;
                }
            }
            let spilled_k: usize = spilled_at.expect("no target in system memory after 127 of 16 MiB");
            let spilled = targets.swap_remove(spilled_k - 1);
            let ((video1, spilled1), released1, _) = pools();
            println!("budget {} MB: {video1} pool heaps of video memory, {spilled1} of system memory after {} targets; {} bytes more non-local video memory", budget >> 20, targets.len() + 2, non_local().saturating_sub(non_local0));
            assert!(spilled1 >= spilled0 + 2, "{spilled1} pool heaps of system memory");

            // the GPU writes and reads the target in system memory
            let queue: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC::default()).unwrap();
            let fence: ID3D12Fence = dev.CreateFence(0, D3D12_FENCE_FLAG_NONE).unwrap();
            let fence_value = std::cell::Cell::new(0u64);
            let run = |record: &dyn Fn(&ID3D12GraphicsCommandList)| {
                let alloc: ID3D12CommandAllocator = dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT).unwrap();
                let list: ID3D12GraphicsCommandList = dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &alloc, None).unwrap();
                record(&list);
                list.Close().unwrap();
                before_execute(&queue);
                queue.ExecuteCommandLists(&[Some(list.cast().unwrap())]);
                fence_value.set(fence_value.get() + 1);
                queue.Signal(&fence, fence_value.get()).unwrap();
                // no event: returns when the fence has the value
                fence.SetEventOnCompletion(fence_value.get(), windows::Win32::Foundation::HANDLE::default()).unwrap();
            };
            let barrier = |r: &ID3D12Resource, from: D3D12_RESOURCE_STATES, to: D3D12_RESOURCE_STATES| {
                let t = D3D12_RESOURCE_TRANSITION_BARRIER { pResource: std::mem::transmute_copy(r), Subresource: D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, StateBefore: from, StateAfter: to };
                D3D12_RESOURCE_BARRIER { Type: D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, Anonymous: D3D12_RESOURCE_BARRIER_0 { Transition: std::mem::ManuallyDrop::new(t) }, ..Default::default() }
            };
            let buffer = |bytes: u64, kind: D3D12_HEAP_TYPE, state: D3D12_RESOURCE_STATES| {
                let d = D3D12_RESOURCE_DESC { Dimension: D3D12_RESOURCE_DIMENSION_BUFFER, Width: bytes, Height: 1, DepthOrArraySize: 1, MipLevels: 1, SampleDesc: sample, Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR, ..Default::default() };
                let mut r: Option<ID3D12Resource> = None;
                dev.CreateCommittedResource(&D3D12_HEAP_PROPERTIES { Type: kind, ..Default::default() }, D3D12_HEAP_FLAG_NONE, &d, state, None, &mut r).unwrap();
                r.unwrap()
            };
            let mut whole = D3D12_PLACED_SUBRESOURCE_FOOTPRINT::default();
            let mut total = 0u64;
            dev.GetCopyableFootprints(&desc, 0, 1, 0, Some(&mut whole), None, None, Some(&mut total));
            let pitch = whole.Footprint.RowPitch as usize;
            let readback = buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            // the pixel at (x, y) of a target in the render-target state
            let read = |r: &ID3D12Resource, at: &[(usize, usize)]| -> Vec<[u8; 4]> {
                run(&|list| {
                    list.ResourceBarrier(&[barrier(r, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE)]);
                    let dst = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(&readback), Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { PlacedFootprint: whole } };
                    let src = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(r), Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: 0 } };
                    list.CopyTextureRegion(&dst, 0, 0, 0, &src, None);
                    list.ResourceBarrier(&[barrier(r, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)]);
                });
                let mut p = std::ptr::null_mut();
                readback.Map(0, None, Some(&mut p)).unwrap();
                let bytes = std::slice::from_raw_parts(p as *const u8, total as usize);
                let out = at.iter().map(|&(x, y)| bytes[y * pitch + x * 4..][..4].try_into().unwrap()).collect();
                readback.Unmap(0, None);
                out
            };
            let rtv_heap: ID3D12DescriptorHeap = dev.CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC { Type: D3D12_DESCRIPTOR_HEAP_TYPE_RTV, NumDescriptors: 1, ..Default::default() }).unwrap();
            let rtv = rtv_heap.GetCPUDescriptorHandleForHeapStart();
            // a 64x64 patch for a copy into the target at (1000, 1500): pixel (x, y) is [x, y, x ^ y, 0xEE]
            const PATCH: usize = 64;
            let upload = buffer((PATCH * PATCH * 4) as u64, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            let mut p = std::ptr::null_mut();
            upload.Map(0, None, Some(&mut p)).unwrap();
            for (i, px) in std::slice::from_raw_parts_mut(p as *mut [u8; 4], PATCH * PATCH).iter_mut().enumerate() {
                let (x, y) = ((i % PATCH) as u8, (i / PATCH) as u8);
                *px = [x, y, x ^ y, 0xEE];
            }
            upload.Unmap(0, None);
            let patch = D3D12_PLACED_SUBRESOURCE_FOOTPRINT { Offset: 0, Footprint: D3D12_SUBRESOURCE_FOOTPRINT { Format: DXGI_FORMAT_R8G8B8A8_UNORM, Width: PATCH as u32, Height: PATCH as u32, Depth: 1, RowPitch: (PATCH * 4) as u32 } };
            let clear_and_patch = |r: &ID3D12Resource, colour: [f32; 4]| {
                run(&|list| {
                    dev.CreateRenderTargetView(r, None, rtv);
                    list.ClearRenderTargetView(rtv, &colour, None);
                    list.ResourceBarrier(&[barrier(r, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST)]);
                    let dst = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(r), Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: 0 } };
                    let src = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(&upload), Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { PlacedFootprint: patch } };
                    list.CopyTextureRegion(&dst, 1000, 1500, 0, &src, None);
                    list.ResourceBarrier(&[barrier(r, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET)]);
                })
            };
            // corners, the middle, and the patch's corners and inside
            let at = [(0, 0), (SIDE - 1, 0), (5, 1024), (0, SIDE - 1), (SIDE - 1, SIDE - 1), (999, 1500), (1000, 1500), (1063, 1563), (1064, 1563), (1017, 1530)];
            let expect = |c: [u8; 4]| [c, c, c, c, c, c, [0, 0, 0, 0xEE], [63, 63, 0, 0xEE], c, [17, 30, 17 ^ 30, 0xEE]];
            clear_and_patch(&spilled, [1.0, 0.0, 0.2, 1.0]);
            clear_and_patch(&in_video, [0.0, 1.0, 0.2, 1.0]);
            assert_eq!(read(&spilled, &at), expect([255, 0, 51, 255]), "the target in system memory");
            assert_eq!(read(&in_video, &at), expect([0, 255, 51, 255]), "the target in video memory");
            // a second target at its address has its pages: the same texels
            let alias = place_at(BASE + spilled_k * STEP);
            assert_eq!(tiles_of(&alias), Some((0, STEP / PAGE)));
            assert_eq!(pools().0, (video1, spilled1), "no pool heap more for a target at an address that has them");
            assert_eq!(read(&alias, &at), expect([255, 0, 51, 255]), "a second target at the address of the one in system memory");
            drop(alias);

            // the other targets released: their pages go back, the heaps left empty are released
            // (not the two targets' own), and the targets keep what they hold
            let kept = (tiles_of(&spilled), tiles_of(&in_video));
            drop(targets);
            let start = Instant::now();
            let settled = || {
                let ((_, spilled), released, retiring) = pools();
                spilled < spilled1 && released > released1 && retiring == 0
            };
            while !settled() && start.elapsed() < Duration::from_secs(30) {
                std::thread::sleep(Duration::from_millis(250));
                before_execute(&queue);
            }
            let ((video2, spilled2), released2, _) = pools();
            println!("{} pool heaps released {:?} after their targets; {video2} of video memory and {spilled2} of system memory left", released2 - released1, start.elapsed());
            assert!(released2 > released1, "no pool heap released 30 s after its targets");
            assert!(spilled2 < spilled1 && spilled2 >= 1, "{spilled2} pool heaps of system memory left of {spilled1}");
            assert_eq!((tiles_of(&spilled), tiles_of(&in_video)), kept, "the targets alive keep their tiles, of heaps still in the pools");
            assert_eq!(read(&spilled, &at), expect([255, 0, 51, 255]), "the target in system memory, after the release");
            assert_eq!(read(&in_video, &at), expect([0, 255, 51, 255]), "the target in video memory, after the release");
            clear_and_patch(&spilled, [0.2, 1.0, 0.0, 1.0]);
            assert_eq!(read(&spilled, &at), expect([51, 255, 0, 255]), "the target in system memory written again");

            drop((spilled, in_video));
            if let Some(p) = PAGES.lock().unwrap().get_mut(&key).and_then(Option::as_mut) {
                p.budget = Budget::of(&dev);
            }
        }
    }

    /// A placement's arena id, with heaps numbered as they are made.
    fn take(a: &mut Arenas<u32>, va: usize, bytes: usize, align: usize, now: Instant) -> u64 {
        let n = a.list.len() as u32;
        a.take(va, bytes, align, now, |_, _| Some(n)).unwrap().2
    }

    #[test]
    fn an_arena_is_shared_at_its_addresses_and_otherwise_its_placements_size() {
        let (t, a0) = (Instant::now(), 0x1_0000_0000);
        let mut a = Arenas::new();
        let first = take(&mut a, a0, 70 * MIB, PAGE, t);
        assert_eq!(a.bytes(), 70 * MIB, "the placement's size, not 256 MiB");
        // the same address, and a smaller placement inside: the same memory
        assert_eq!(take(&mut a, a0, 70 * MIB, PAGE, t), first);
        assert_eq!(take(&mut a, a0 + 4 * MIB, 8 * MIB, PAGE, t), first);
        // one straddling its end gets an arena of its own size; so does an MSAA target that needs 4 MiB
        let straddle = take(&mut a, a0 + 64 * MIB, 70 * MIB, PAGE, t);
        let msaa = take(&mut a, a0 + 8 * MIB, 8 * MIB, 4 * MIB, t);
        assert!(straddle != first && msaa != first && msaa != straddle);
        assert_eq!(a.bytes(), (70 + 70 + 8) * MIB);
        // the newest arena holding an address is the one placements there share
        assert_eq!(take(&mut a, a0 + 64 * MIB, 4 * MIB, PAGE, t), straddle);
    }

    #[test]
    fn an_arena_no_resource_uses_goes_after_two_seconds() {
        let (t, a0) = (Instant::now(), 0x1_0000_0000);
        let mut a = Arenas::new();
        let id = take(&mut a, a0, 70 * MIB, PAGE, t);
        a.release(id, t);
        // placed again within 2 s: the same memory (with what it holds)
        assert_eq!(take(&mut a, a0, 70 * MIB, PAGE, t + Duration::from_secs(1)), id);
        a.release(id, t + Duration::from_secs(1));
        take(&mut a, a0 + 512 * MIB, MIB, PAGE, t + Duration::from_secs(3) + Duration::from_millis(1));
        assert_eq!(a.bytes(), MIB, "the idle arena went");
    }

    /// The retail game's pattern (HANDOFF): targets packed back to back over 3.5 GB of its
    /// addresses and placed again and again, each round at the last round's addresses or a third
    /// of a target further on, so placements straddle earlier ones. Main's 256 MiB arenas merged
    /// straddled arenas into growing unions while the old ones lived on (15 GB in 2 s).
    #[test]
    fn streamed_targets_stay_bounded() {
        const S: usize = 64 * MIB; // a 3840x2160 D32S8, rounded
        const K: usize = 8; // targets a round
        const SPAN: usize = 3584 * MIB;
        let (t0, a0) = (Instant::now(), 0x1_0000_0000usize);
        let mut a = Arenas::new();
        let mut live: std::collections::VecDeque<Vec<u64>> = std::collections::VecDeque::new();
        let mut bytes = Vec::new();
        for r in 0..400usize {
            let now = t0 + Duration::from_millis(500 * r as u64);
            let start = ((r / 2) * S / 3) % (SPAN - K * S) & !(PAGE - 1);
            live.push_back((0..K).map(|k| take(&mut a, a0 + start + k * S, S, PAGE, now)).collect());
            // a target lives three rounds (1.5 s)
            if live.len() > 3 {
                for id in live.pop_front().unwrap() {
                    a.release(id, now);
                }
            }
            bytes.push(a.bytes());
        }
        // live: 3 rounds of targets; idle and kept (< 2 s): 4 more
        let most = *bytes.iter().max().unwrap();
        assert!(most <= 7 * K * S, "{most} bytes of arenas");
        let early = *bytes[20..120].iter().max().unwrap();
        let late = *bytes[300..].iter().max().unwrap();
        assert!(late <= early, "arenas grow over time: {early} then {late} bytes");
    }

    /// Small textures take their size in 4 KiB slots of 16 MiB heaps; a released one's slots
    /// are taken again 2 s later, not before; a heap with nothing in it goes, but one.
    #[test]
    fn small_textures_are_packed_in_heaps_and_their_slots_reused() {
        let t0 = Instant::now();
        let mut s: SmallHeaps<u32> = SmallHeaps::new();
        let mut made = 0u32;
        let mut take = |s: &mut SmallHeaps<u32>, bytes: usize, now: Instant| {
            s.take(bytes, now, || {
                made += 1;
                Some(made)
            })
            .unwrap()
        };
        // 3.3 KiB, 4 KiB + 1 and 32 KiB: 1, 2 and 8 slots, back to back in the first heap
        let a = take(&mut s, 3300, t0);
        let b = take(&mut s, 4097, t0);
        let c = take(&mut s, SMALL_MAX, t0);
        assert_eq!([(a.0, a.1), (b.0, b.1), (c.0, c.1)], [(1, 0), (1, SMALL_SLOT), (1, 3 * SMALL_SLOT)]);
        assert_eq!(s.counts(), (3, 11 * SMALL_SLOT, SMALL_HEAP));

        // released: its slots cool for 2 s, the next texture goes after the others
        s.release(b.2, t0);
        let d = take(&mut s, 8000, t0 + Duration::from_secs(1));
        assert_eq!((d.0, d.1), (1, 11 * SMALL_SLOT));
        let e = take(&mut s, 8000, t0 + COOL);
        assert_eq!((e.0, e.1), (1, SMALL_SLOT), "the released slots, the lowest free");
        // a hole of one slot is not taken for two
        s.release(a.2, t0);
        let f = take(&mut s, 5000, t0 + COOL);
        assert_eq!((f.0, f.1), (1, 13 * SMALL_SLOT));
        assert_eq!(take(&mut s, 100, t0 + COOL).1, 0);

        // 13,000 textures of 3.3 KiB: 4 heaps (64 MiB) where pages took 813 MiB
        let mut s: SmallHeaps<u32> = SmallHeaps::new();
        let ids: Vec<u64> = (0..13_000).map(|_| take(&mut s, 3300, t0).2).collect();
        assert_eq!(s.counts(), (13_000, 13_000 * SMALL_SLOT, 4 * SMALL_HEAP));
        assert!(s.take(SMALL_HEAP + 1, t0, || None).is_none(), "more than a heap holds");
        assert!(s.take(SMALL_HEAP, t0, || None).is_none(), "no heap: the caller places it elsewhere");
        // all released: after 2 s one empty heap is left, and the next texture is in it
        for id in ids {
            s.release(id, t0);
        }
        s.sweep(t0 + Duration::from_secs(1));
        assert_eq!(s.counts(), (0, 0, 4 * SMALL_HEAP));
        s.sweep(t0 + COOL);
        assert_eq!(s.counts(), (0, 0, SMALL_HEAP));
        let g = take(&mut s, 3300, t0 + COOL);
        assert_eq!((g.1, s.counts()), (0, (1, SMALL_SLOT, SMALL_HEAP)));
    }

    /// On the GPU: 256 small sampled textures (block-compressed with mips, and one-channel)
    /// go to one heap of small textures, far under the 64 KiB each takes on the pages; one over
    /// half a page and a render target do not go there; released, their slots are counted out.
    /// Needs a hardware adapter (skips on WARP).
    #[test]
    fn small_title_textures_share_heaps_of_video_memory() {
        let _vram_guard = VRAM_TEST_LOCK.lock().unwrap();
        use windows::core::Interface;
        use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
        use windows::Win32::Graphics::Dxgi::Common::*;
        use windows::Win32::Graphics::Dxgi::*;
        unsafe {
            let factory: IDXGIFactory4 = CreateDXGIFactory1().expect("DXGI factory");
            let Ok(adapter) = factory.EnumAdapters1(0) else { return };
            if adapter.GetDesc1().is_ok_and(|d| d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE.0 as u32 != 0) {
                return;
            }
            let mut dev: Option<ID3D12Device> = None;
            let _ = D3D12CreateDevice(&adapter, D3D_FEATURE_LEVEL_11_0, &mut dev);
            let Some(dev) = dev.filter(|d| tier2(d)) else { return };
            super::super::real::adopt(dev.as_raw(), super::super::real::Kind::Device);
            let sample = DXGI_SAMPLE_DESC { Count: 1, Quality: 0 };
            let tex = |w: u64, h: u32, mips: u16, format: DXGI_FORMAT| D3D12_RESOURCE_DESC { Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D, Width: w, Height: h, DepthOrArraySize: 1, MipLevels: mips, Format: format, SampleDesc: sample, ..Default::default() };
            let riid = &ID3D12Resource::IID as *const windows::core::GUID as *const windows_sys::core::GUID;
            let place_at = |d: &D3D12_RESOURCE_DESC, console_bytes: u64| -> Option<ID3D12Resource> {
                let mut p = std::ptr::null_mut();
                let hr = place_small(&dev, dev.as_raw(), d, D3D12_RESOURCE_STATE_COPY_DEST.0 as u32, std::ptr::null(), Some(console_bytes), riid, &mut p)?;
                assert!(hr >= 0 && !p.is_null());
                Some(ID3D12Resource::from_raw(p))
            };
            let (n0, slots0, heaps0) = small_counts();
            let kinds = [tex(64, 64, 7, DXGI_FORMAT_BC7_UNORM), tex(32, 32, 6, DXGI_FORMAT_BC1_UNORM), tex(128, 64, 1, DXGI_FORMAT_BC3_UNORM), tex(64, 64, 1, DXGI_FORMAT_R8_UNORM)];
            let mut textures = Vec::new();
            for k in 0..256 {
                let d = &kinds[k % kinds.len()];
                let Some(r) = place_at(d, 3300) else {
                    println!("SKIPPED small_title_textures_share_heaps_of_video_memory: the driver places no {}x{} format {} texture at 4 KiB", d.Width, d.Height, d.Format.0);
                    crate::gpu::recomp::runner::skip_test("no small texture placement");
                    return;
                };
                let got = r.GetDesc();
                assert_eq!((got.Width, got.Height, got.MipLevels, got.Format), (d.Width, d.Height, d.MipLevels, d.Format));
                textures.push(r);
            }
            let (n, slots, heaps) = small_counts();
            println!("256 small textures: {} bytes of slots, {} bytes of heaps", slots - slots0, heaps - heaps0);
            assert_eq!(n - n0, 256);
            assert!(slots - slots0 <= 256 * SMALL_MAX && heaps - heaps0 <= SMALL_HEAP, "{} bytes of slots, {} of heaps", slots - slots0, heaps - heaps0);
            assert!(slots - slots0 < 256 * PAGE / 2, "under half of what the pages take");
            // too large on the console, too large on the PC, a render target: not here
            assert!(place_at(&kinds[0], SMALL_MAX as u64 + 1).is_none());
            assert!(place_at(&tex(512, 512, 1, DXGI_FORMAT_R8G8B8A8_UNORM), 3300).is_none());
            assert!(place_at(&D3D12_RESOURCE_DESC { Flags: D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, ..tex(64, 64, 1, DXGI_FORMAT_R8_UNORM) }, 3300).is_none());
            // released (as the Release hook does): counted out
            for r in textures {
                forget(r.as_raw() as usize);
            }
            assert_eq!(small_counts().0, n0);
        }
    }

    /// On the GPU: D32S8 targets 256 MiB apart take their own size of video memory, not an arena
    /// of 256 MiB each; depth and stencil survive a clear, CopyResource and a read of both planes;
    /// arenas no target uses go. Needs a hardware adapter (skips on WARP).
    #[test]
    fn d32s8_targets_take_their_own_size_of_video_memory() {
        let _vram_guard = VRAM_TEST_LOCK.lock().unwrap();
        use windows::core::Interface;
        use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
        use windows::Win32::Graphics::Dxgi::Common::*;
        use windows::Win32::Graphics::Dxgi::*;
        unsafe {
            let factory: IDXGIFactory4 = CreateDXGIFactory1().expect("DXGI factory");
            let Ok(adapter) = factory.EnumAdapters1(0) else { return };
            if adapter.GetDesc1().is_ok_and(|d| d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE.0 as u32 != 0) {
                return;
            }
            let mut dev: Option<ID3D12Device> = None;
            let _ = D3D12CreateDevice(&adapter, D3D_FEATURE_LEVEL_11_0, &mut dev);
            let Some(dev) = dev.filter(|d| tier2(d)) else { return };
            super::super::real::adopt(dev.as_raw(), super::super::real::Kind::Device);
            let memory: IDXGIAdapter3 = adapter.cast().unwrap();
            let local = || {
                let mut i = DXGI_QUERY_VIDEO_MEMORY_INFO::default();
                let _ = memory.QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mut i);
                i.CurrentUsage
            };
            let sample = DXGI_SAMPLE_DESC { Count: 1, Quality: 0 };
            let tex = |w: u64, h: u32| D3D12_RESOURCE_DESC { Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D, Width: w, Height: h, DepthOrArraySize: 1, MipLevels: 1, Format: DXGI_FORMAT_R32G8X24_TYPELESS, SampleDesc: sample, Flags: D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, ..Default::default() };
            let ds = D3D12_DEPTH_STENCIL_VALUE { Depth: 1.0, Stencil: 0 };
            let clear = D3D12_CLEAR_VALUE { Format: DXGI_FORMAT_D32_FLOAT_S8X24_UINT, Anonymous: D3D12_CLEAR_VALUE_0 { DepthStencil: ds } };
            let riid = &ID3D12Resource::IID as *const windows::core::GUID as *const windows_sys::core::GUID;
            let place_at = |va: usize, d: &D3D12_RESOURCE_DESC| {
                let mut p = std::ptr::null_mut();
                let hr = place(dev.as_raw(), va, d, D3D12_RESOURCE_STATE_DEPTH_WRITE.0 as u32, &clear as *const _ as *const c_void, None, riid, &mut p);
                assert!(hr.is_some_and(|hr| hr >= 0) && !p.is_null(), "D32S8 target at {va:#x}");
                ID3D12Resource::from_raw(p)
            };

            // 14 4K targets over 3.5 GB of addresses
            let big = tex(3840, 2160);
            let size = dev.GetResourceAllocationInfo(0, &[big]).SizeInBytes as usize;
            let (used0, arenas0) = (local(), arena_bytes().0);
            let targets: Vec<ID3D12Resource> = (0..14).map(|k| place_at(0x40_0000_0000 + k * 256 * MIB, &big)).collect();
            let (used, arenas) = (local().saturating_sub(used0) as usize, arena_bytes().0 - arenas0);
            println!("14 D32S8 3840x2160 targets ({size} bytes each): {arenas} bytes of arenas, {used} bytes more local video memory");
            assert!(arenas <= 14 * size.next_multiple_of(PAGE), "{arenas} bytes of arenas");
            assert!(used < 14 * 256 * MIB, "{used} bytes of video memory");
            drop(targets);

            // clear one, copy it to another, read both planes back
            let small = tex(64, 64);
            let (a, b) = (place_at(0x48_0000_0000, &small), place_at(0x48_1000_0000, &small));
            let dsv_heap: ID3D12DescriptorHeap = dev.CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC { Type: D3D12_DESCRIPTOR_HEAP_TYPE_DSV, NumDescriptors: 1, ..Default::default() }).unwrap();
            let dsv = dsv_heap.GetCPUDescriptorHandleForHeapStart();
            let view = D3D12_DEPTH_STENCIL_VIEW_DESC { Format: DXGI_FORMAT_D32_FLOAT_S8X24_UINT, ViewDimension: D3D12_DSV_DIMENSION_TEXTURE2D, ..Default::default() };
            dev.CreateDepthStencilView(&a, Some(&view), dsv);
            let mut layouts = [D3D12_PLACED_SUBRESOURCE_FOOTPRINT::default(); 2];
            let mut total = 0u64;
            dev.GetCopyableFootprints(&small, 0, 2, 0, Some(layouts.as_mut_ptr()), None, None, Some(&mut total));
            let buffer = D3D12_RESOURCE_DESC { Dimension: D3D12_RESOURCE_DIMENSION_BUFFER, Width: total, Height: 1, DepthOrArraySize: 1, MipLevels: 1, SampleDesc: sample, Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR, ..Default::default() };
            let mut readback: Option<ID3D12Resource> = None;
            let heap = D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_READBACK, ..Default::default() };
            dev.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, None, &mut readback).unwrap();
            let readback = readback.unwrap();
            let queue: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC::default()).unwrap();
            let alloc: ID3D12CommandAllocator = dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT).unwrap();
            let list: ID3D12GraphicsCommandList = dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &alloc, None).unwrap();
            let barrier = |r: &ID3D12Resource, from: D3D12_RESOURCE_STATES, to: D3D12_RESOURCE_STATES| {
                let t = D3D12_RESOURCE_TRANSITION_BARRIER { pResource: std::mem::transmute_copy(r), Subresource: D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, StateBefore: from, StateAfter: to };
                D3D12_RESOURCE_BARRIER { Type: D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, Anonymous: D3D12_RESOURCE_BARRIER_0 { Transition: std::mem::ManuallyDrop::new(t) }, ..Default::default() }
            };
            list.ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.25, 0x5A, None);
            list.ResourceBarrier(&[barrier(&a, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE), barrier(&b, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_DEST)]);
            list.CopyResource(&b, &a);
            list.ResourceBarrier(&[barrier(&b, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE)]);
            for (plane, layout) in layouts.iter().enumerate() {
                let dst = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(&readback), Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { PlacedFootprint: *layout } };
                let src = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(&b), Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: plane as u32 } };
                list.CopyTextureRegion(&dst, 0, 0, 0, &src, None);
            }
            list.Close().unwrap();
            queue.ExecuteCommandLists(&[Some(list.cast().unwrap())]);
            let fence: ID3D12Fence = dev.CreateFence(0, D3D12_FENCE_FLAG_NONE).unwrap();
            queue.Signal(&fence, 1).unwrap();
            // no event: returns when the fence has the value
            fence.SetEventOnCompletion(1, windows::Win32::Foundation::HANDLE::default()).unwrap();
            let mut p = std::ptr::null_mut();
            readback.Map(0, None, Some(&mut p)).unwrap();
            let bytes = std::slice::from_raw_parts(p as *const u8, total as usize);
            let row = |plane: usize, y: usize| layouts[plane].Offset as usize + y * layouts[plane].Footprint.RowPitch as usize;
            for y in [0, 31, 63] {
                let depth = f32::from_le_bytes(bytes[row(0, y)..row(0, y) + 4].try_into().unwrap());
                assert_eq!((depth, bytes[row(1, y)]), (0.25, 0x5A), "row {y}: depth, stencil");
            }
            readback.Unmap(0, None);
            drop((a, b));

            // 2 s later the arenas no target uses are let go: one new target's is all that is left
            std::thread::sleep(IDLE + Duration::from_millis(100));
            let last = place_at(0x50_0000_0000, &small);
            let small_size = dev.GetResourceAllocationInfo(0, &[small]).SizeInBytes as usize;
            assert_eq!(arena_bytes().0 - arenas0, small_size.next_multiple_of(PAGE));
            drop(last);
        }
    }

    /// Page budget: the least recently touched CPU-filled texture gives its
    /// pages back first, a GPU-written one never does, and an evicted one maps again.
    #[test]
    fn page_budget_takes_unseen_textures_first_and_passes_over_shared_pages() {
        let _vram_guard = VRAM_TEST_LOCK.lock().unwrap();
        use windows::core::Interface;
        use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
        use windows::Win32::Graphics::Dxgi::Common::*;
        use windows::Win32::Graphics::Dxgi::*;
        unsafe {
            let factory: IDXGIFactory4 = CreateDXGIFactory1().expect("DXGI factory");
            let Ok(adapter) = factory.EnumAdapters1(0) else { return };
            if adapter.GetDesc1().is_ok_and(|d| d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE.0 as u32 != 0) {
                return;
            }
            let mut dev: Option<ID3D12Device> = None;
            let _ = D3D12CreateDevice(&adapter, D3D_FEATURE_LEVEL_11_0, &mut dev);
            let Some(dev) = dev.filter(|d| tier2(d)) else { return };
            super::super::real::adopt(dev.as_raw(), super::super::real::Kind::Device);
            let desc = D3D12_RESOURCE_DESC { Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D, Width: 512, Height: 512, DepthOrArraySize: 1, MipLevels: 1, Format: DXGI_FORMAT_R8G8B8A8_UNORM, SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 }, ..Default::default() };
            let riid = &ID3D12Resource::IID as *const windows::core::GUID as *const windows_sys::core::GUID;
            let place_at = |va: usize| {
                let mut p = std::ptr::null_mut();
                let hr = place_pages(dev.as_raw(), va, &desc, D3D12_RESOURCE_STATE_COMMON.0 as u32, std::ptr::null(), Some(1 << 20), riid, &mut p);
                if !hr.is_some_and(|hr| hr >= 0) || p.is_null() {
                    return None;
                }
                Some(ID3D12Resource::from_raw(p))
            };
            // two textures at the same address share every page
            let (Some(a), Some(b), Some(unseen), Some(own)) = (place_at(0x61_0000_0000), place_at(0x61_0000_0000), place_at(0x61_1000_0000), place_at(0x61_2000_0000)) else {
                crate::gpu::recomp::runner::skip_test("no reserved textures");
                return;
            };
            let key = |r: &ID3D12Resource| r.as_raw() as usize;
            for r in [&a, &b, &unseen, &own] {
                filled_from_memory(key(r));
            }
            let dev_key = dev.as_raw() as usize;
            let now = Instant::now();
            HEAPS_LOOKED.store(true, std::sync::atomic::Ordering::Relaxed);
            {
                let mut g = PAGES.lock().unwrap();
                let pages = g.get_mut(&dev_key).unwrap().as_mut().unwrap();
                for (r, age, seen) in [(&a, 60, Seen::CpuOnly), (&b, 60, Seen::CpuOnly), (&unseen, 3, Seen::None), (&own, 30, Seen::CpuOnly)] {
                    let t = pages.targets.get_mut(&key(r)).unwrap();
                    t.touched = now - Duration::from_secs(age);
                    t.seen = seen;
                }
                pages.evict_lru(1);
                assert!(!pages.targets[&key(&unseen)].mapped, "a texture with no T# goes first, though younger");
                assert!(pages.targets[&key(&a)].mapped && pages.targets[&key(&b)].mapped && pages.targets[&key(&own)].mapped);
                pages.evict_lru(1);
                assert!(pages.targets[&key(&a)].mapped && pages.targets[&key(&b)].mapped, "shared pages give nothing back: passed over");
                assert!(!pages.targets[&key(&own)].mapped, "the next with pages of its own is evicted");
                assert!(pages.evict_after.is_none(), "rounds that gave back what was wanted are not held back");
                pages.evict_lru(usize::MAX);
                assert!(pages.evict_after.is_some_and(|t| t > Instant::now()), "a round that fell short holds the next back");
                assert!(pages.targets[&key(&a)].mapped && pages.targets[&key(&b)].mapped);
            }
            HEAPS_LOOKED.store(false, std::sync::atomic::Ordering::Relaxed);
            assert!(take_evicted(key(&unseen)) && take_evicted(key(&own)));
            drop((a, b, unseen, own));
        }
    }

    #[test]
    fn page_budget_evicts_the_least_recently_touched_cpu_filled_texture() {
        let _vram_guard = VRAM_TEST_LOCK.lock().unwrap();
        use windows::core::Interface;
        use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
        use windows::Win32::Graphics::Dxgi::Common::*;
        use windows::Win32::Graphics::Dxgi::*;
        unsafe {
            let factory: IDXGIFactory4 = CreateDXGIFactory1().expect("DXGI factory");
            let Ok(adapter) = factory.EnumAdapters1(0) else { return };
            if adapter.GetDesc1().is_ok_and(|d| d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE.0 as u32 != 0) {
                return;
            }
            let mut dev: Option<ID3D12Device> = None;
            let _ = D3D12CreateDevice(&adapter, D3D_FEATURE_LEVEL_11_0, &mut dev);
            let Some(dev) = dev.filter(|d| tier2(d)) else { return };
            super::super::real::adopt(dev.as_raw(), super::super::real::Kind::Device);
            let desc = D3D12_RESOURCE_DESC { Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D, Width: 512, Height: 512, DepthOrArraySize: 1, MipLevels: 1, Format: DXGI_FORMAT_R8G8B8A8_UNORM, SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 }, ..Default::default() };
            let riid = &ID3D12Resource::IID as *const windows::core::GUID as *const windows_sys::core::GUID;
            let place_at = |va: usize| {
                let mut p = std::ptr::null_mut();
                let hr = place_pages(dev.as_raw(), va, &desc, D3D12_RESOURCE_STATE_COMMON.0 as u32, std::ptr::null(), Some(1 << 20), riid, &mut p);
                if !hr.is_some_and(|hr| hr >= 0) || p.is_null() {
                    return None;
                }
                Some(ID3D12Resource::from_raw(p))
            };
            let (Some(old), Some(newer), Some(written)) = (place_at(0x60_0000_0000), place_at(0x60_1000_0000), place_at(0x60_2000_0000)) else {
                crate::gpu::recomp::runner::skip_test("no reserved textures");
                return;
            };
            let key = |r: &ID3D12Resource| r.as_raw() as usize;
            // not kept in step with the title's memory: nothing would bring it back
            let Some(alone) = place_at(0x60_3000_0000) else { return };
            for r in [&old, &newer, &written] {
                filled_from_memory(key(r));
            }
            pin(key(&written));
            let dev_key = dev.as_raw() as usize;
            let now = Instant::now();
            {
                let mut g = PAGES.lock().unwrap();
                let pages = g.get_mut(&dev_key).unwrap().as_mut().unwrap();
                for (r, age) in [(&old, 30), (&newer, 20), (&written, 40), (&alone, 50)] {
                    pages.targets.get_mut(&key(r)).unwrap().touched = now - Duration::from_secs(age);
                }
                let used_before = pages.pages.len();
                pages.evict_lru(1);
                assert!(!pages.targets[&key(&old)].mapped, "the oldest CPU-filled texture is evicted");
                assert!(pages.targets[&key(&newer)].mapped, "one is enough for one tile");
                assert!(pages.targets[&key(&written)].mapped, "a GPU-written texture is never evicted");
                assert!(pages.targets[&key(&alone)].mapped, "nor one that is not kept in step with the title's memory");
                assert!(pages.pages.len() < used_before, "its pages went back");
            }
            assert!(take_evicted(key(&old)));
            assert!(!take_evicted(key(&old)), "true once");
            assert!(remap_if_unmapped(&old));
            assert!(PAGES.lock().unwrap().get(&dev_key).unwrap().as_ref().unwrap().targets[&key(&old)].mapped, "mapped again");
            drop((old, newer, written, alone));
        }
    }

    /// Budget: the top mips a texture placed at the budget goes without – two from 2048 texels,
    /// one from 1024, block-compressed 2D textures with a chain only, never a packed mip.
    #[test]
    fn large_block_compressed_textures_lose_their_top_mips_at_the_budget() {
        use windows::Win32::Graphics::Dxgi::Common::*;
        let tex = |w: u64, h: u32, mips: u16, format| D3D12_RESOURCE_DESC { Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D, Width: w, Height: h, DepthOrArraySize: 1, MipLevels: mips, Format: format, SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 }, ..Default::default() };
        assert_eq!(trim_mips(&tex(4096, 4096, 13, DXGI_FORMAT_BC7_UNORM), 6), 2);
        assert_eq!(trim_mips(&tex(2048, 2048, 12, DXGI_FORMAT_BC1_UNORM_SRGB), 5), 2);
        assert_eq!(trim_mips(&tex(4096, 1024, 13, DXGI_FORMAT_BC5_UNORM), 4), 1, "by its shorter side");
        assert_eq!(trim_mips(&tex(1024, 1024, 11, DXGI_FORMAT_BC3_UNORM), 4), 1);
        assert_eq!(trim_mips(&tex(512, 512, 10, DXGI_FORMAT_BC7_UNORM), 3), 0, "small: whole");
        assert_eq!(trim_mips(&tex(2048, 2048, 1, DXGI_FORMAT_BC7_UNORM), 1), 0, "its only mip stays");
        assert_eq!(trim_mips(&tex(2048, 2048, 2, DXGI_FORMAT_BC7_UNORM), 2), 1, "its last mip stays");
        assert_eq!(trim_mips(&tex(2048, 2048, 12, DXGI_FORMAT_BC7_UNORM), 2), 1, "a packed mip is not left out");
        assert_eq!(trim_mips(&tex(2048, 2048, 12, DXGI_FORMAT_R8G8B8A8_UNORM), 5), 0, "not block-compressed: the GPU may write it");
        let volume = D3D12_RESOURCE_DESC { Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE3D, DepthOrArraySize: 64, ..tex(2048, 2048, 12, DXGI_FORMAT_BC7_UNORM) };
        assert_eq!(trim_mips(&volume, 5), 0);

        // two slices of 4 mips (64, 16, 4 tiles and a packed one): mips 0 and 1 of each slice
        let t = |start: u32, w: u32| D3D12_SUBRESOURCE_TILING { WidthInTiles: w, HeightInTiles: w as u16, DepthInTiles: 1, StartTileIndexInOverallResource: start };
        let packed = D3D12_SUBRESOURCE_TILING { StartTileIndexInOverallResource: u32::MAX, ..Default::default() };
        let tilings = [t(0, 8), t(64, 4), t(80, 2), packed, t(85, 8), t(149, 4), t(165, 2), packed];
        assert_eq!(trimmed_tiles(&tilings, 4, 2), [(0, 64), (64, 16), (85, 64), (149, 16)]);
        assert_eq!(trimmed_tiles(&tilings, 4, 1), [(0, 64), (85, 64)]);
        assert!(trimmed_tiles(&tilings, 4, 0).is_empty());
        assert!(trimmed_tiles(&[packed, packed], 2, 1).is_empty(), "packed mips have no tiles of their own");
    }

    /// Budget: a view of a texture without its top mips never reads below the mips it has,
    /// whatever the title asked for; the default view too.
    #[test]
    fn views_of_a_trimmed_texture_are_clamped_to_its_mips() {
        use windows::Win32::Graphics::Dxgi::Common::*;
        let desc = D3D12_RESOURCE_DESC { Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D, Width: 2048, Height: 2048, DepthOrArraySize: 1, MipLevels: 12, Format: DXGI_FORMAT_BC7_UNORM, SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 }, ..Default::default() };
        unsafe {
            let v = clamp_view(&desc, None, 2).unwrap();
            assert_eq!((v.ViewDimension, v.Format, v.Shader4ComponentMapping), (D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_BC7_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING));
            assert_eq!((v.Anonymous.Texture2D.MostDetailedMip, v.Anonymous.Texture2D.MipLevels, v.Anonymous.Texture2D.ResourceMinLODClamp), (0, u32::MAX, 2.0));
            let array = clamp_view(&D3D12_RESOURCE_DESC { DepthOrArraySize: 6, ..desc }, None, 1).unwrap();
            assert_eq!((array.ViewDimension, array.Anonymous.Texture2DArray.ArraySize, array.Anonymous.Texture2DArray.ResourceMinLODClamp), (D3D12_SRV_DIMENSION_TEXTURE2DARRAY, u32::MAX, 1.0));
            // the title's own view keeps what it says, with the clamp raised – never lowered
            let mut own = D3D12_SHADER_RESOURCE_VIEW_DESC { Format: DXGI_FORMAT_BC7_UNORM_SRGB, ViewDimension: D3D12_SRV_DIMENSION_TEXTURECUBE, Shader4ComponentMapping: 0x1234, ..Default::default() };
            own.Anonymous.TextureCube = D3D12_TEXCUBE_SRV { MostDetailedMip: 1, MipLevels: 5, ResourceMinLODClamp: 0.5 };
            let v = clamp_view(&desc, Some(own), 2).unwrap();
            assert_eq!((v.Format, v.Shader4ComponentMapping, v.Anonymous.TextureCube.MostDetailedMip, v.Anonymous.TextureCube.MipLevels, v.Anonymous.TextureCube.ResourceMinLODClamp), (DXGI_FORMAT_BC7_UNORM_SRGB, 0x1234, 1, 5, 2.0));
            own.Anonymous.TextureCube.ResourceMinLODClamp = 4.0;
            assert_eq!(clamp_view(&desc, Some(own), 2).unwrap().Anonymous.TextureCube.ResourceMinLODClamp, 4.0);
            // a view that is not of a 2D texture or cube is left as it is
            let mut buffer = D3D12_SHADER_RESOURCE_VIEW_DESC { ViewDimension: D3D12_SRV_DIMENSION_BUFFER, ..Default::default() };
            buffer.Anonymous.Buffer = D3D12_BUFFER_SRV { FirstElement: 7, NumElements: 9, StructureByteStride: 4, ..Default::default() };
            let v = clamp_view(&desc, Some(buffer), 2).unwrap();
            assert_eq!((v.Anonymous.Buffer.FirstElement, v.Anonymous.Buffer.NumElements, v.Anonymous.Buffer.StructureByteStride), (7, 9, 4));
        }
    }

    /// Budget: the local video memory the pools keep the process under – 90 % of what the
    /// adapter grants, or `XCL_VRAM_BUDGET_MB`, never more than the adapter grants.
    #[test]
    fn the_budget_of_local_video_memory_is_the_adapters_or_the_switchs() {
        const MIB64: u64 = 1 << 20;
        assert_eq!(local_limit(15_000 * MIB64, None), 13_500 * MIB64);
        assert_eq!(local_limit(15_000 * MIB64, Some(14_500 * MIB64)), 14_500 * MIB64);
        assert_eq!(local_limit(15_000 * MIB64, Some(20_000 * MIB64)), 15_000 * MIB64);
        // 14.5 GB asked for, 12 GB in use of which 6 GB of pool heaps: 2.5 GB more for them
        assert_eq!(adapter_cap(15_000 * MIB64, Some(14_500 * MIB64), 12_000 * MIB64, 6_000 * MIB64), 8_500 * MIB64);
        assert_eq!(adapter_cap(15_000 * MIB64, Some(14_500 * MIB64), 19_700 * MIB64, 6_000 * MIB64), 6_000 * MIB64, "past it: no more");
    }

    /// The first hardware adapter's device with tiled resources, as the page-budget tests use.
    unsafe fn hardware_device() -> Option<ID3D12Device> {
        use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
        use windows::Win32::Graphics::Dxgi::*;
        let factory: IDXGIFactory4 = CreateDXGIFactory1().expect("DXGI factory");
        let adapter = factory.EnumAdapters1(0).ok()?;
        if adapter.GetDesc1().is_ok_and(|d| d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE.0 as u32 != 0) {
            return None;
        }
        let mut dev: Option<ID3D12Device> = None;
        let _ = D3D12CreateDevice(&adapter, D3D_FEATURE_LEVEL_11_0, &mut dev);
        let dev = dev.filter(|d| tier2(d))?;
        super::super::real::adopt(dev.as_raw(), super::super::real::Kind::Device);
        Some(dev)
    }

    /// Budget, on the GPU: a large block-compressed texture placed at the budget takes no page
    /// for its two top mips, is known as trimmed for its views, and stays so when it is mapped
    /// again; the same texture placed under the budget, and one that is not block-compressed,
    /// are whole.
    #[test]
    fn a_texture_placed_at_the_budget_takes_no_pages_for_its_top_mips() {
        let _vram_guard = VRAM_TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        use windows::Win32::Graphics::Dxgi::Common::*;
        unsafe {
            let Some(dev) = hardware_device() else { return };
            let riid = &ID3D12Resource::IID as *const windows::core::GUID as *const windows_sys::core::GUID;
            let place_at = |va: usize, format: DXGI_FORMAT| {
                let desc = D3D12_RESOURCE_DESC { Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D, Width: 2048, Height: 2048, DepthOrArraySize: 1, MipLevels: 12, Format: format, SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 }, ..Default::default() };
                let mut p = std::ptr::null_mut();
                let hr = place_pages(dev.as_raw(), va, &desc, D3D12_RESOURCE_STATE_COMMON.0 as u32, std::ptr::null(), Some(32 << 20), riid, &mut p);
                (hr.is_some_and(|hr| hr >= 0) && !p.is_null()).then(|| ID3D12Resource::from_raw(p))
            };
            let dev_key = dev.as_raw() as usize;
            let set_over = |over: bool| {
                if let Some(p) = PAGES.lock().unwrap().get_mut(&dev_key).and_then(Option::as_mut) {
                    p.over = over;
                }
            };
            let Some(whole) = place_at(0x62_0000_0000, DXGI_FORMAT_BC7_UNORM) else {
                crate::gpu::recomp::runner::skip_test("no reserved textures");
                return;
            };
            set_over(false);
            let Some(under) = place_at(0x62_1000_0000, DXGI_FORMAT_BC7_UNORM) else { return };
            set_over(true);
            let (Some(trimmed), Some(rgba)) = (place_at(0x62_2000_0000, DXGI_FORMAT_BC7_UNORM), place_at(0x62_4000_0000, DXGI_FORMAT_R8G8B8A8_UNORM)) else {
                set_over(false);
                return;
            };
            set_over(false);
            let key = |r: &ID3D12Resource| r.as_raw() as usize;
            let held = |r: &ID3D12Resource| {
                let g = PAGES.lock().unwrap();
                let t = &g.get(&dev_key).unwrap().as_ref().unwrap().targets[&key(r)];
                (t.total as usize, t.used.len() + t.used_extra.len(), t.trimmed, t.skip.iter().map(|s| s.1 as usize).sum::<usize>())
            };
            let (total, pages, mips, left_out) = held(&under);
            assert_eq!((pages, mips, left_out), (total, 0, 0), "under the budget: every tile has a page");
            assert_eq!(trimmed_mips(key(&under)), 0);
            let (total, pages, mips, left_out) = held(&trimmed);
            // BC7 2048x2048: 4 MiB and 1 MiB for mips 0 and 1 – 64 and 16 tiles
            assert_eq!((mips, left_out, pages), (2, 80, total - 80), "at the budget: no page for mips 0 and 1");
            assert_eq!(trimmed_mips(key(&trimmed)), 2, "its views are clamped");
            let (total, pages, mips, _) = held(&rgba);
            assert_eq!((pages, mips), (total, 0), "not block-compressed: whole");
            // unmapped and mapped again (an eviction, a superseding placement): trimmed still
            unmap_superseded(&trimmed);
            assert_eq!(held(&trimmed).1, 0);
            assert!(remap_if_unmapped(&trimmed));
            assert_eq!(held(&trimmed), (total_of(&trimmed), total_of(&trimmed) - 80, 2, 80));
            assert!(dev.GetDeviceRemovedReason().is_ok(), "the device took the partial mappings");
            let k = key(&trimmed);
            drop((whole, under, trimmed, rgba));
            assert_eq!(trimmed_mips(k), 0, "released: forgotten");
        }
    }

    unsafe fn total_of(r: &ID3D12Resource) -> usize {
        let mut dev: Option<ID3D12Device> = None;
        r.GetDevice(&mut dev).unwrap();
        let mut total = 0u32;
        dev.unwrap().GetResourceTiling(r, Some(&mut total), None, None, None, 0, std::ptr::null_mut());
        total as usize
    }

    /// Budget, on the GPU: at the budget a pool heap whose textures nothing has used for
    /// `DEMOTE_AGE` gets a low residency priority; it has its normal one again when a texture
    /// on it is in a shader-visible heap, or the pools are under the budget. A GPU-owned target
    /// always counts as in use.
    #[test]
    fn idle_pool_heaps_get_a_low_priority_at_the_budget() {
        let _vram_guard = VRAM_TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        use windows::Win32::Graphics::Dxgi::Common::*;
        unsafe {
            let Some(dev) = hardware_device() else { return };
            let desc = D3D12_RESOURCE_DESC { Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D, Width: 512, Height: 512, DepthOrArraySize: 1, MipLevels: 1, Format: DXGI_FORMAT_R8G8B8A8_UNORM, SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 }, ..Default::default() };
            let riid = &ID3D12Resource::IID as *const windows::core::GUID as *const windows_sys::core::GUID;
            let mut p = std::ptr::null_mut();
            let hr = place_pages(dev.as_raw(), 0x63_0000_0000, &desc, D3D12_RESOURCE_STATE_COMMON.0 as u32, std::ptr::null(), Some(1 << 20), riid, &mut p);
            if !hr.is_some_and(|hr| hr >= 0) || p.is_null() {
                crate::gpu::recomp::runner::skip_test("no reserved textures");
                return;
            }
            let tex = ID3D12Resource::from_raw(p);
            let (key, dev_key) = (tex.as_raw() as usize, dev.as_raw() as usize);
            let now = Instant::now();
            let looked = HEAPS_LOOKED.swap(true, std::sync::atomic::Ordering::Relaxed);
            {
                let mut g = PAGES.lock().unwrap();
                let pages = g.get_mut(&dev_key).unwrap().as_mut().unwrap();
                // nothing on the pages was used for a minute
                for t in pages.targets.values_mut() {
                    t.touched = now - Duration::from_secs(60);
                    t.seen = Seen::None;
                }
                let heap = pages.pages[&pages.targets[&key].used[0]].tile.0 as usize;
                let demoted = |pages: &Pages| pages.pools.heaps[heap].as_ref().unwrap().demoted;
                pages.over = false;
                pages.demote(&dev, now);
                assert!(!demoted(pages), "under the budget nothing is demoted");
                pages.over = true;
                pages.demote(&dev, now);
                assert!(demoted(pages), "at the budget: a heap nothing uses");
                assert!(budget_counts_of(pages) >= 1);
                pages.targets.get_mut(&key).unwrap().seen = Seen::Visible;
                pages.demote(&dev, now);
                assert!(!demoted(pages), "its texture is in a shader-visible heap: normal again");
                pages.targets.get_mut(&key).unwrap().seen = Seen::None;
                pages.demote(&dev, now);
                assert!(demoted(pages));
                pages.targets.get_mut(&key).unwrap().gpu_owned = true;
                pages.demote(&dev, now);
                assert!(!demoted(pages), "a GPU-owned target is in use for all XCL sees");
                pages.targets.get_mut(&key).unwrap().gpu_owned = false;
                pages.demote(&dev, now);
                assert!(demoted(pages));
                pages.targets.get_mut(&key).unwrap().touched = now;
                pages.demote(&dev, now);
                assert!(!demoted(pages), "used just now");
                pages.targets.get_mut(&key).unwrap().touched = now - Duration::from_secs(60);
                pages.demote(&dev, now);
                assert!(demoted(pages));
                pages.over = false;
                pages.demote(&dev, now);
                assert!(!demoted(pages), "under the budget again: normal again");
                assert!(dev.GetDeviceRemovedReason().is_ok());
            }
            HEAPS_LOOKED.store(looked, std::sync::atomic::Ordering::Relaxed);
            drop(tex);
        }
    }

    fn budget_counts_of(pages: &Pages) -> usize {
        pages.pools.heaps.iter().flatten().filter(|h| h.demoted).count()
    }
}
