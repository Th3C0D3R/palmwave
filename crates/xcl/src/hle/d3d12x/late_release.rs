//! The title's last `Release` of a resource, put off until the GPU is past the work in flight.
//!
//! D3D12 on the PC asks that an object outlives the GPU work that uses it. A console title
//! keeps to that by its own count of finished frames – but XCL's submits are not the title's:
//! texture uploads go out with a later submit, tile mappings on a queue of XCL's own. A retail
//! title's final `Release` of a resource then came while a queue still had work on it (debug
//! layer: "CORRUPTION: … is referenced by GPU operations in-flight", id 921).
//!
//! The last reference is held here instead. Each batch of held resources gets a fence value
//! signalled on every queue with work on it; the batch goes once all of them have passed it.
use std::collections::{HashMap, VecDeque};
use std::ffi::c_void;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use windows::core::Interface;
use windows::Win32::Graphics::Direct3D12::*;

/// a batch is not closed more often than this (a `Signal` a queue each time)
const SEAL_EVERY: Duration = Duration::from_millis(10);
/// how often the thread here looks for batches that can go
const TICK: Duration = Duration::from_millis(4);

struct Batch {
    dead: Vec<usize>,
    /// (fence, value) a queue: all reached = nothing submitted before the batch closed is left
    marks: Vec<(ID3D12Fence, u64)>,
}

#[derive(Default)]
struct Held {
    /// queue → its fence here and the last value signalled on it
    fences: HashMap<usize, (ID3D12Fence, u64)>,
    open: Vec<usize>,
    sealed: VecDeque<Batch>,
    last_seal: Option<Instant>,
}

// (the interface pointers are only used under the lock)
unsafe impl Send for Held {}

impl Held {
    fn is_empty(&self) -> bool {
        self.open.is_empty() && self.sealed.is_empty()
    }

    /// Close the open batch (at most every `SEAL_EVERY`, or `seal_now`) with a fence value on each of `queues`,
    /// and take out the batches every queue is past: the resources to release now.
    unsafe fn drain(&mut self, queues: &[ID3D12CommandQueue], seal_now: bool) -> Vec<usize> {
        if !self.open.is_empty() && (seal_now || self.last_seal.is_none_or(|t| t.elapsed() >= SEAL_EVERY)) {
            let mut marks = Vec::with_capacity(queues.len());
            let mut ok = true;
            for q in queues {
                let key = q.as_raw() as usize;
                if !self.fences.contains_key(&key) {
                    let mut dev: Option<ID3D12Device> = None;
                    let made = q.GetDevice(&mut dev).ok().and(dev).and_then(|d| d.CreateFence::<ID3D12Fence>(0, D3D12_FENCE_FLAG_NONE).ok());
                    let Some(f) = made else {
                        ok = false;
                        break;
                    };
                    self.fences.insert(key, (f, 0));
                }
                let (fence, value) = self.fences.get_mut(&key).unwrap();
                if q.Signal(&*fence, *value + 1).is_err() {
                    ok = false;
                    break;
                }
                *value += 1;
                marks.push((fence.clone(), *value));
            }
            // (no fence or no signal: the device is gone – the batch stays open, and held)
            if ok {
                self.last_seal = Some(Instant::now());
                let dead = std::mem::take(&mut self.open);
                self.sealed.push_back(Batch { dead, marks });
            }
        }
        let mut gone = Vec::new();
        while self.sealed.front().is_some_and(|b| b.marks.iter().all(|(f, v)| f.GetCompletedValue() >= *v)) {
            gone.extend(self.sealed.pop_front().unwrap().dead);
        }
        gone
    }
}

static HELD: Mutex<Option<Held>> = Mutex::new(None);

/// Resources whose last reference came while another thread held `HELD`, for its next holder
/// (`held`). `hold` runs inside a resource's Release, which D3D12 also makes itself while it
/// holds a lock of its own (resetting a command list), and the holder of `HELD` can be inside
/// D3D12 (`Held::drain` makes fences and signals the queues): waiting there for `HELD` could
/// deadlock both threads, as the sparse lock did (run P66).
static WAITING: Mutex<Vec<usize>> = Mutex::new(Vec::new());

