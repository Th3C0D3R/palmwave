//! The host process: started by the launcher with the game's command line. Sets up
//! logging, the VFS, hooks and HLE, maps the title and jumps to its entry point.

use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

use crate::config::Boot;

pub static HOST_MODE: AtomicBool = AtomicBool::new(false);

/// Whether `XCL_ACTIVITY` is set (the 2 s activity and memory lines; counters only they read).
pub fn activity() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("XCL_ACTIVITY").is_some())
}

/// `XCL_ACTIVITY` with `XCL_ACTIVITY_HEAPS=1`: the process's heaps – (bytes committed, bytes
/// allocated) of the default heap (Rust's allocator, the C runtime's and DXC's `malloc`), then
/// (count, bytes committed, the largest's bytes committed) of the others (drivers, the D3D12
/// runtime). Off by default: HeapSummary locks each heap while it walks it, 0.5 s on a retail
/// title's multi-GB heap every 2 s, and the title's threads that allocate wait meanwhile.
fn heaps() -> ((usize, usize), (usize, usize, usize)) {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    if !*ON.get_or_init(|| std::env::var("XCL_ACTIVITY_HEAPS").is_ok_and(|v| v == "1")) {
        return ((0, 0), (0, 0, 0));
    }
    measure_heaps()
}

/// `heaps`, measured whatever the switch says.
fn measure_heaps() -> ((usize, usize), (usize, usize, usize)) {
    use windows_sys::Win32::System::Memory::{GetProcessHeap, GetProcessHeaps, HeapSummary, HEAP_SUMMARY};
    unsafe {
        let own = GetProcessHeap();
        let mut all = [std::ptr::null_mut(); 256];
        let n = (GetProcessHeaps(all.len() as u32, all.as_mut_ptr()) as usize).min(all.len());
        let summary = |h| {
            let mut s: HEAP_SUMMARY = std::mem::zeroed();
            s.cb = std::mem::size_of::<HEAP_SUMMARY>() as u32;
            if HeapSummary(h, 0, &mut s) != 0 { (s.cbCommitted, s.cbAllocated) } else { (0, 0) }
        };
        let others = all[..n].iter().filter(|&&h| h != own).fold((0, 0, 0), |a, &h| {
            let c = summary(h).0;
            (a.0 + 1, a.1 + c, a.2.max(c))
        });
        (summary(own), others)
    }
}

