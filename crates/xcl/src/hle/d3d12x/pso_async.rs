//! Non-blocking pipeline creation. Translating a console shader takes milliseconds; compiling
//! the HLSL with DXC takes 0.2 s to seconds, and a title creates thousands of pipelines while
//! it loads. When a pipeline's DXIL is not in the disk cache yet, the title gets a stand-in
//! pipeline (shaders that draw nothing) at once and the real one is built on a worker; binding
//! (`SetPipelineState`, console list replay, the two-pass lookups) uses the real one as soon
//! as it is there. A draw waits for its graphics pipeline only so long (`resolve_for_draw`),
//! then draws nothing until it is built; a compute pipeline is waited for at its first bind
//! (a skipped dispatch loses what it computes). All these waits between two presents share
//! one budget (`FrameBudget`): once it is spent, draws and dispatches go with the stand-in.
//! `XCL_ASYNC_PSO=0` builds every pipeline in the call, as before.

use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{mpsc, Arc, Condvar, Mutex, OnceLock};
use std::time::{Duration, Instant};

struct Pending {
    /// the build, until a worker – or a bind that cannot wait for the queue – takes it
    job: Mutex<Option<Job>>,
    /// the build went to the workers (a lazy one goes on its first `resolve`)
    sent: std::sync::atomic::AtomicBool,
    /// the real pipeline state (raw pointer), 0 while building or when building failed
    real: AtomicUsize,
    done: Mutex<bool>,
    cv: Condvar,
    compute: bool,
    /// of its shaders (`used`); 0 = none
    key: u64,
    /// it was used (`used::note` is told once)
    used: std::sync::atomic::AtomicBool,
    /// the stand-in was released before the real pipeline was built
    orphan: std::sync::atomic::AtomicBool,
    /// creation order (`ahead`)
    seq: u64,
    /// built by an `ahead` thread and not asked for by the title since
    ahead: std::sync::atomic::AtomicBool,
    /// the `preload` thread building it (a handle to raise its priority by), 0 = none
    builder: AtomicUsize,
    /// a title thread waits for it (`title_waits`)
    waited: std::sync::atomic::AtomicBool,
}

impl Pending {
    fn note_use(&self) {
        if self.key != 0 && !self.used.swap(true, Ordering::Relaxed) {
            used::note(self.key);
        }
    }

    /// A title thread waits for this build (called with `done` held, before it is done). A
    /// `preload` thread building it goes on at normal priority – the title does not wait on a
    /// thread every other one runs before – and takes no compile slot (`preload_wanted`).
    fn title_waits(&self) {
        // (SeqCst: the preload thread stores `builder`, then reads `waited`)
        self.waited.store(true, Ordering::SeqCst);
        let h = self.builder.load(Ordering::SeqCst);
        if h != 0 {
            set_priority(h, windows_sys::Win32::System::Threading::THREAD_PRIORITY_NORMAL);
            static N: AtomicUsize = AtomicUsize::new(0);
            let n = N.fetch_add(1, Ordering::Relaxed) + 1;
            if n.is_power_of_two() {
                crate::xlog_info!("D3D12.X: {n} waits of the title for a pipeline still being built ahead of its first use (that build goes on at normal priority)");
            }
        }
    }
}

/// Thread `h` (a handle with THREAD_SET_LIMITED_INFORMATION) runs at `priority` now.
fn set_priority(h: usize, priority: i32) {
    unsafe { windows_sys::Win32::System::Threading::SetThreadPriority(h as windows_sys::Win32::Foundation::HANDLE, priority) };
}

/// stand-in handed to the title → its real pipeline
static PENDING: Mutex<Option<HashMap<usize, Arc<Pending>>>> = Mutex::new(None);

/// The build is done (`real`: its pipeline, 0 = failed).
fn complete(p: &Pending, real: usize) {
    if real == 0 {
        FAILED.fetch_add(1, Ordering::Relaxed);
    } else {
        BUILT.fetch_add(1, Ordering::Relaxed);
    }
    let orphan = {
        let mut done = p.done.lock().unwrap();
        let orphan = p.orphan.load(Ordering::Acquire);
        if !orphan {
            p.real.store(real, Ordering::Release);
        }
        *done = true;
        p.cv.notify_all();
        orphan
    };
    // outside `done`: the Release override takes PENDING (`counts` takes PENDING, then `done`)
    if orphan {
        release(real);
    }
}
static BUILT: AtomicUsize = AtomicUsize::new(0);
static FAILED: AtomicUsize = AtomicUsize::new(0);

pub fn enabled() -> bool {
    std::env::var("XCL_ASYNC_PSO").map_or(true, |v| v != "0")
}

/// Builds the real pipeline: its raw pointer, 0 = failed.
pub type Job = Box<dyn FnOnce() -> usize + Send>;

fn queue() -> &'static Mutex<mpsc::Sender<Arc<Pending>>> {
    static Q: OnceLock<Mutex<mpsc::Sender<Arc<Pending>>>> = OnceLock::new();
    Q.get_or_init(|| {
        let (tx, rx) = mpsc::channel::<Arc<Pending>>();
        let rx = Arc::new(Mutex::new(rx));
        // translation recurses deeply on big programs; DXC itself runs on its own pool
        let n = crate::gpu::recomp::dxc::compile_threads();
        for k in 0..n {
            let rx = rx.clone();
            let _ = std::thread::Builder::new().name(format!("xcl-pso-{k}")).stack_size(64 << 20).spawn(move || loop {
                let next = rx.lock().unwrap().recv();
                let Ok(p) = next else { return };
                // (None: a bind took it and builds it itself)
                let job = p.job.lock().unwrap().take();
                if let Some(job) = job {
                    complete(&p, job());
                }
            });
        }
        Mutex::new(tx)
    })
}

/// Release a pipeline state XCL owns (raw pointer, 0 = none).
pub fn release(p: usize) {
    if p != 0 {
        unsafe { drop(<windows::Win32::Graphics::Direct3D12::ID3D12PipelineState as windows::core::Interface>::from_raw(p as *mut std::ffi::c_void)) };
    }
}

/// `standin` went to the title; `job` builds the real pipeline on a worker.
pub fn submit(standin: usize, compute: bool, job: Job) {
    submit_as(standin, compute, job, true, 0);
}

/// Lazy pipeline creation (`XCL_LAZY_PSO=0`: off): a title's pipelines with console shaders get
/// their stand-in when made and are built when first used – the first bind of a graphics one
/// sends it to the workers, a compute one is built at its first bind. A retail game makes
/// ~51 000 pipelines in its first 30 s and uses few; each one built stays in the driver
/// (~400 KB), and the process ran out of commit.
pub fn lazy() -> bool {
    static L: OnceLock<bool> = OnceLock::new();
    *L.get_or_init(|| enabled() && std::env::var("XCL_LAZY_PSO").map_or(true, |v| v != "0"))
}

