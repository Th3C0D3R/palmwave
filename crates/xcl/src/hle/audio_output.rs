//! Opt-in observation of Windows audio output. This never creates or changes an audio stream.
//! Meter readings cover the previous device period, so polling can miss short peaks. Endpoint
//! readings include every application and precede endpoint volume/mute; they do not prove audibility.

use std::sync::Once;
use std::time::{Duration, Instant};
use windows::core::{HRESULT, Interface};
use windows::Win32::Media::Audio::{
    eConsole, eRender, AudioSessionStateActive, IAudioSessionControl2, IAudioSessionManager2,
    IMMDevice, IMMDeviceEnumerator, ISimpleAudioVolume, MMDeviceEnumerator, DEVICE_STATE_ACTIVE,
};
use windows::Win32::Media::Audio::Endpoints::{IAudioEndpointVolume, IAudioMeterInformation};
use windows::Win32::System::Com::{CoCreateInstance, CoInitializeEx, CoTaskMemFree, CoUninitialize, CLSCTX_ALL, COINIT_MULTITHREADED};

const REPORT_INTERVAL: Duration = Duration::from_secs(10);
const REFRESH_INTERVAL: Duration = Duration::from_secs(1);
const POLL_INTERVAL: Duration = Duration::from_millis(10);
const MAX_SESSIONS: usize = 64;

#[derive(Default, Debug)]
struct Peak {
    maximum: Option<f32>,
    valid: u64,
    unavailable: u64,
}

impl Peak {
    fn record(&mut self, value: Option<f32>) {
        if let Some(v) = value.filter(|v| v.is_finite() && (0.0..=1.0).contains(v)) {
            self.maximum = Some(self.maximum.map_or(v, |old| old.max(v)));
            self.valid += 1;
        } else {
            self.unavailable += 1;
        }
    }

    fn describe(&self) -> String {
        let value = self.maximum.map_or_else(|| "unavailable".into(), |v| format!("{v:.6}"));
        format!("{value} (valid={}, unavailable/invalid={})", self.valid, self.unavailable)
    }
}

struct Window {
    start: Instant,
    endpoint: Peak,
    process: Peak,
}

impl Window {
    fn new(start: Instant) -> Self {
        Self { start, endpoint: Peak::default(), process: Peak::default() }
    }

    fn take_if_due(&mut self, now: Instant) -> Option<(Peak, Peak)> {
        if now.saturating_duration_since(self.start) < REPORT_INTERVAL {
            return None;
        }
        self.start = now;
        Some((std::mem::take(&mut self.endpoint), std::mem::take(&mut self.process)))
    }
}

struct Session {
    meter: windows::core::Result<IAudioMeterInformation>,
    active: Option<bool>,
    is_default: Option<bool>,
    volume: Option<(f32, bool)>,
}

struct Snapshot {
    endpoint: windows::core::Result<IAudioMeterInformation>,
    volume: String,
    sessions: Vec<Session>,
    shared_excluded: u32,
    scan_error: Option<String>,
}

unsafe fn device_id(device: &IMMDevice) -> Option<String> {
    let id = device.GetId().ok()?;
    let result = id.to_string().ok();
    CoTaskMemFree(Some(id.0.cast()));
    result
}

fn owned_session(status: HRESULT, pid: u32, own_pid: u32) -> windows::core::Result<bool> {
    status.ok()?;
    Ok(status.0 == 0 && pid == own_pid)
}

