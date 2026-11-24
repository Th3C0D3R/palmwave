//! Present intervals and a slow, independent sampler for the performance overlay.
use std::collections::{HashMap, VecDeque};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};
use windows::core::Interface;
use windows::Win32::Graphics::Direct3D12::ID3D12Device;
use windows::Win32::Graphics::Dxgi::*;
use windows_sys::Win32::System::Performance::*;

#[derive(Clone, Copy, Default)]
struct Metrics {
    sampled: Option<Instant>,
    cpu: Option<f64>,
    gpu: Option<f64>,
    commit: Option<u64>,
    video: Option<(u64, u64)>,
}

static METRICS: Mutex<Metrics> = Mutex::new(Metrics {
    sampled: None, cpu: None, gpu: None, commit: None, video: None,
});

#[derive(Default)]
pub(super) struct FrameStats {
    previous: Option<Instant>,
    frames: VecDeque<(Instant, f64)>,
    metrics: Metrics,
}

impl FrameStats {
    pub(super) fn record_frame(&mut self, now: Instant) {
        if let Some(previous) = self.previous.replace(now) {
            let seconds = now.saturating_duration_since(previous).as_secs_f64();
            if seconds > 0.0 { self.frames.push_back((now, seconds)); }
        }
        // Keep intervals ending in the last second, including a frame longer than a second.
        while self.frames.front().is_some_and(|(end, _)| now.saturating_duration_since(*end) >= Duration::from_secs(1)) {
            self.frames.pop_front();
        }
    }

    fn frame_times(&self) -> (Option<f64>, Option<f64>) {
        let last = self.frames.back().map(|(_, seconds)| *seconds);
        let average = (!self.frames.is_empty()).then(|| {
            self.frames.iter().map(|(_, seconds)| seconds).sum::<f64>() / self.frames.len() as f64
        });
        (last, average)
    }

    pub(super) fn lines(&mut self, copy_ms: Option<f64>, overlay_ms: Option<f64>) -> Vec<String> {
        if let Ok(metrics) = METRICS.try_lock() { self.metrics = *metrics; }
        let metrics = if self.metrics.sampled.is_some_and(|t| t.elapsed() <= Duration::from_secs(3)) {
            self.metrics
        } else { Metrics::default() };
        let (last, average) = self.frame_times();
        format_lines(last, average, metrics, copy_ms, overlay_ms)
    }
}

fn number(value: Option<f64>) -> String {
    value.filter(|v| v.is_finite() && *v >= 0.0).map(|v| format!("{v:.2}")).unwrap_or_else(|| "N/A".into())
}

fn format_lines(last: Option<f64>, average: Option<f64>, metrics: Metrics, copy: Option<f64>, overlay: Option<f64>) -> Vec<String> {
    let mib = |bytes: u64| format!("{}", bytes >> 20);
    vec![
        format!("FPS      {}  1S {}", number(last.map(|s| 1.0 / s)), number(average.map(|s| 1.0 / s))),
        format!("FRAME MS {}  1S {}", number(last.map(|s| s * 1000.0)), number(average.map(|s| s * 1000.0))),
        format!("GPU USE  {}%", number(metrics.gpu)),
        format!("COPY GPU MS    {}", number(copy)),
        format!("OVERLAY GPU MS {}", number(overlay)),
        format!("CPU USE  {}%", number(metrics.cpu)),
        format!("VRAM MIB {} / {}", metrics.video.map(|v| mib(v.0)).unwrap_or_else(|| "N/A".into()),
            metrics.video.map(|v| mib(v.1)).unwrap_or_else(|| "N/A".into())),
        format!("RAM COMMIT MIB {}", metrics.commit.map(mib).unwrap_or_else(|| "N/A".into())),
    ]
}

fn cpu_percent(previous: u64, current: u64, elapsed: Duration, processors: u32) -> Option<f64> {
    if elapsed.is_zero() || processors == 0 { return None; }
    let ticks = current.checked_sub(previous)?;
    Some((ticks as f64 / 10_000_000.0 / elapsed.as_secs_f64() / processors as f64 * 100.0).clamp(0.0, 100.0))
}

fn process_ticks() -> Option<u64> {
    use windows_sys::Win32::Foundation::FILETIME;
    use windows_sys::Win32::System::Threading::{GetCurrentProcess, GetProcessTimes};
    let [mut created, mut exited, mut kernel, mut user] = [FILETIME::default(); 4];
    unsafe {
        if GetProcessTimes(GetCurrentProcess(), &mut created, &mut exited, &mut kernel, &mut user) == 0 { return None; }
    }
    let ticks = |t: FILETIME| (t.dwHighDateTime as u64) << 32 | t.dwLowDateTime as u64;
    ticks(kernel).checked_add(ticks(user))
}