/// `submit`, sent to the workers only on its first `resolve` (`lazy`) – or now, at a low
/// priority, when an earlier run used a pipeline of these shaders (`key`, 0 = none; `used`).
pub fn submit_lazy(standin: usize, compute: bool, job: Job, key: u64) {
    submit_as(standin, compute, job, false, key);
}

fn submit_as(standin: usize, compute: bool, job: Job, send: bool, key: u64) {
    let p = Arc::new(Pending {
        job: Mutex::new(Some(job)),
        sent: std::sync::atomic::AtomicBool::new(send),
        real: AtomicUsize::new(0),
        done: Mutex::new(false),
        cv: Condvar::new(),
        compute,
        key,
        used: Default::default(),
        orphan: Default::default(),
        seq: NEXT_SEQ.fetch_add(1, Ordering::Relaxed),
        ahead: Default::default(),
        builder: AtomicUsize::new(0),
        waited: Default::default(),
    });
    PENDING.lock().unwrap().get_or_insert_with(HashMap::new).insert(standin, p.clone());
    if compute && !send && ahead_threads() > 0 {
        LAZY_COMPUTE.lock().unwrap().insert(p.seq, Arc::downgrade(&p));
        start_ahead();
    }
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| {
        crate::xlog_info!("D3D12.X: pipelines whose shaders are not compiled yet get a stand-in and are built in the background");
    });
    if send {
        let _ = queue().lock().unwrap().send(p);
    } else if used::known(key) {
        if let Some(q) = preload() {
            q.list.lock().unwrap().push_back(p);
            q.cv.notify_one();
        }
    }
}

/// The pipelines a title used, by a hash of their shaders, kept between runs
/// (`used-pipelines.txt` beside the DXIL cache, a key a line).
///
/// A lazy pipeline is built at its first use: until then a graphics one draws nothing, and a
/// compute one is waited for on the title's thread. A retail title's first frames of its world
/// use hundreds of pipelines for the first time at once. The pipelines of an earlier run are
/// built while the title loads instead (`preload`): measured on a build that skips a dispatch
/// instead of waiting for its pipeline, ~2 170 dispatches were skipped in those frames without
/// this and 94 – 150 with it.
mod used {
    use std::collections::HashSet;
    use std::io::Write;
    use std::sync::{Mutex, OnceLock, RwLock};

    struct Used {
        earlier: RwLock<HashSet<u64>>,
        now: Mutex<(HashSet<u64>, Option<std::fs::File>)>,
    }

    /// The keys in a `used-pipelines.txt` (lines that are not a hexadecimal key are passed over).
    pub fn parse(text: &str) -> HashSet<u64> {
        text.lines().filter_map(|l| u64::from_str_radix(l.trim(), 16).ok()).filter(|&k| k != 0).collect()
    }

    fn get() -> &'static Used {
        static U: OnceLock<Used> = OnceLock::new();
        U.get_or_init(|| {
            // (a test neither reads nor writes the user's file)
            if cfg!(test) {
                return Used { earlier: RwLock::new(HashSet::new()), now: Mutex::new((HashSet::new(), None)) };
            }
            let path = crate::gpu::recomp::dxc::cache_dir().join("used-pipelines.txt");
            let earlier = parse(&std::fs::read_to_string(&path).unwrap_or_default());
            if !earlier.is_empty() {
                crate::xlog_info!("D3D12.X: the pipelines of {} shader sets an earlier run used are built ahead of their first use", earlier.len());
            }
            if let Some(dir) = path.parent() {
                let _ = std::fs::create_dir_all(dir);
            }
            let file = std::fs::OpenOptions::new().create(true).append(true).open(&path).ok();
            Used { now: Mutex::new((earlier.clone(), file)), earlier: RwLock::new(earlier) }
        })
    }

    /// An earlier run used a pipeline of these shaders.
    pub fn known(key: u64) -> bool {
        key != 0 && get().earlier.read().unwrap().contains(&key)
    }

    /// A pipeline of these shaders is used now: kept for the next run.
    pub fn note(key: u64) {
        if key == 0 {
            return;
        }
        let mut g = get().now.lock().unwrap();
        if g.0.insert(key) {
            if let Some(f) = g.1.as_mut() {
                let _ = writeln!(f, "{key:016x}");
            }
        }
    }

    /// (tests) This run used a pipeline of these shaders.
    #[cfg(test)]
    pub fn noted(key: u64) -> bool {
        get().now.lock().unwrap().0.contains(&key)
    }

    #[cfg(test)]
    pub fn earlier_run_used(key: u64) {
        get().earlier.write().unwrap().insert(key);
    }
}

/// The workers that build the pipelines of an earlier run ahead of their first use
/// (`XCL_PSO_PRELOAD=0`: off). Apart from the ones a bind sends to (`queue`) and at the lowest
/// priority: a pipeline that is bound is not held up by the ones made ahead (whoever takes
/// `job` first builds it), and the title's own threads are not either. Each builds one pipeline
/// at a time, so they send DXC no more compiles at once than there are of them; their big
/// driver compiles have slots of their own (`real::admit_big`). When a title thread waits for a
/// pipeline one of them builds, that build goes on at normal priority and without waiting for a
/// slot (`Pending::title_waits`).
struct Preload {
    list: Mutex<std::collections::VecDeque<Arc<Pending>>>,
    cv: Condvar,
}

thread_local! {
    /// the pipeline this thread builds ahead of its first use (`preload`)
    static PRELOADING: std::cell::RefCell<Option<Arc<Pending>>> = const { std::cell::RefCell::new(None) };
}

/// This thread builds a pipeline ahead of its first use (`preload`): it may wait for what a
/// build the title waits for does not (a slot for a big driver compile), unless `preload_wanted`.
pub fn preloading() -> bool {
    PRELOADING.with(|c| c.borrow().is_some())
}

/// A title thread waits for the pipeline this thread builds ahead of its first use.
pub fn preload_wanted() -> bool {
    PRELOADING.with(|c| c.borrow().as_ref().is_some_and(|p| p.waited.load(Ordering::SeqCst)))
}