impl Snapshot {
    unsafe fn refresh(enumerator: &IMMDeviceEnumerator) -> Self {
        let device = enumerator.GetDefaultAudioEndpoint(eRender, eConsole);
        let default_id = device.as_ref().ok().and_then(|d| device_id(d));
        let volume = match &device {
            Ok(d) => match d.Activate::<IAudioEndpointVolume>(CLSCTX_ALL, None) {
                Ok(v) => format!("level={:?}, muted={:?}", v.GetMasterVolumeLevelScalar(), v.GetMute().map(|m| m.as_bool())),
                Err(e) => format!("unavailable ({e})"),
            },
            Err(e) => format!("no default endpoint ({e})"),
        };
        let endpoint = device.and_then(|d| d.Activate::<IAudioMeterInformation>(CLSCTX_ALL, None));
        let mut snapshot = Self { endpoint, volume, sessions: Vec::new(), shared_excluded: 0, scan_error: None };
        let scan = (|| -> windows::core::Result<()> {
            let devices = enumerator.EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE)?;
            for i in 0..devices.GetCount()? {
                // Replace this bounded snapshot every second; never accumulate expired sessions.
                let result = (|| -> windows::core::Result<()> {
                    let device = devices.Item(i)?;
                    let id = device_id(&device);
                    let is_default = id.as_ref().zip(default_id.as_ref()).map(|(a, b)| a == b);
                    let manager: IAudioSessionManager2 = device.Activate(CLSCTX_ALL, None)?;
                    let sessions = manager.GetSessionEnumerator()?;
                    for j in 0..sessions.GetCount()? {
                        let control: IAudioSessionControl2 = sessions.GetSession(j)?.cast()?;
                        // The projected method discards AUDCLNT_S_NO_SINGLE_PROCESS. Shared
                        // sessions cannot be attributed to this process, even when their PID matches.
                        let mut pid = 0;
                        let hr = (control.vtable().GetProcessId)(control.as_raw(), &mut pid);
                        if hr.is_ok() && hr.0 != 0 {
                            snapshot.shared_excluded += 1;
                        }
                        if !owned_session(hr, pid, std::process::id())? {
                            continue;
                        }
                        if snapshot.sessions.len() == MAX_SESSIONS {
                            snapshot.scan_error = Some(format!("session limit {MAX_SESSIONS} reached; readings incomplete"));
                            return Ok(());
                        }
                        snapshot.sessions.push(Session {
                            meter: control.cast(),
                            active: control.GetState().ok().map(|s| s == AudioSessionStateActive),
                            is_default,
                            volume: control.cast::<ISimpleAudioVolume>().ok().and_then(|v| Some((v.GetMasterVolume().ok()?, v.GetMute().ok()?.as_bool()))),
                        });
                    }
                    Ok(())
                })();
                if let Err(e) = result {
                    snapshot.scan_error = Some(format!("session discovery incomplete ({e})"));
                }
            }
            Ok(())
        })();
        if let Err(e) = scan {
            snapshot.scan_error = Some(format!("session discovery unavailable ({e})"));
        }
        snapshot
    }

    unsafe fn sample(&self, window: &mut Window) {
        window.endpoint.record(self.endpoint.as_ref().ok().and_then(|m| m.GetPeakValue().ok()));
        if self.sessions.is_empty() || self.scan_error.is_some() {
            window.process.record(None);
        }
        for session in &self.sessions {
            window.process.record(session.meter.as_ref().ok().and_then(|m| m.GetPeakValue().ok()));
        }
    }

    fn report(&self, endpoint: Peak, process: Peak) {
        let active = self.sessions.iter().filter(|s| s.active == Some(true)).count();
        let unknown = self.sessions.iter().filter(|s| s.active.is_none()).count();
        let missing = self.sessions.iter().filter(|s| s.meter.is_err()).count();
        let nondefault = self.sessions.iter().filter(|s| s.is_default == Some(false)).count();
        let route_unknown = self.sessions.iter().filter(|s| s.is_default.is_none()).count();
        let muted = self.sessions.iter().filter(|s| s.volume.is_some_and(|(v, m)| m || v == 0.0)).count();
        let volume_unknown = self.sessions.iter().filter(|s| s.volume.is_none()).count();
        let discovery = self.scan_error.as_deref().unwrap_or(if self.sessions.is_empty() { "no process sessions" } else { "ok" });
        let status = self.endpoint.as_ref().map(|_| "available".into()).unwrap_or_else(|e| format!("unavailable ({e})"));
        crate::xlog_info!(
            "audio-output: sampled WASAPI peaks (10 ms poll, 10 s window): process-session-max={} [current sessions={}, active={}, nondefault={}, route-unknown={}, muted-or-zero-volume={}, volume-unavailable={}, state-unavailable={}, meter-unavailable={}, shared-excluded={}, discovery={}]; default eConsole endpoint={} [all apps, before endpoint volume; current meter={}, {}]",
            process.describe(), self.sessions.len(), active, nondefault, route_unknown, muted, volume_unknown, unknown, missing, self.shared_excluded, discovery, endpoint.describe(), status, self.volume
        );
    }
}

/// Starts one observer on its own COM apartment when XCL_AUDIO_PEAK=1.
pub fn start() {
    if std::env::var("XCL_AUDIO_PEAK").as_deref() != Ok("1") {
        return;
    }
    static START: Once = Once::new();
    START.call_once(|| {
        if let Err(e) = std::thread::Builder::new().name("xcl-audio-peak".into()).spawn(|| unsafe { observe() }) {
            crate::xlog_warn!("audio-output: cannot start meter worker ({e})");
        }
    });
}