fn process_commit() -> Option<u64> {
    use windows_sys::Win32::System::ProcessStatus::{K32GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS_EX};
    unsafe {
        let mut counters = PROCESS_MEMORY_COUNTERS_EX::default();
        counters.cb = std::mem::size_of_val(&counters) as u32;
        if K32GetProcessMemoryInfo(windows_sys::Win32::System::Threading::GetCurrentProcess(),
            &mut counters as *mut _ as *mut _, counters.cb) == 0 { return None; }
        Some(if counters.PrivateUsage != 0 { counters.PrivateUsage } else { counters.PagefileUsage } as u64)
    }
}

fn adapter() -> Option<(IDXGIAdapter3, String)> {
    let raw = super::real::device_raw();
    let device = unsafe { ID3D12Device::from_raw_borrowed(&raw) }?;
    unsafe {
        let luid = device.GetAdapterLuid();
        let factory: IDXGIFactory4 = CreateDXGIFactory1().ok()?;
        Some((factory.EnumAdapterByLuid(luid).ok()?, format!("luid_0x{:08x}_0x{:08x}_", luid.HighPart as u32, luid.LowPart)))
    }
}

/// The worker owns all PDH/DXGI calls; presenting only copies its latest small snapshot.
pub(super) fn start_sampling() {
    static STARTED: OnceLock<()> = OnceLock::new();
    STARTED.get_or_init(|| {
        if let Err(error) = std::thread::Builder::new().name("xcl-overlay-stats".into()).spawn(|| {
            let mut gpu = GpuCounter::new();
            let mut adapter = None;
            let mut previous = process_ticks().map(|ticks| (Instant::now(), ticks));
            loop {
                std::thread::sleep(Duration::from_secs(1));
                let now = Instant::now();
                let ticks = process_ticks();
                let processors = unsafe { windows_sys::Win32::System::Threading::GetActiveProcessorCount(0xffff) };
                let cpu = previous.and_then(|(time, before)| cpu_percent(before, ticks?, now.duration_since(time), processors));
                previous = ticks.map(|ticks| (now, ticks));
                if adapter.is_none() { adapter = self::adapter(); }
                let video = adapter.as_ref().and_then(|(a, _)| unsafe {
                    let mut info = DXGI_QUERY_VIDEO_MEMORY_INFO::default();
                    a.QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mut info).ok()?;
                    (info.Budget != 0).then_some((info.CurrentUsage, info.Budget))
                });
                let gpu_use = gpu.as_mut().and_then(|counter| counter.sample(adapter.as_ref().map(|a| a.1.as_str())));
                let metrics = Metrics { sampled: Some(Instant::now()), cpu, gpu: gpu_use, commit: process_commit(), video };
                if let Ok(mut current) = METRICS.lock() { *current = metrics; }
            }
        }) {
            crate::xlog_warn!("D3D12.X: overlay statistics thread unavailable: {error}");
        }
    });
}

struct GpuCounter { query: PDH_HQUERY, counter: PDH_HCOUNTER }

impl Drop for GpuCounter {
    fn drop(&mut self) { unsafe { PdhCloseQuery(self.query); } }
}

impl GpuCounter {
    fn new() -> Option<Self> {
        let mut result = Self { query: std::ptr::null_mut(), counter: std::ptr::null_mut() };
        let path: Vec<u16> = "\\GPU Engine(*)\\Utilization Percentage\0".encode_utf16().collect();
        unsafe {
            if PdhOpenQueryW(std::ptr::null(), 0, &mut result.query) != 0 { return None; }
            if PdhAddEnglishCounterW(result.query, path.as_ptr(), 0, &mut result.counter) != 0 { return None; }
            PdhCollectQueryData(result.query);
        }
        Some(result)
    }

    fn sample(&mut self, adapter: Option<&str>) -> Option<f64> {
        unsafe {
            if PdhCollectQueryData(self.query) != 0 { return None; }
            let adapter = adapter?;
            let (mut bytes, mut count) = (0, 0);
            if PdhGetFormattedCounterArrayW(self.counter, PDH_FMT_DOUBLE, &mut bytes, &mut count, std::ptr::null_mut()) != PDH_MORE_DATA { return None; }
            // u64 storage supplies the alignment required by the returned C structures.
            let mut buffer = vec![0u64; (bytes as usize).div_ceil(8)];
            let items = buffer.as_mut_ptr() as *mut PDH_FMT_COUNTERVALUE_ITEM_W;
            if PdhGetFormattedCounterArrayW(self.counter, PDH_FMT_DOUBLE, &mut bytes, &mut count, items) != 0 { return None; }
            let mut engines = HashMap::new();
            for item in std::slice::from_raw_parts(items, count as usize) {
                if item.FmtValue.CStatus > PDH_CSTATUS_NEW_DATA || item.szName.is_null() { continue; }
                let mut len = 0;
                while *item.szName.add(len) != 0 { len += 1; }
                let name = String::from_utf16_lossy(std::slice::from_raw_parts(item.szName, len));
                add_engine(&mut engines, adapter, &name, item.FmtValue.Anonymous.doubleValue);
            }
            engines.values().copied().reduce(f64::max).map(|value| value.clamp(0.0, 100.0))
        }
    }
}