fn preload() -> Option<&'static Preload> {
    static Q: OnceLock<Option<Preload>> = OnceLock::new();
    static STARTED: std::sync::Once = std::sync::Once::new();
    let q = Q
        .get_or_init(|| {
            if std::env::var("XCL_PSO_PRELOAD").is_ok_and(|v| v.trim() == "0") {
                return None;
            }
            Some(Preload { list: Mutex::new(Default::default()), cv: Condvar::new() })
        })
        .as_ref()?;
    STARTED.call_once(|| {
        let n = (crate::gpu::recomp::dxc::compile_threads() / 4).clamp(2, 4);
        for k in 0..n {
            let _ = std::thread::Builder::new().name(format!("xcl-pso-pre-{k}")).stack_size(64 << 20).spawn(move || {
                use windows_sys::Win32::System::Threading::{GetCurrentThread, GetCurrentThreadId, OpenThread, SetThreadPriority, THREAD_PRIORITY_LOWEST, THREAD_PRIORITY_NORMAL, THREAD_SET_LIMITED_INFORMATION};
                // (what a title thread waiting for a pipeline this one builds raises it by; never closed)
                let me = unsafe { OpenThread(THREAD_SET_LIMITED_INFORMATION, 0, GetCurrentThreadId()) } as usize;
                unsafe { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST) };
                static DONE: AtomicUsize = AtomicUsize::new(0);
                loop {
                    let p = {
                        let mut g = q.list.lock().unwrap();
                        loop {
                            if let Some(p) = g.pop_front() {
                                break p;
                            }
                            g = q.cv.wait(g).unwrap();
                        }
                    };
                    // (released by the title meanwhile)
                    if p.orphan.load(Ordering::Acquire) {
                        continue;
                    }
                    // (None: a bind took it and builds it itself.) `builder` is set with the job
                    // taken: a title thread that finds no job sees who builds it.
                    let job = {
                        let mut j = p.job.lock().unwrap();
                        let job = j.take();
                        if job.is_some() {
                            p.builder.store(me, Ordering::SeqCst);
                        }
                        job
                    };
                    if let Some(job) = job {
                        // (a title thread waits for it already)
                        if p.waited.load(Ordering::SeqCst) {
                            unsafe { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL) };
                        }
                        PRELOADING.with(|c| *c.borrow_mut() = Some(p.clone()));
                        let real = job();
                        PRELOADING.with(|c| *c.borrow_mut() = None);
                        complete(&p, real);
                        // (raised while a title thread waited: done now, so not raised again)
                        unsafe { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST) };
                        let n = DONE.fetch_add(1, Ordering::Relaxed) + 1;
                        if n.is_power_of_two() {
                            crate::xlog_info!("D3D12.X: {n} pipelines built ahead of their first use");
                        }
                    }
                }
            });
        }
    });
    Some(q)
}

#[cfg(test)]
mod used_tests {
    use super::*;

    #[test]
    fn the_keys_of_a_file_are_read_and_other_lines_passed_over() {
        let keys = used::parse("00000000000000ff\n\nnot a key\n  1a2b \n0\n");
        assert_eq!(keys.len(), 2);
        assert!(keys.contains(&0xff) && keys.contains(&0x1a2b));
    }

    /// A lazy pipeline of shaders an earlier run used is built without a bind; another one is
    /// built only at its first use, and that use is kept for the next run.
    #[test]
    fn a_pipeline_an_earlier_run_used_is_built_ahead_of_its_first_use() {
        let (known, unknown) = (0x7E57_7001usize, 0x7E57_7002usize);
        let (known_key, unknown_key) = (0x5EED_0001u64, 0x5EED_0002u64);
        used::earlier_run_used(known_key);
        submit_lazy(known, false, Box::new(|| 0x4321), known_key);
        submit_lazy(unknown, false, Box::new(|| 0x4322), unknown_key);
        let start = std::time::Instant::now();
        while is_pending(known) && start.elapsed() < std::time::Duration::from_secs(10) {
            std::thread::sleep(std::time::Duration::from_millis(5));
        }
        assert!(!is_pending(known), "built by the workers that build ahead, with no bind");
        assert_eq!(resolve(known), 0x4321);
        assert!(is_pending(unknown), "nobody used it and no earlier run did");
        assert!(!used::noted(unknown_key));
        assert_eq!(resolve_wait(unknown), 0x4322);
        assert!(used::noted(unknown_key) && used::noted(known_key), "a first use is kept for the next run");
        released(known);
        released(unknown);
    }

    /// A pipeline built ahead is built at the lowest priority until a title thread waits for it,
    /// then at normal priority (a title thread waiting on a thread that every busy one runs
    /// before waits for as long as the machine is busy), and the build is told it is wanted (it
    /// waits for no compile slot then). Once built, its thread is at the lowest priority again.
    #[test]
    fn a_title_thread_waiting_for_a_pipeline_built_ahead_raises_its_thread() {
        use windows_sys::Win32::Foundation::CloseHandle;
        use windows_sys::Win32::System::Threading::{GetCurrentThread, GetCurrentThreadId, GetThreadPriority, OpenThread, THREAD_PRIORITY_LOWEST, THREAD_PRIORITY_NORMAL, THREAD_QUERY_LIMITED_INFORMATION};
        // (thread, its priority, building ahead, wanted)
        fn seen() -> (u32, i32, bool, bool) {
            unsafe { (GetCurrentThreadId(), GetThreadPriority(GetCurrentThread()), preloading(), preload_wanted()) }
        }
        let (standin, key) = (0x7E57_7011usize, 0x5EED_0011u64);
        used::earlier_run_used(key);
        let (seen_tx, seen_rx) = mpsc::channel();
        let (go_tx, go_rx) = mpsc::channel::<()>();
        submit_lazy(
            standin,
            true,
            Box::new(move || {
                let _ = seen_tx.send(seen());
                let _ = go_rx.recv();
                let _ = seen_tx.send(seen());
                standin + 1
            }),
            key,
        );
        let long = Duration::from_secs(10);
        let (tid, priority, ahead, wanted) = seen_rx.recv_timeout(long).expect("built ahead, with no bind");
        assert_eq!((priority, ahead, wanted), (THREAD_PRIORITY_LOWEST, true, false));
        assert!(!preloading() && !preload_wanted(), "not on a title thread");
        let h = unsafe { OpenThread(THREAD_QUERY_LIMITED_INFORMATION, 0, tid) };
        assert!(!h.is_null());
        let priority_after = |want: i32| {
            let start = Instant::now();
            while unsafe { GetThreadPriority(h) } != want && start.elapsed() < long {
                std::thread::sleep(Duration::from_millis(5));
            }
            unsafe { GetThreadPriority(h) }
        };
        // a title thread waits for it (a compute bind): its thread is raised
        let title = std::thread::spawn(move || resolve_wait(standin));
        assert_eq!(priority_after(THREAD_PRIORITY_NORMAL), THREAD_PRIORITY_NORMAL, "raised while the title waits");
        go_tx.send(()).unwrap();
        assert_eq!(seen_rx.recv_timeout(long).unwrap(), (tid, THREAD_PRIORITY_NORMAL, true, true));
        assert_eq!(title.join().unwrap(), standin + 1);
        assert_eq!(priority_after(THREAD_PRIORITY_LOWEST), THREAD_PRIORITY_LOWEST, "back to the lowest once built");
        unsafe { CloseHandle(h) };
        released(standin);
    }
}

fn entry(pso: usize) -> Option<Arc<Pending>> {
    PENDING.lock().unwrap().as_ref()?.get(&pso).cloned()
}

/// The pipeline to use for `pso` now: the real one once built, else `pso` itself.
pub fn resolve(pso: usize) -> usize {
    let Some(p) = entry(pso) else { return pso };
    asked(&p);
    match p.real.load(Ordering::Acquire) {
        0 => {
            // a lazy one is wanted now: to the workers
            if !p.sent.swap(true, Ordering::AcqRel) {
                let _ = queue().lock().unwrap().send(p);
            }
            pso
        }
        r => r,
    }
}