unsafe fn observe() {
    let init = CoInitializeEx(None, COINIT_MULTITHREADED);
    if init.is_err() {
        crate::xlog_warn!("audio-output: COM initialization failed ({init:?}); meters unavailable");
        return;
    }
    struct Apartment;
    impl Drop for Apartment {
        fn drop(&mut self) { unsafe { CoUninitialize() } }
    }
    let _apartment = Apartment;
    let enumerator: IMMDeviceEnumerator = match CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL) {
        Ok(e) => e,
        Err(e) => {
            crate::xlog_warn!("audio-output: device enumeration unavailable ({e})");
            return;
        }
    };
    crate::xlog_info!("audio-output: observing current-process sessions on active render devices and default eConsole endpoint; sampled peaks do not prove speaker audibility");
    let mut refreshed = Instant::now();
    let mut window = Window::new(refreshed);
    let mut snapshot = Snapshot::refresh(&enumerator);
    loop {
        let now = Instant::now();
        if now.duration_since(refreshed) >= REFRESH_INTERVAL {
            snapshot = Snapshot::refresh(&enumerator);
            refreshed = now;
        }
        snapshot.sample(&mut window);
        if let Some((endpoint, process)) = window.take_if_due(now) {
            snapshot.report(endpoint, process);
        }
        std::thread::sleep(POLL_INTERVAL);
    }
}

/// Hardware-test helper; caller initializes COM and owns the synthetic stream.
#[cfg(test)]
pub(crate) unsafe fn current_process_peak() -> Result<Option<f32>, String> {
    let enumerator: IMMDeviceEnumerator = CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL).map_err(|e| e.to_string())?;
    let snapshot = Snapshot::refresh(&enumerator);
    if let Some(e) = snapshot.scan_error {
        return Err(e);
    }
    let mut peak = Peak::default();
    for session in snapshot.sessions {
        let meter = session.meter.map_err(|e| e.to_string())?;
        peak.record(Some(meter.GetPeakValue().map_err(|e| e.to_string())?));
    }
    if peak.unavailable != 0 {
        return Err("invalid process meter reading".into());
    }
    Ok(peak.maximum)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_single_process_sessions_are_attributed() {
        assert!(owned_session(HRESULT(0), 42, 42).unwrap());
        assert!(!owned_session(HRESULT(0), 43, 42).unwrap());
        // Unlike the projected Result<u32>, retain nonzero success (a shared session).
        assert!(!owned_session(HRESULT(1), 42, 42).unwrap());
        assert!(owned_session(HRESULT(0x80004005u32 as i32), 42, 42).is_err());
    }

    #[test]
    fn peak_window_keeps_maximum_and_resets() {
        let now = Instant::now();
        let mut window = Window::new(now);
        for value in [0.1, 0.8, 0.4] { window.endpoint.record(Some(value)); }
        window.process.record(Some(0.3));
        let (endpoint, process) = window.take_if_due(now + REPORT_INTERVAL).unwrap();
        assert_eq!((endpoint.maximum, endpoint.valid), (Some(0.8), 3));
        assert_eq!(process.maximum, Some(0.3));
        assert_eq!((window.endpoint.maximum, window.process.maximum), (None, None));
        window.endpoint.record(Some(0.0));
        assert_eq!(window.take_if_due(now + REPORT_INTERVAL * 2).unwrap().0.maximum, Some(0.0));
    }

    #[test]
    fn unavailable_and_invalid_readings_are_not_silence() {
        let mut peak = Peak::default();
        for value in [None, Some(f32::NAN), Some(f32::INFINITY), Some(-0.1), Some(1.1)] { peak.record(value); }
        assert_eq!((peak.maximum, peak.valid, peak.unavailable), (None, 0, 5));
        assert!(peak.describe().starts_with("unavailable"));
        peak.record(Some(0.0));
        peak.record(Some(1.0));
        assert_eq!((peak.maximum, peak.valid, peak.unavailable), (Some(1.0), 2, 5));
    }

    #[test]
    fn report_is_due_only_after_ten_seconds() {
        let now = Instant::now();
        let mut window = Window::new(now);
        assert!(window.take_if_due(now + REPORT_INTERVAL - Duration::from_nanos(1)).is_none());
        assert!(window.take_if_due(now + REPORT_INTERVAL).is_some());
        assert!(window.take_if_due(now + REPORT_INTERVAL).is_none());
        assert!(window.take_if_due(now + REPORT_INTERVAL * 3).is_some());
    }
}