/// `HELD`, with the resources left in `WAITING` taken in.
fn held() -> std::sync::MutexGuard<'static, Option<Held>> {
    let mut g = HELD.lock().unwrap();
    take_waiting(&mut g);
    g
}

fn take_waiting(g: &mut Option<Held>) {
    let waiting = std::mem::take(&mut *WAITING.lock().unwrap());
    if !waiting.is_empty() {
        g.get_or_insert_with(Held::default).open.extend(waiting);
    }
}

/// Whether resources are held. `wait` false: never waits for `HELD`, and says yes when another
/// thread holds it.
fn pending(wait: bool) -> bool {
    if !WAITING.lock().unwrap().is_empty() {
        return true;
    }
    if wait {
        return held().as_ref().is_some_and(|h| !h.is_empty());
    }
    match HELD.try_lock() {
        Ok(g) => g.as_ref().is_some_and(|h| !h.is_empty()),
        Err(_) => true,
    }
}

/// Whether last releases are held (`XCL_LATE_RELEASE=0` turns it off).
pub fn enabled() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| !std::env::var("XCL_LATE_RELEASE").is_ok_and(|v| v.trim() == "0"))
}

/// how long a last `Release` itself waits for queues with nothing to do to say so
const IDLE_WAIT: Duration = Duration::from_micros(300);

/// `res` (an adopted resource) holds one reference that is now this module's. With no work in
/// flight it goes before this returns (the queues pass a fence value at once): a title that
/// frees the memory under a resource right after its last `Release` finds it unpinned, as
/// before. Otherwise the thread here lets it go. Never waits for a lock another thread holds
/// (`WAITING`): what it cannot do now, the thread here does.
pub unsafe fn hold(res: usize) {
    match HELD.try_lock() {
        Ok(mut g) => {
            take_waiting(&mut g);
            g.get_or_insert_with(Held::default).open.push(res);
        }
        Err(std::sync::TryLockError::WouldBlock) => WAITING.lock().unwrap().push(res),
        Err(std::sync::TryLockError::Poisoned(e)) => panic!("{e}"),
    }
    let start = Instant::now();
    drain(true, false);
    while start.elapsed() < IDLE_WAIT && pending(false) {
        std::hint::spin_loop();
        drain(false, false);
    }
    if !pending(false) {
        return;
    }
    // A thread of its own lets the held ones go: the title may release a resource and then
    // wait for its memory without another submit (an imported heap pins the pages under its
    // resources – unmapping them stood still while one was held "until the next submit").
    static THREAD: std::sync::Once = std::sync::Once::new();
    THREAD.call_once(|| {
        let _ = std::thread::Builder::new().name("xcl-late-release".into()).spawn(|| loop {
            std::thread::sleep(TICK);
            unsafe { drain_known(false) };
        });
    });
}

/// Release what every queue XCL knows of is past: the title's, the tile-mapping one, the
/// upload ones.
pub unsafe fn drain_known(seal_now: bool) {
    drain(seal_now, true);
}