/// `resolve`, waiting for a pipeline still being built – built here if no worker has started
/// it yet (a queue of a title's hundreds of pipelines must not hold up the one it binds now).
pub fn resolve_wait(pso: usize) -> usize {
    let Some(p) = entry(pso) else { return pso };
    wait_unbounded(pso, &p, frame_budget())
}

fn wait_unbounded(pso: usize, p: &Pending, budget: &FrameBudget) -> usize {
    asked(p);
    let start = Instant::now();
    let _waiting = TitleWait::new();
    let job = p.job.lock().unwrap().take();
    let here = job.is_some();
    if let Some(job) = job {
        complete(p, job());
    }
    let mut done = p.done.lock().unwrap();
    if !*done {
        p.title_waits();
    }
    while !*done {
        done = p.cv.wait(done).unwrap();
    }
    let real = p.real.load(Ordering::Acquire);
    drop(done);
    budget.charge(start.elapsed());
    note_long_wait(pso, p.compute, here, start.elapsed());
    match real {
        0 => pso,
        r => r,
    }
}

/// A title's thread held this long in one pipeline wait is logged: a retail game's watchdog
/// thread (title+0x2215a60) ends the process (`mov dword [0], 0` at title+0x2215c35, decoded in
/// #232) when it has woken 30 times in a row, a second apart, without a new heartbeat of the
/// main thread – 300 s only inside the game's long-operation scopes – and an NVIDIA driver
/// takes 10 - 17 s for one big translated pixel shader, minutes for a big compute program (#156).
const LONG_WAIT: Duration = Duration::from_secs(2);

fn note_long_wait(pso: usize, compute: bool, here: bool, took: Duration) {
    static N: AtomicUsize = AtomicUsize::new(0);
    if took >= LONG_WAIT && N.fetch_add(1, Ordering::Relaxed) < 32 {
        crate::xlog_warn!(
            "D3D12.X: thread '{}' waited {:.1} s for {} pipeline {pso:#x} ({})",
            std::thread::current().name().unwrap_or("title"),
            took.as_secs_f32(),
            if compute { "compute" } else { "graphics" },
            if here { "built in this thread" } else { "built on a worker" }
        );
    }
}

/// The longest one draw holds the title's thread for a graphics pipeline still being built
/// (`XCL_PSO_WAIT_MS`, default 4000; 0: no limit, as before). Past it the draw goes on with the
/// stand-in (draws nothing) and the pipeline is built on a worker. 4 s is a safety margin under
/// the title's watchdog (30 s without a main-thread heartbeat, see `LONG_WAIT`), not a measured
/// threshold; what all the waits of a frame may add up to is `FrameBudget`'s.
fn draw_wait_limit() -> Option<Duration> {
    static L: OnceLock<Option<Duration>> = OnceLock::new();
    *L.get_or_init(|| match std::env::var("XCL_PSO_WAIT_MS").ok().and_then(|v| v.trim().parse::<u64>().ok()) {
        Some(0) => None,
        Some(ms) => Some(Duration::from_millis(ms)),
        None => Some(Duration::from_secs(4)),
    })
}

/// The time title threads spent waiting for pipelines still being built since the last PresentX,
/// all threads together, and what it may come to. A retail game's main thread waits without a
/// timeout for its render thread, which waits for its command-list jobs, and those wait here:
/// eight pipelines at `draw_wait_limit` each already hold the main thread past its watchdog's
/// 30 s (`LONG_WAIT`). Once the budget is spent a draw goes with its stand-in at once – and so
/// does a compute bind or dispatch, unless `compute` is off.
struct FrameBudget {
    waited_us: AtomicU64,
    /// `XCL_PSO_FRAME_BUDGET_MS` (default 8000; 0: None, no budget – every wait as before)
    limit: Option<Duration>,
    /// `XCL_PSO_COMPUTE_BUDGET=1`: compute pipelines come under the budget too, and a dispatch past
    /// it is skipped. Off by default: what a skipped dispatch would have written is missing, and a
    /// retail title then read a corrupted heap object and crashed; off, compute pipelines are
    /// waited for without limit and built in the title's thread, as before.
    compute: bool,
}

/// the cumulative wait of a frame is logged each time it passes another one of these
const WAIT_LOG_STEP: Duration = Duration::from_secs(2);

impl FrameBudget {
    const fn new(limit: Option<Duration>, compute: bool) -> Self {
        Self { waited_us: AtomicU64::new(0), limit, compute }
    }

    fn waited(&self) -> Duration {
        Duration::from_micros(self.waited_us.load(Ordering::Relaxed))
    }

    /// What is left of the budget (None: there is none).
    fn left(&self) -> Option<Duration> {
        self.limit.map(|l| l.saturating_sub(self.waited()))
    }

    fn spent(&self) -> bool {
        self.left().is_some_and(|l| l.is_zero())
    }

    /// A thread waited `d` for a pipeline.
    fn charge(&self, d: Duration) {
        let d = d.as_micros() as u64;
        if d == 0 {
            return;
        }
        let before = self.waited_us.fetch_add(d, Ordering::Relaxed);
        let step = WAIT_LOG_STEP.as_micros() as u64;
        if (before + d) / step != before / step {
            static N: AtomicUsize = AtomicUsize::new(0);
            if N.fetch_add(1, Ordering::Relaxed) < 256 {
                crate::xlog_warn!(
                    "D3D12.X: title threads have waited {:.1} s for pipelines since the last present (budget {}; a retail game's watchdog ends the process after 30 s without a main-thread heartbeat)",
                    (before + d) as f32 / 1e6,
                    self.limit.map_or("none".to_string(), |l| format!("{} ms, XCL_PSO_FRAME_BUDGET_MS", l.as_millis()))
                );
            }
        }
    }

    /// A frame was presented: the next one has the whole budget.
    fn reset(&self) {
        self.waited_us.store(0, Ordering::Relaxed);
    }
}

fn frame_budget() -> &'static FrameBudget {
    static B: OnceLock<FrameBudget> = OnceLock::new();
    B.get_or_init(|| budget_from(std::env::var("XCL_PSO_FRAME_BUDGET_MS").ok().as_deref(), std::env::var("XCL_PSO_COMPUTE_BUDGET").ok().as_deref()))
}

/// The budget `XCL_PSO_FRAME_BUDGET_MS` and `XCL_PSO_COMPUTE_BUDGET` ask for (None: not set).
fn budget_from(ms: Option<&str>, compute: Option<&str>) -> FrameBudget {
    let limit = match ms.and_then(|v| v.trim().parse::<u64>().ok()) {
        Some(0) => None,
        Some(ms) => Some(Duration::from_millis(ms)),
        None => Some(Duration::from_secs(8)),
    };
    FrameBudget::new(limit, compute.is_some_and(|v| v.trim() == "1"))
}