fn add_engine(engines: &mut HashMap<String, f64>, adapter: &str, name: &str, value: f64) {
    if !value.is_finite() || value < 0.0 { return; }
    // PDH exposes each process separately. Sum each physical engine before selecting the busiest.
    if let Some((_, engine)) = name.to_ascii_lowercase().split_once(adapter) {
        *engines.entry(engine.to_string()).or_default() += value;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn moving_average_expires_old_frames_and_uses_mean_time() {
        let start = Instant::now();
        let mut stats = FrameStats::default();
        stats.record_frame(start);
        assert_eq!(stats.frame_times(), (None, None));
        stats.record_frame(start + Duration::from_millis(100));
        stats.record_frame(start + Duration::from_millis(300));
        let (last, average) = stats.frame_times();
        assert_eq!(last, Some(0.2));
        assert!((average.unwrap() - 0.15).abs() < 1e-10);
        stats.record_frame(start + Duration::from_millis(1200));
        assert!((stats.frame_times().1.unwrap() - 0.55).abs() < 1e-10);
        stats.record_frame(start + Duration::from_secs(16));
        assert_eq!(stats.frame_times(), (Some(14.8), Some(14.8)));
    }

    #[test]
    fn cpu_deltas_normalize_across_processors() {
        assert_eq!(cpu_percent(100, 10_000_100, Duration::from_secs(1), 4), Some(25.0));
        assert_eq!(cpu_percent(0, 80_000_000, Duration::from_secs(2), 4), Some(100.0));
        assert_eq!(cpu_percent(1, 0, Duration::from_secs(1), 4), None);
        assert_eq!(cpu_percent(0, 1, Duration::ZERO, 4), None);
        assert_eq!(cpu_percent(0, 1, Duration::from_secs(1), 0), None);
    }

    #[test]
    fn formatting_keeps_unknown_metrics_and_copy_scope_explicit() {
        let unknown = format_lines(None, None, Metrics::default(), None, Some(f64::NAN));
        assert!(unknown.iter().all(|line| line.contains("N/A")));
        let metrics = Metrics { cpu: Some(25.0), gpu: Some(72.5), commit: Some(3 << 30), video: Some((2 << 30, 8 << 30)), ..Default::default() };
        let lines = format_lines(Some(0.02), Some(0.025), metrics, Some(0.125), Some(0.05));
        assert_eq!(lines[0], "FPS      50.00  1S 40.00");
        assert_eq!(lines[1], "FRAME MS 20.00  1S 25.00");
        assert_eq!(lines[3], "COPY GPU MS    0.12");
        assert_eq!(lines[6], "VRAM MIB 2048 / 8192");
        assert_eq!(lines[7], "RAM COMMIT MIB 3072");
        assert!(lines.iter().all(|line| line.is_ascii()));
    }

    #[test]
    fn gpu_usage_sums_processes_on_one_adapter_then_uses_busiest_engine() {
        let mut engines = HashMap::new();
        let adapter = "luid_0x00000000_0x000012ab_";
        add_engine(&mut engines, adapter, "pid_1_luid_0x00000000_0x000012AB_phys_0_eng_0_engtype_3D", 40.0);
        add_engine(&mut engines, adapter, "pid_2_luid_0x00000000_0x000012ab_phys_0_eng_0_engtype_3D", 25.0);
        add_engine(&mut engines, adapter, "pid_1_luid_0x00000000_0x000012ab_phys_0_eng_1_engtype_Copy", 10.0);
        add_engine(&mut engines, adapter, "pid_1_luid_0x00000000_0x00009999_phys_0_eng_0_engtype_3D", 99.0);
        add_engine(&mut engines, adapter, "pid_3_luid_0x00000000_0x000012ab_phys_0_eng_0_engtype_3D", f64::NAN);
        assert_eq!(engines.len(), 2);
        assert_eq!(engines.values().copied().reduce(f64::max), Some(65.0));
    }
}