/// `drain_known`. `wait` false (inside a Release, `hold`): no lock another thread holds is
/// waited for – the held resources then wait for the next drain.
unsafe fn drain(seal_now: bool, wait: bool) {
    if !pending(wait) {
        return;
    }
    let mut queues: Vec<ID3D12CommandQueue> = Vec::new();
    for raw in super::present::known_queues() {
        let raw = raw as *mut c_void;
        if let Some(q) = ID3D12CommandQueue::from_raw_borrowed(&raw) {
            queues.push(q.clone());
        }
    }
    queues.extend(super::sparse::queue());
    let Some(uploads) = super::title_textures::upload_queues(wait) else { return };
    queues.extend(uploads);
    let mut g = if wait {
        held()
    } else {
        let Ok(mut g) = HELD.try_lock() else { return };
        take_waiting(&mut g);
        g
    };
    let gone = match g.as_mut() {
        Some(h) => h.drain(&queues, seal_now),
        None => return,
    };
    drop(g);
    // (outside the lock: a release may end other objects, whose releases come back here)
    for res in gone {
        let f: unsafe extern "system" fn(*mut c_void) -> u32 = std::mem::transmute(super::real::orig_fn(res as *mut c_void, 2));
        f(res as *mut c_void);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use windows::Win32::Graphics::Dxgi::Common::DXGI_SAMPLE_DESC;

    /// A resource's last Release can come inside D3D12 while another thread holds `HELD` and is
    /// inside D3D12 itself: `hold` must not wait for `HELD` then (run P66 deadlocked so, through
    /// the sparse lock). It leaves the resource for the lock's next holder.
    #[test]
    fn hold_does_not_wait_for_the_held_lock() {
        // no real resource has this address: it is taken out again before anything can drain it
        // (only a holder of `HELD` takes `WAITING` in)
        const RES: usize = 0x7E57_401D_0000;
        let held = HELD.lock().unwrap();
        let (tx, rx) = std::sync::mpsc::channel();
        let t = std::thread::spawn(move || {
            unsafe { hold(RES) };
            tx.send(()).unwrap();
        });
        let returned = rx.recv_timeout(Duration::from_secs(5)).is_ok();
        let waiting = {
            let mut w = WAITING.lock().unwrap();
            let had = w.contains(&RES);
            w.retain(|&r| r != RES);
            had
        };
        drop(held);
        assert!(returned, "hold waited for the lock another thread held");
        t.join().unwrap();
        assert!(waiting, "the resource was not left for the lock's next holder");
    }

    /// A held resource stays while a queue has not got past the work submitted before it was
    /// released, and goes once the queue has.
    #[test]
    fn a_held_resource_goes_when_the_queue_is_past_its_batch() {
        unsafe {
            let Ok(dev) = crate::gpu::agility::create_device(None, windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0) else {
                return crate::gpu::recomp::runner::skip_test("no GPU");
            };
            let queue: ID3D12CommandQueue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC { Type: D3D12_COMMAND_LIST_TYPE_DIRECT, ..Default::default() }).unwrap();
            // the queue stands still, as with work in flight: it waits for a fence nobody has signalled
            let gate: ID3D12Fence = dev.CreateFence(0, D3D12_FENCE_FLAG_NONE).unwrap();
            queue.Wait(&gate, 1).unwrap();
            let mut res: Option<ID3D12Resource> = None;
            let desc = D3D12_RESOURCE_DESC {
                Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
                Width: 4096,
                Height: 1,
                DepthOrArraySize: 1,
                MipLevels: 1,
                SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
                Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                ..Default::default()
            };
            dev.CreateCommittedResource(&D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() }, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, None, &mut res).unwrap();
            let raw = res.unwrap().into_raw() as usize;

            let mut held = Held::default();
            held.open.push(raw);
            let queues = [queue.clone()];
            assert!(held.drain(&queues, false).is_empty(), "the queue has not passed the batch's fence value");
            std::thread::sleep(Duration::from_millis(100));
            assert!(held.drain(&queues, false).is_empty(), "still in flight: still held");
            assert!(!held.is_empty());

            gate.Signal(1).unwrap();
            let start = Instant::now();
            let mut gone = Vec::new();
            while gone.is_empty() && start.elapsed() < Duration::from_secs(10) {
                std::thread::sleep(Duration::from_millis(5));
                gone = held.drain(&queues, false);
            }
            assert_eq!(gone, vec![raw], "released once the queue is past the batch");
            assert!(held.is_empty());
            drop(ID3D12Resource::from_raw(raw as *mut c_void));
        }
    }

    /// With no queue at all nothing can be in flight: the next drain lets the batch go.
    #[test]
    fn without_queues_a_batch_goes_at_once() {
        let mut held = Held::default();
        held.open.push(0x1000);
        assert_eq!(unsafe { held.drain(&[], false) }, vec![0x1000]);
        assert!(held.is_empty());
    }
}