/// PresentX: the pipeline waits of the next frame start from nothing.
pub fn frame_presented() {
    frame_budget().reset();
    FRAME.fetch_add(1, Ordering::Relaxed);
}

/// the frame being made: 1 + the PresentX so far (`frame_presented`)
static FRAME: AtomicU64 = AtomicU64::new(1);
/// the frame (`FRAME`) in which a dispatch was last skipped for a pipeline still being built (0: none)
static DISPATCH_SKIPPED_IN: AtomicU64 = AtomicU64::new(0);

/// A compute dispatch was skipped because its pipeline is still being built (`resolve_for_draw`
/// past the frame's budget, `XCL_PSO_COMPUTE_BUDGET=1`): what it would have written is missing.
pub fn dispatch_skipped() {
    DISPATCH_SKIPPED_IN.store(FRAME.load(Ordering::Relaxed), Ordering::Relaxed);
}

/// `pso` is the stand-in of a compute pipeline (built, failed or still being built).
pub fn is_compute(pso: usize) -> bool {
    entry(pso).is_some_and(|p| p.compute)
}

/// Whether work that takes its size from GPU memory – an ExecuteIndirect: the counts of its
/// draws or the groups of its dispatches – is left out: a dispatch of this frame or of the one
/// before it was skipped for a pipeline still being built, and the arguments it may have been
/// there to write (a title's GPU culling) are then whatever the memory holds. The PC call has
/// no bound for them: one draw of 4 billion instances keeps the GPU past the system's 2 s and
/// the device is removed (0x887a0006). `XCL_PSO_SKIP_INDIRECT=0`: issued all the same.
pub fn indirect_unsafe() -> bool {
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| std::env::var("XCL_PSO_SKIP_INDIRECT").map_or(true, |v| v.trim() != "0"))
        && skipped_lately(DISPATCH_SKIPPED_IN.load(Ordering::Relaxed), FRAME.load(Ordering::Relaxed))
}

/// A dispatch skipped in frame `skipped_in` (0: none) still counts in frame `frame`: that frame
/// and the next (what one frame writes the next may read).
fn skipped_lately(skipped_in: u64, frame: u64) -> bool {
    skipped_in != 0 && frame.saturating_sub(skipped_in) <= 1
}

/// draws and dispatches issued with a stand-in because their pipeline was not built in time
static STANDIN_DRAWS: AtomicUsize = AtomicUsize::new(0);

/// Draws and dispatches that went on with a stand-in (drew or computed nothing) because
/// `resolve_for_draw` gave up on their pipeline (`report.json → d3d12.standin_draws`). Lost,
/// not replayed.
pub fn standin_draws() -> usize {
    STANDIN_DRAWS.load(Ordering::Relaxed)
}

/// stand-ins whose first give-up was logged (forgotten with the stand-in, `released`)
static GAVE_UP: Mutex<Option<std::collections::HashSet<usize>>> = Mutex::new(None);

/// A wait for `pso` ended with its stand-in. `lost`: for a draw or a dispatch (not a bind).
fn note_standin(pso: usize, compute: bool, lost: bool, waited: Duration, limit: Option<Duration>, budget: &FrameBudget) {
    let n = if lost { STANDIN_DRAWS.fetch_add(1, Ordering::Relaxed) + 1 } else { STANDIN_DRAWS.load(Ordering::Relaxed) };
    let first = {
        let mut g = GAVE_UP.lock().unwrap();
        let seen = g.get_or_insert_with(Default::default);
        seen.len() < 64 && seen.insert(pso)
    };
    if first || (lost && n >= 64 && n.is_power_of_two()) {
        let why = match (budget.spent(), budget.limit, limit) {
            (true, Some(b), _) => format!("the frame's {} ms for pipeline waits are spent, XCL_PSO_FRAME_BUDGET_MS", b.as_millis()),
            (_, _, Some(l)) => format!("limit {} ms, XCL_PSO_WAIT_MS", l.as_millis()),
            _ => "no time left".to_string(),
        };
        crate::xlog_warn!(
            "D3D12.X: thread '{}': {} pipeline {pso:#x} not built after {} ms ({why}) – {}, the pipeline still building on a worker ({n} draws and dispatches lost so far)",
            std::thread::current().name().unwrap_or("title"),
            if compute { "compute" } else { "graphics" },
            waited.as_millis(),
            match (lost, compute) {
                (false, _) => "its stand-in is bound",
                (true, false) => "the draw goes with its stand-in and is lost",
                (true, true) => "the dispatch is skipped",
            }
        );
    }
}

/// For a draw, a dispatch or an ExecuteIndirect of console code: the pipeline is waited for
/// while the frame's budget lasts – a graphics one at most `draw_wait_limit` besides, each draw
/// on its own – and built on a worker, not in the title's thread, where a driver compile cannot
/// be cut short. Past that the call goes on with the stand-in (a draw draws nothing, a dispatch
/// is skipped by its caller: the stand-in is not a translated pipeline), counted and logged.
/// Without a budget for it (`FrameBudget`) a compute pipeline is waited for as `resolve_wait` does.
pub fn resolve_for_draw(pso: usize) -> usize {
    resolve_within(pso, draw_wait_limit(), frame_budget(), true)
}

/// Waits for `p`'s build as long as `limit` (this wait's own) and `budget` allow, charging the
/// budget as it goes (in slices: other threads' waits spend it too): (built, time waited).
fn wait_built(p: &Pending, limit: Option<Duration>, budget: &FrameBudget) -> (bool, Duration) {
    const SLICE: Duration = Duration::from_millis(50);
    let start = Instant::now();
    let _waiting = TitleWait::new();
    let mut charged = start;
    let mut done = p.done.lock().unwrap();
    if !*done {
        p.title_waits();
    }
    while !*done {
        let now = Instant::now();
        budget.charge(now - charged);
        charged = now;
        let mut slice = SLICE;
        if let Some(l) = limit {
            slice = slice.min(l.saturating_sub(now - start));
        }
        if let Some(left) = budget.left() {
            slice = slice.min(left);
        }
        if slice.is_zero() {
            break;
        }
        done = p.cv.wait_timeout(done, slice).unwrap().0;
    }
    let finished = *done;
    drop(done);
    budget.charge(charged.elapsed());
    (finished, start.elapsed())
}

fn resolve_within(pso: usize, limit: Option<Duration>, budget: &FrameBudget, lost: bool) -> usize {
    let Some(p) = entry(pso) else { return pso };
    asked(&p);
    // a compute pipeline has no limit of its own, only the frame's budget
    let limit = limit.filter(|_| !p.compute);
    let bounded = if p.compute { budget.limit.is_some() && budget.compute } else { limit.is_some() || budget.limit.is_some() };
    if !bounded {
        return wait_unbounded(pso, &p, budget);
    }
    // to the workers (a lazy one has not gone yet)
    if !p.sent.swap(true, Ordering::AcqRel) {
        let _ = queue().lock().unwrap().send(p.clone());
    }
    let (finished, waited) = wait_built(&p, limit, budget);
    if !finished {
        note_standin(pso, p.compute, lost, waited, limit, budget);
    } else {
        note_long_wait(pso, p.compute, false, waited);
    }
    match p.real.load(Ordering::Acquire) {
        0 => pso,
        r => r,
    }
}