pub fn host_main(boot_path: &Path) -> i32 {
    let boot: Boot = match std::fs::read_to_string(boot_path).map_err(|e| e.to_string()).and_then(|s| serde_json::from_str(&s).map_err(|e| e.to_string())) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("xcl host: cannot read {}: {e}", boot_path.display());
            return 0x7C1_0001;
        }
    };
    crate::log::init(
        Some(&boot.logs_dir().join("xcl.log")),
        crate::log::Level::parse(&boot.config.log_level),
        boot.config.log_to_stderr,
    );
    crate::xlog_info!("XCL {} – host for '{}'", env!("CARGO_PKG_VERSION"), boot.title);
    crate::xlog_info!("game: {} (read-only folder: {})", boot.exe_path.display(), boot.game_root.display());
    crate::xlog_info!("XCL data: {}", boot.data_dir.display());

    if std::env::var_os("XCL_UMA_MB").is_none() && boot.config.uma_mb != 0 {
        std::env::set_var("XCL_UMA_MB", boot.config.uma_mb.to_string());
    }
    crate::state::set_boot(boot.clone());
    // `XCL_ACTIVITY=1`: every 2 s what the title did since (a timeline of where it stops)
    if activity() {
        let _ = std::thread::Builder::new().name("xcl-activity".into()).spawn(|| {
            let snap = || {
                let (p, e) = crate::hle::d3d12x::present::activity();
                let (d, x) = crate::hle::d3d12x::bind::activity();
                let f = crate::hle::d3d12x::frame::activity();
                let (r, b, fail) = crate::hle::dstorage::counters();
                [p, e, d, x, f[0], f[1], f[2], f[3], r, b >> 20, fail]
            };
            let mut last = snap();
            loop {
                std::thread::sleep(std::time::Duration::from_secs(2));
                let now = snap();
                let d: Vec<u64> = now.iter().zip(last).map(|(a, b)| a - b).collect();
                crate::xlog_info!(
                    "activity (2 s): {} presents, {} submits, {} draws, {} dispatches, {} frame-event waits ({} timed out), {} frame statistics ({} not flipped); DirectStorage {} requests, {} MB, {} failed",
                    d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10]
                );
                // where the memory is: the process's commit, video memory, and XCL's parts of it
                let private = unsafe {
                    use windows_sys::Win32::System::ProcessStatus::{K32GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS_EX};
                    let mut c: PROCESS_MEMORY_COUNTERS_EX = std::mem::zeroed();
                    c.cb = std::mem::size_of_val(&c) as u32;
                    K32GetProcessMemoryInfo(windows_sys::Win32::System::Threading::GetCurrentProcess(), &mut c as *mut _ as *mut _, c.cb);
                    c.PrivateUsage
                };
                let (local, non_local) = crate::hle::d3d12x::real::video_memory().unwrap_or((0, 0));
                let (arenas, arenas_made) = crate::hle::d3d12x::vram::arena_bytes();
                let (pools, pools_mapped) = crate::hle::d3d12x::sparse::pool_bytes();
                let (pages, page_pools) = crate::hle::d3d12x::vram::page_bytes();
                let (under, given_back, taken_again) = crate::hle::d3d12x::sparse::under_target_counts();
                let mb = |b: u64| b >> 20;
                crate::xlog_info!(
                    "memory: {} MB committed by the process; video memory {} MB local, {} MB non-local; target pages {} MB (pools {} MB); arenas {} MB in use, {} MB made; sparse pools {} MB ({} MB mapped); {} MB of sparse tiles under targets hold none ({} tiles given back, {} taken again)",
                    mb(private as u64),
                    mb(local),
                    mb(non_local),
                    mb(pages as u64),
                    mb(page_pools as u64),
                    mb(arenas as u64),
                    mb(arenas_made as u64),
                    mb(pools as u64),
                    mb(pools_mapped as u64),
                    mb(under as u64),
                    given_back,
                    taken_again
                );
                // what XCL itself holds: one line each for the textures kept in step, the
                // buffers of their uploads and of the other paths, and the process's heaps
                let (active, shadowed, pending, vecs, vecs_peak) = crate::hle::d3d12x::title_textures::memory();
                let page_counts = crate::hle::d3d12x::vram::page_counts();
                let (small, small_slots, small_heaps) = crate::hle::d3d12x::vram::small_counts();
                let (own_pages, extra_pages) = crate::hle::d3d12x::vram::page_kinds();
                crate::xlog_info!(
                    "memory: title textures {} active ({} MB of title memory, {} KB of layout tables), {} shadowed ({} MB, {} KB), {} waiting for an upload; {} on pages ({} MB at their addresses, {} MB of extra tiles); page pools {} heaps, {} tiles free, {} cooling; {} small textures in {} MB of their heaps ({} MB of slots)",
                    active[0], mb(active[1]), active[2] >> 10, shadowed[0], mb(shadowed[1]), shadowed[2] >> 10, pending, page_counts[0], mb(own_pages as u64), mb(extra_pages as u64), page_counts[1], page_counts[2], page_counts[3], small, mb(small_heaps as u64), mb(small_slots as u64)
                );
                let staging = crate::gpu::texture::staging_counts();
                let (sparse_heaps, sparse_upload, sparse_readback) = crate::hle::d3d12x::sparse::buffer_counts();
                let (patterns, pattern_bytes) = crate::hle::d3d12x::list_x::pattern_bytes();
                let (title_uploads, title_upload_bytes) = crate::hle::d3d12x::real::title_uploads();
                crate::xlog_info!(
                    "memory: UPLOAD resources: texture staging {} alive ({} MB), {} made ({} MB); sparse ring and zeros {} MB (READBACK {} MB), {} sparse pool heaps; fill patterns {} ({} KB); the title's own {} made ({} MB); upload Vecs {} MB alive, {} MB at most",
                    staging[2], mb(staging[3]), staging[0], mb(staging[1]), mb(sparse_upload as u64), mb(sparse_readback as u64), sparse_heaps, patterns, pattern_bytes >> 10, title_uploads, mb(title_upload_bytes), mb(vecs), mb(vecs_peak)
                );
                let (compiling, queued) = crate::gpu::recomp::dxc::compiles();
                let (shaders, shader_bytes) = crate::gpu::recomp::pso::shader_cache();
                let heaps_t0 = std::time::Instant::now();
                let ((heap, heap_used), (others, others_bytes, largest)) = heaps();
                crate::xlog_info!(
                    "memory: DXC {} compiling, {} queued; ShaderCache {} shaders, {} KB; DirectStorage buffers {} KB in flight; default heap {} MB committed ({} MB allocated), {} other heaps {} MB committed (largest {} MB) [{} ms]",
                    compiling, queued, shaders, shader_bytes >> 10, crate::hle::dstorage::in_flight_bytes() >> 10, mb(heap as u64), mb(heap_used as u64), others, mb(others_bytes as u64), mb(largest as u64), heaps_t0.elapsed().as_millis()
                );
                let (evicted, mapped_again) = crate::hle::d3d12x::vram::eviction_counts();
                if evicted > 0 {
                    crate::xlog_info!("page budget: {evicted} textures evicted, {mapped_again} mapped again");
                }
                last = now;
            }
        });
    }
    // `XCL_THREAD_DUMP_AT=s1,s2…`: where every thread is at those seconds (a stalled title)
    if let Ok(v) = std::env::var("XCL_THREAD_DUMP_AT") {
        let at: Vec<f64> = v.split(',').filter_map(|s| s.trim().parse().ok()).collect();
        let _ = std::thread::Builder::new().name("xcl-thread-dump".into()).spawn(move || {
            let t0 = std::time::Instant::now();
            for s in at {
                let left = s - t0.elapsed().as_secs_f64();
                if left > 0.0 {
                    std::thread::sleep(std::time::Duration::from_secs_f64(left));
                }
                crate::eh::dump_threads();
            }
        });
    }
    // `XCL_RUN_SECONDS=n`: the title ends itself after n seconds – the GPU drained, the log and
    // report written (a process killed from outside with GPU work in flight can hang in the driver)
    if let Some(secs) = std::env::var("XCL_RUN_SECONDS").ok().and_then(|v| v.parse::<u64>().ok()) {
        std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_secs(secs));
            let (shown, presented) = crate::hle::d3d12x::present::counters();
            crate::xlog_info!("XCL_RUN_SECONDS: {secs} s – ending the title ({shown} frames shown, {presented} PresentX calls)");
            crate::report::write_final(None);
            crate::log::flush();
            crate::hle::d3d12x::real::end_title(0);
        });
    }
    crate::stubs::init_db(&boot.config.stub_returns);
    crate::hle::init(&boot.config);
    crate::hle::kernelx::fine_timer();
    crate::loader::set_cache_dir(boot.cache_dir());
    let mut dirs = vec![boot.game_root.clone()];
    if let Some(p) = boot.exe_path.parent() {
        if p != boot.game_root {
            dirs.insert(0, p.to_path_buf());
        }
    }
    crate::resolver::set_search_dirs(dirs);

    let mounts: Vec<(String, String)> = boot.config.mounts.iter().map(|(k, v)| (k.clone(), boot.expand(v))).collect();
    // (the shader cache is XCL's too: on a drive a console mount names it must stay where it is)
    crate::vfs::init(crate::vfs::Vfs::new(&boot.game_root, &boot.data_dir, mounts).own_folder(&crate::gpu::recomp::dxc::cache_dir()));
    let _ = std::fs::create_dir_all(boot.data_dir.join("temp"));

    if let Err(e) = crate::vfs_hooks::install() {
        crate::xlog_error!("VFS: not all hooks installed: {e}");
    }
    if let Err(e) = crate::eh::install_pc_hook() {
        crate::xlog_error!("hook RtlPcToFileHeader: {e} – C++ exceptions in the game may not work");
    }
    crate::eh::install_crash_handler();
    crate::eh::start_probes();
    HOST_MODE.store(true, Ordering::Release);

    let main = match crate::loader::load_main(&boot.exe_path) {
        Ok(m) => m,
        Err(e) => {
            crate::xlog_error!("loading the game failed: {e}");
            crate::report::write_final(Some(format!("loader: {e}")));
            return 0x7C1_0002;
        }
    };
    unsafe { crate::hle::kernelx::adopt_title_hooks(&main) };
    crate::report::write_final(None);
    let code = crate::loader::run_main(&main);
    crate::xlog_info!("game entry point returned code {code}");
    crate::report::write_final(None);
    crate::log::flush();
    code as i32
}

#[cfg(test)]
mod tests {
    /// The heap summary of the activity line reads the default heap (what a `Vec` comes from).
    #[test]
    fn the_default_heap_reports_what_is_allocated() {
        let held = vec![1u8; 300 << 10];
        let ((committed, allocated), _) = super::measure_heaps();
        assert!(allocated >= held.len() && committed >= allocated, "{committed} committed, {allocated} allocated");
    }
}