/// For binding: compute pipelines are waited for (while the frame's budget lasts, as in
/// `resolve_for_draw`; past it the stand-in is bound), graphics ones are not.
pub fn resolve_for_bind(pso: usize) -> usize {
    match entry(pso) {
        Some(p) if p.compute => resolve_within(pso, None, frame_budget(), false),
        Some(_) => resolve(pso),
        None => pso,
    }
}

/// The title released stand-in `pso` for good: forget it (its address may come back for
/// another object) and return its real pipeline to release (0: none, or still being built –
/// the worker's result is then released when it arrives).
pub fn released(pso: usize) -> usize {
    let Some(p) = PENDING.lock().unwrap().as_mut().and_then(|m| m.remove(&pso)) else { return 0 };
    if let Some(g) = GAVE_UP.lock().unwrap().as_mut() {
        g.remove(&pso);
    }
    let done = p.done.lock().unwrap();
    if *done {
        p.real.swap(0, Ordering::AcqRel)
    } else {
        // the worker stores its result into a Pending nobody will read: mark it orphaned
        p.orphan.store(true, Ordering::Release);
        0
    }
}

/// `pso` is a stand-in whose real pipeline is still being built.
pub fn is_pending(pso: usize) -> bool {
    entry(pso).is_some_and(|p| !*p.done.lock().unwrap())
}

// ---------------------------------------------------------------------------------------
// Building ahead while the title waits
//
// When a retail title loads its world it binds hundreds of compute pipelines one after another
// from one thread, each built at its first bind (DXC, then the driver: seconds, up to many
// minutes for its biggest programs on an NVIDIA GPU), and presents nothing meanwhile. Its
// pipelines of one family are made one after another, so while its thread waits for one,
// `ahead_threads` threads build the compute pipelines made after it, at a lower priority. They
// run only while no frame is held up but the title's: from a title thread's wait of 3 s for a
// pipeline, or 5 s of such waits since the last present, to the next present.
// ---------------------------------------------------------------------------------------

/// creation order of pipelines (`Pending::seq`)
static NEXT_SEQ: AtomicU64 = AtomicU64::new(0);
/// lazy compute pipelines not sent to a worker yet, in the order they were made (dead and sent
/// ones are dropped as `next_after` passes them)
static LAZY_COMPUTE: Mutex<std::collections::BTreeMap<u64, std::sync::Weak<Pending>>> = Mutex::new(std::collections::BTreeMap::new());
/// creation order of the compute pipeline the title asked for last
static CURSOR: AtomicU64 = AtomicU64::new(0);
/// compute pipelines built ahead, and those of them the title asked for later
static AHEAD_BUILT: AtomicUsize = AtomicUsize::new(0);
static AHEAD_USED: AtomicUsize = AtomicUsize::new(0);
/// title threads in a pipeline wait, and when the first of them started (µs from `epoch`)
static IN_WAIT: AtomicUsize = AtomicUsize::new(0);
static WAIT_SINCE_US: AtomicU64 = AtomicU64::new(0);

fn epoch() -> Instant {
    static E: OnceLock<Instant> = OnceLock::new();
    *E.get_or_init(Instant::now)
}

/// A title thread waits for a pipeline while this lives.
struct TitleWait;

impl TitleWait {
    fn new() -> Self {
        if IN_WAIT.fetch_add(1, Ordering::AcqRel) == 0 {
            WAIT_SINCE_US.store(epoch().elapsed().as_micros() as u64, Ordering::Release);
        }
        TitleWait
    }
}

impl Drop for TitleWait {
    fn drop(&mut self) {
        IN_WAIT.fetch_sub(1, Ordering::AcqRel);
    }
}

/// `XCL_PSO_AHEAD=n`: n threads build compute pipelines ahead while the title waits (default 0:
/// none). Each of a retail title's biggest programs takes 1.5 – 4 GB in an NVIDIA driver while
/// it compiles, and the big ones of the background share one slot (`BIG_COMPILES`): in a first
/// run of a retail title's world loading (RTX 5080) the one thread that got it spent 700 s on a
/// single program, so nothing built ahead was used yet.
fn ahead_threads() -> usize {
    if cfg!(test) {
        return 0;
    }
    static N: OnceLock<usize> = OnceLock::new();
    *N.get_or_init(|| std::env::var("XCL_PSO_AHEAD").ok().and_then(|v| v.trim().parse().ok()).unwrap_or(0).min(16))
}

/// The title asks for `p` (bind, draw, dispatch): it is used (`used`), and building ahead goes
/// on from it.
fn asked(p: &Pending) {
    p.note_use();
    if !p.compute {
        return;
    }
    CURSOR.store(p.seq, Ordering::Relaxed);
    if p.ahead.swap(false, Ordering::AcqRel) {
        AHEAD_USED.fetch_add(1, Ordering::Relaxed);
    }
}

/// Only the title's own waits hold up its frames now.
fn stalled() -> bool {
    if frame_budget().waited() >= Duration::from_secs(5) {
        return true;
    }
    IN_WAIT.load(Ordering::Acquire) > 0
        && epoch().elapsed().as_micros() as u64 >= WAIT_SINCE_US.load(Ordering::Acquire) + Duration::from_secs(3).as_micros() as u64
}

/// The first pipeline of `m` not sent yet after `cursor`, else from the start; the dead and sent
/// ones it passes are dropped, and so is the one returned.
fn next_after(m: &mut std::collections::BTreeMap<u64, std::sync::Weak<Pending>>, cursor: u64) -> Option<Arc<Pending>> {
    let mut drop_keys = Vec::new();
    let mut found = None;
    for (&k, w) in m.range(cursor.saturating_add(1)..).chain(m.range(..=cursor)) {
        drop_keys.push(k);
        if let Some(p) = w.upgrade() {
            if !p.sent.load(Ordering::Acquire) {
                found = Some(p);
                break;
            }
        }
    }
    for k in drop_keys {
        m.remove(&k);
    }
    found
}

fn start_ahead() {
    static STARTED: std::sync::Once = std::sync::Once::new();
    STARTED.call_once(|| {
        for k in 0..ahead_threads() {
            let _ = std::thread::Builder::new().name(format!("xcl-pso-ahead-{k}")).stack_size(64 << 20).spawn(ahead_loop);
        }
    });
}

fn ahead_loop() {
    unsafe {
        use windows_sys::Win32::System::Threading::{GetCurrentThread, SetThreadPriority, THREAD_PRIORITY_BELOW_NORMAL};
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    }
    loop {
        if !stalled() {
            std::thread::sleep(Duration::from_millis(250));
            continue;
        }
        let next = next_after(&mut LAZY_COMPUTE.lock().unwrap(), CURSOR.load(Ordering::Relaxed));
        let Some(p) = next else {
            std::thread::sleep(Duration::from_secs(1));
            continue;
        };
        if p.sent.swap(true, Ordering::AcqRel) {
            continue;
        }
        let Some(job) = p.job.lock().unwrap().take() else { continue };
        static ONCE: std::sync::Once = std::sync::Once::new();
        ONCE.call_once(|| {
            crate::xlog_info!(
                "D3D12.X: the title waits for pipelines and presents nothing – {} threads build the compute pipelines made after the one it waits for (XCL_PSO_AHEAD)",
                ahead_threads()
            );
        });
        let start = Instant::now();
        let real = job();
        p.ahead.store(real != 0, Ordering::Release);
        complete(&p, real);
        let n = AHEAD_BUILT.fetch_add(1, Ordering::Relaxed) + 1;
        if n.is_power_of_two() || n % 64 == 0 || start.elapsed() >= Duration::from_secs(30) {
            crate::xlog_info!(
                "D3D12.X: {n} compute pipelines built ahead of the title, {} of them asked for since (this one {:.1} s)",
                AHEAD_USED.load(Ordering::Relaxed),
                start.elapsed().as_secs_f32()
            );
        }
    }
}

/// (built in the background, failed, still building)
pub fn counts() -> (usize, usize, usize) {
    let pending = PENDING.lock().unwrap().as_ref().map_or(0, |m| m.values().filter(|p| !*p.done.lock().unwrap()).count());
    (BUILT.load(Ordering::Relaxed), FAILED.load(Ordering::Relaxed), pending)
}

#[cfg(test)]
mod ahead_tests {
    use super::*;

    fn pending(seq: u64, sent: bool) -> Arc<Pending> {
        Arc::new(Pending {
            job: Mutex::new(Some(Box::new(|| 0))),
            sent: std::sync::atomic::AtomicBool::new(sent),
            real: AtomicUsize::new(0),
            done: Mutex::new(false),
            cv: Condvar::new(),
            compute: true,
            key: 0,
            used: Default::default(),
            orphan: Default::default(),
            seq,
            ahead: Default::default(),
            builder: AtomicUsize::new(0),
            waited: Default::default(),
        })
    }

    /// Building ahead takes the pipelines made after the one the title asks for, in order, then
    /// wraps around to the earlier ones; sent and released ones are passed and forgotten.
    #[test]
    fn the_pipelines_made_after_the_asked_one_come_first_then_the_earlier_ones() {
        let kept: Vec<Arc<Pending>> = [(1, false), (2, false), (5, true), (6, false), (9, false)].iter().map(|&(s, sent)| pending(s, sent)).collect();
        let released = pending(7, false);
        let mut m = std::collections::BTreeMap::new();
        for p in kept.iter().chain([&released]) {
            m.insert(p.seq, Arc::downgrade(p));
        }
        drop(released);
        let mut order = Vec::new();
        while let Some(p) = next_after(&mut m, 4) {
            order.push(p.seq);
            p.sent.store(true, Ordering::Release);
        }
        // 5 was sent already, 7 is gone; 6 and 9 come before 1 and 2
        assert_eq!(order, [6, 9, 1, 2]);
        assert!(m.is_empty());
    }
}

#[cfg(test)]
mod draw_wait_tests {
    use super::*;

    /// A graphics build that does not finish (an NVIDIA driver compile of minutes, #156) holds
    /// each draw only up to the limit and runs on a worker. With `resolve_wait` the draw waited
    /// for it without limit (building it itself when no worker had taken it), which held the
    /// title past its hang watchdog.
    #[test]
    fn each_draw_waits_for_a_graphics_build_only_within_the_limit_and_never_builds_it_itself() {
        let (standin, real) = (0x7410_0001usize, 0x7410_0002usize);
        let (go_tx, go_rx) = mpsc::channel::<()>();
        let (who_tx, who_rx) = mpsc::channel::<Option<String>>();
        submit_lazy(
            standin,
            false,
            Box::new(move || {
                let _ = who_tx.send(std::thread::current().name().map(str::to_string));
                let _ = go_rx.recv();
                real
            }),
            0,
        );
        let limit = Some(Duration::from_millis(300));
        // no frame budget (`XCL_PSO_FRAME_BUDGET_MS=0`), whatever was waited before
        let none = FrameBudget::new(None, true);
        none.charge(Duration::from_secs(60));
        let lost = standin_draws();
        for draw in 0..2 {
            // every draw gets the whole limit, not what an earlier draw left of it
            let t = Instant::now();
            assert_eq!(resolve_within(standin, limit, &none, true), standin, "draw {draw} goes with the stand-in");
            let took = t.elapsed();
            assert!(took >= Duration::from_millis(290) && took < Duration::from_secs(5), "draw {draw} waited {took:?}");
        }
        assert!(standin_draws() >= lost + 2, "both draws counted as lost");
        let who = who_rx.recv_timeout(Duration::from_secs(5)).expect("the build went to a worker");
        assert!(who.as_deref().is_some_and(|n| n.starts_with("xcl-pso")), "built on {who:?}");
        assert!(is_pending(standin));
        // once built: the real pipeline
        go_tx.send(()).unwrap();
        assert_eq!(resolve_within(standin, Some(Duration::from_secs(10)), &none, true), real);
        assert!(!is_pending(standin));
        let _ = released(standin);
    }

    /// Without a frame budget (`XCL_PSO_FRAME_BUDGET_MS=0`) compute pipelines are still waited
    /// for, and `XCL_PSO_WAIT_MS=0` (no limit) waits for graphics ones as before.
    #[test]
    fn compute_pipelines_and_no_limit_wait_as_before() {
        let none = budget_from(Some("0"), None);
        assert!(none.limit.is_none() && !none.compute);
        none.charge(Duration::from_secs(60));
        assert!(!none.spent());
        let (c, c_real) = (0x7410_0011usize, 0x7410_0012usize);
        submit_lazy(
            c,
            true,
            Box::new(move || {
                std::thread::sleep(Duration::from_millis(100));
                c_real
            }),
            0,
        );
        assert_eq!(resolve_within(c, Some(Duration::ZERO), &none, true), c_real);
        let (g, g_real) = (0x7410_0021usize, 0x7410_0022usize);
        submit_lazy(g, false, Box::new(move || g_real), 0);
        assert_eq!(resolve_within(g, None, &none, true), g_real);
        assert_eq!(resolve_within(0x7410_0031, Some(Duration::ZERO), &none, true), 0x7410_0031, "not a stand-in");
        let _ = released(c);
        let _ = released(g);
    }

    /// The defaults: 8 s a frame, compute pipelines included.
    #[test]
    fn the_frame_budget_is_8_s_with_compute_unless_the_variables_say_otherwise() {
        let b = budget_from(None, None);
        assert_eq!((b.limit, b.compute), (Some(Duration::from_secs(8)), false));
        let b = budget_from(Some("1500"), Some("0"));
        assert_eq!((b.limit, b.compute), (Some(Duration::from_millis(1500)), false));
        assert!(budget_from(Some("0"), Some("1")).limit.is_none());
    }

    /// A build that does not finish, waited for by two threads at once: they spend one budget
    /// between them (each alone would have waited all of it), a later draw does not wait at
    /// all, and after a present the next one waits again. Per draw (the 4 s of #267) eight
    /// pending pipelines held a retail game's render thread 32 s, past its 30 s watchdog.
    #[test]
    fn the_frame_budget_is_shared_by_all_threads_and_a_present_gives_it_back() {
        let standin = 0x7410_0041usize;
        let (go_tx, go_rx) = mpsc::channel::<()>();
        submit_lazy(
            standin,
            false,
            Box::new(move || {
                let _ = go_rx.recv();
                0
            }),
            0,
        );
        let budget = Arc::new(FrameBudget::new(Some(Duration::from_millis(600)), true));
        let limit = Some(Duration::from_secs(20));
        let waits: Vec<Duration> = (0..2)
            .map(|_| {
                let budget = budget.clone();
                std::thread::spawn(move || {
                    let t = Instant::now();
                    assert_eq!(resolve_within(standin, limit, &budget, true), standin);
                    t.elapsed()
                })
            })
            .collect::<Vec<_>>()
            .into_iter()
            .map(|t| t.join().unwrap())
            .collect();
        assert!(budget.spent(), "waited {:?} of 600 ms", budget.waited());
        // what one thread waited, the other did not (each with the whole budget: 1200 ms)
        assert!(waits.iter().sum::<Duration>() < Duration::from_millis(1000), "each thread had the whole budget: {waits:?}");
        // spent: the next draw goes with the stand-in at once
        let t = Instant::now();
        assert_eq!(resolve_within(standin, limit, &budget, true), standin);
        assert!(t.elapsed() < Duration::from_millis(500), "a draw past the budget waited {:?}", t.elapsed());
        // a present: the whole budget again
        budget.reset();
        assert!(!budget.spent());
        let t = Instant::now();
        assert_eq!(resolve_within(standin, Some(Duration::from_millis(200)), &budget, true), standin);
        assert!(t.elapsed() >= Duration::from_millis(190), "a draw after the present waited only {:?}", t.elapsed());
        go_tx.send(()).unwrap();
        let _ = released(standin);
    }

    /// An ExecuteIndirect is left out in the frame a dispatch was skipped for its pipeline and
    /// in the next one (its arguments may be that dispatch's to write), not before any skip and
    /// not once two frames passed without one. Only compute stand-ins count as producers.
    #[test]
    fn indirect_work_is_left_out_for_two_frames_after_a_skipped_dispatch() {
        assert!(!skipped_lately(0, 1) && !skipped_lately(0, 7), "no dispatch was skipped");
        assert!(skipped_lately(7, 7), "the frame of the skip");
        assert!(skipped_lately(7, 8), "the frame after it");
        assert!(!skipped_lately(7, 9) && !skipped_lately(7, 1000));
        let (g, c) = (0x7410_0071usize, 0x7410_0073usize);
        submit_lazy(g, false, Box::new(|| 0), 0);
        submit_lazy(c, true, Box::new(|| 0), 0);
        assert!(is_compute(c) && !is_compute(g) && !is_compute(0x7410_0075));
        for p in [g, c] {
            let _ = released(p);
        }
    }

    /// Once the frame's budget is spent a compute pipeline still building is not waited for
    /// either, at a dispatch or at a bind, and is built on a worker – not in the title's
    /// thread, where its driver compile (minutes, #156) cannot be cut short. The stand-in
    /// comes back: not a translated pipeline, so its dispatch is skipped by the caller.
    /// `XCL_PSO_COMPUTE_BUDGET=0`: waited for and built here, as before.
    #[test]
    fn past_the_frame_budget_draws_and_compute_go_with_the_stand_in_at_once() {
        let spent = FrameBudget::new(Some(Duration::from_millis(100)), true);
        spent.charge(Duration::from_secs(1));
        assert!(spent.spent());
        let (g, c) = (0x7410_0051usize, 0x7410_0053usize);
        let (go_tx, go_rx) = mpsc::channel::<()>();
        let go_rx = Arc::new(Mutex::new(go_rx));
        let (who_tx, who_rx) = mpsc::channel::<Option<String>>();
        for (standin, compute) in [(g, false), (c, true)] {
            let (go_rx, who_tx) = (go_rx.clone(), who_tx.clone());
            submit_lazy(
                standin,
                compute,
                Box::new(move || {
                    let _ = who_tx.send(std::thread::current().name().map(str::to_string));
                    let _ = go_rx.lock().unwrap().recv();
                    standin + 1
                }),
                0,
            );
        }
        let lost = standin_draws();
        let t = Instant::now();
        assert_eq!(resolve_within(g, Some(Duration::from_secs(20)), &spent, true), g, "the draw goes with the stand-in");
        assert_eq!(resolve_within(c, None, &spent, true), c, "the dispatch gets the stand-in");
        assert_eq!(resolve_within(c, None, &spent, false), c, "and so does a bind");
        assert!(t.elapsed() < Duration::from_millis(500), "waited {:?} with the budget spent", t.elapsed());
        assert!(standin_draws() >= lost + 2, "the draw and the dispatch are counted as lost");
        for _ in 0..2 {
            let who = who_rx.recv_timeout(Duration::from_secs(5)).expect("both builds went to the workers");
            assert!(who.as_deref().is_some_and(|n| n.starts_with("xcl-pso")), "built on {who:?}");
        }
        assert!(is_pending(g) && is_pending(c));
        // built: the real pipelines, budget or not
        go_tx.send(()).unwrap();
        go_tx.send(()).unwrap();
        assert_eq!(resolve_within(c, None, &FrameBudget::new(Some(Duration::from_secs(20)), true), true), c + 1);
        assert_eq!(resolve_within(g, Some(Duration::from_secs(20)), &FrameBudget::new(Some(Duration::from_secs(20)), true), true), g + 1);

        // XCL_PSO_COMPUTE_BUDGET=0: the spent budget does not stop a compute wait
        let old = budget_from(Some("100"), Some("0"));
        old.charge(Duration::from_secs(1));
        let (c2, me) = (0x7410_0061usize, std::thread::current().id());
        let (here_tx, here_rx) = mpsc::channel::<bool>();
        submit_lazy(
            c2,
            true,
            Box::new(move || {
                let _ = here_tx.send(std::thread::current().id() == me);
                std::thread::sleep(Duration::from_millis(100));
                c2 + 1
            }),
            0,
        );
        assert_eq!(resolve_within(c2, None, &old, true), c2 + 1);
        assert!(here_rx.recv().unwrap(), "built in the waiting thread, as before");
        for p in [g, c, c2] {
            let _ = released(p);
        }
    }
}
