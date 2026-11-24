//! Optional console mirror. Only the output thread touches the console; producers
//! drop on a full or contended queue instead of waiting for a slow terminal.

use super::Level;
use std::collections::VecDeque;
use std::io::Write;
use std::sync::atomic::{AtomicIsize, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::thread::{self, Thread};
use std::time::Duration;
use windows::Win32::Graphics::Gdi::{GetMonitorInfoW, MonitorFromWindow, MONITORINFO, MONITOR_DEFAULTTONEAREST};
use windows_sys::Win32::Foundation::{CloseHandle, GENERIC_READ, GENERIC_WRITE, HANDLE, INVALID_HANDLE_VALUE, RECT};
use windows_sys::Win32::Storage::FileSystem::{CreateFileW, FILE_SHARE_READ, FILE_SHARE_WRITE, OPEN_EXISTING};
use windows_sys::Win32::System::Console::*;
use windows_sys::Win32::UI::WindowsAndMessaging::{GetWindowRect, SetWindowPos, SWP_NOACTIVATE, SWP_NOZORDER};

const CAPACITY: usize = 1024;
static CONSOLE: OnceLock<Option<Arc<Queue>>> = OnceLock::new();

struct Queue {
    pending: Mutex<VecDeque<(Level, String)>>,
    dropped: AtomicU64,
    game_window: AtomicIsize,
    worker: OnceLock<Thread>,
}

impl Queue {
    fn new() -> Self {
        Self {
            pending: Mutex::new(VecDeque::with_capacity(CAPACITY)),
            dropped: AtomicU64::new(0),
            game_window: AtomicIsize::new(0),
            worker: OnceLock::new(),
        }
    }

    fn push(&self, level: Level, line: String) {
        let queued = if let Ok(mut pending) = self.pending.try_lock() {
            if pending.len() < CAPACITY {
                pending.push_back((level, line));
                true
            } else {
                false
            }
        } else {
            false
        };
        if !queued {
            self.dropped.fetch_add(1, Ordering::Relaxed);
        }
        self.wake();
    }

    fn wake(&self) {
        if let Some(worker) = self.worker.get() {
            worker.unpark();
        }
    }
}

pub(super) fn init() {
    CONSOLE.get_or_init(|| {
        if std::env::var("XCL_CONSOLE").as_deref() != Ok("1") {
            return None;
        }
        let queue = Arc::new(Queue::new());
        let output = Arc::clone(&queue);
        let worker = thread::Builder::new().name("xcl-console".into())
            .spawn(move || run(output)).ok()?;
        let _ = queue.worker.set(worker.thread().clone());
        Some(queue)
    });
}

pub(super) fn write(level: Level, line: String) -> bool {
    if let Some(Some(queue)) = CONSOLE.get() {
        queue.push(level, line);
        true
    } else {
        false
    }
}

pub(super) fn position(game_window: isize) {
    if let Some(Some(queue)) = CONSOLE.get() {
        queue.game_window.store(game_window, Ordering::Relaxed);
        queue.wake();
    }
}

pub(super) fn dropped_lines() -> u64 {
    CONSOLE.get().and_then(Option::as_ref)
        .map_or(0, |queue| queue.dropped.load(Ordering::Relaxed))
}

fn run(queue: Arc<Queue>) {
    let output = ConsoleOutput::open();
    let mut batch = VecDeque::with_capacity(CAPACITY);
    let mut reported_drops = 0;
    loop {
        let game_window = queue.game_window.swap(0, Ordering::Relaxed);
        if game_window != 0 {
            unsafe { place_console(game_window); }
        }
        if let Ok(mut pending) = queue.pending.lock() {
            std::mem::swap(&mut *pending, &mut batch);
        }
        for (level, line) in batch.drain(..) {
            output.write(level, &line);
        }
        let drops = queue.dropped.load(Ordering::Relaxed);
        if drops != reported_drops {
            output.write(Level::Warn, &format!(
                "xcl: console dropped {} log lines ({} total); file log is unaffected\n",
                drops - reported_drops, drops,
            ));
            reported_drops = drops;
        }
        thread::park_timeout(Duration::from_millis(40));
    }
}

struct ConsoleOutput {
    handle: HANDLE,
    attributes: Option<u16>,
}

impl ConsoleOutput {
    fn open() -> Self {
        unsafe {
            if GetConsoleWindow().is_null() {
                let _ = AllocConsole();
            }
            // CONOUT$ bypasses stdout/stderr redirection and is passed through by
            // the VFS, so a parent can still capture the normal log file.
            let name: Vec<u16> = "CONOUT$\0".encode_utf16().collect();
            let handle = CreateFileW(name.as_ptr(), GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, std::ptr::null(), OPEN_EXISTING,
                0, std::ptr::null_mut());
            let mut info: CONSOLE_SCREEN_BUFFER_INFO = std::mem::zeroed();
            let attributes = (handle != INVALID_HANDLE_VALUE
                && GetConsoleScreenBufferInfo(handle, &mut info) != 0).then_some(info.wAttributes);
            Self { handle, attributes }
        }
    }

    fn write(&self, level: Level, line: &str) {
        if self.handle == INVALID_HANDLE_VALUE {
            let _ = std::io::stderr().write_all(line.as_bytes());
            return;
        }
        unsafe {
            if let Some(attributes) = self.attributes {
                let foreground = match level {
                    Level::Error => FOREGROUND_RED | FOREGROUND_INTENSITY,
                    Level::Warn => FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
                    Level::Info => attributes & 0x0f,
                    Level::Debug => FOREGROUND_GREEN | FOREGROUND_BLUE,
                    Level::Trace => FOREGROUND_INTENSITY,
                };
                let _ = SetConsoleTextAttribute(self.handle, (attributes & !0x0f) | foreground);
            }
            let wide: Vec<u16> = line.replace('\n', "\r\n").encode_utf16().collect();
            let mut remaining = wide.as_slice();
            while !remaining.is_empty() {
                let mut written = 0;
                let count = remaining.len().min(16_384) as u32;
                if WriteConsoleW(self.handle, remaining.as_ptr(), count, &mut written,
                    std::ptr::null()) == 0 || written == 0 {
                    break;
                }
                remaining = &remaining[written as usize..];
            }
            if let Some(attributes) = self.attributes {
                let _ = SetConsoleTextAttribute(self.handle, attributes);
            }
        }
    }
}

impl Drop for ConsoleOutput {
    fn drop(&mut self) {
        if self.handle != INVALID_HANDLE_VALUE {
            unsafe { CloseHandle(self.handle); }
        }
    }
}

unsafe fn place_console(game_window: isize) {
    let console = unsafe { GetConsoleWindow() };
    if console.is_null() {
        return;
    }
    let game = game_window as *mut std::ffi::c_void;
    let mut rect: RECT = unsafe { std::mem::zeroed() };
    if unsafe { GetWindowRect(game, &mut rect) } == 0 {
        return;
    }
    let monitor = unsafe {
        MonitorFromWindow(windows::Win32::Foundation::HWND(game), MONITOR_DEFAULTTONEAREST)
    };
    let mut info = MONITORINFO { cbSize: std::mem::size_of::<MONITORINFO>() as u32, ..Default::default() };
    if !unsafe { GetMonitorInfoW(monitor, &mut info) }.as_bool() {
        return;
    }
    let work = info.rcWork;
    let width = 640.min(work.right - work.left);
    let height = (rect.bottom - rect.top).max(240).min(work.bottom - work.top);
    let x = if rect.right + 8 + width <= work.right {
        rect.right + 8
    } else if rect.left - 8 - width >= work.left {
        rect.left - 8 - width
    } else {
        work.right - width
    };
    let y = rect.top.clamp(work.top, work.bottom - height);
    unsafe { SetWindowPos(console, std::ptr::null_mut(), x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER); }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn console_queue_preserves_order_and_level() {
        let queue = Queue::new();
        queue.push(Level::Warn, "first".into());
        queue.push(Level::Debug, "second".into());
        let mut pending = queue.pending.lock().unwrap();
        assert_eq!(pending.pop_front(), Some((Level::Warn, "first".into())));
        assert_eq!(pending.pop_front(), Some((Level::Debug, "second".into())));
        assert_eq!(queue.dropped.load(Ordering::Relaxed), 0);
    }

    #[test]
    fn console_queue_counts_full_and_contended_drops() {
        let queue = Queue::new();
        for _ in 0..CAPACITY {
            queue.push(Level::Info, "line".into());
        }
        queue.push(Level::Error, "full".into());
        {
            let pending = queue.pending.lock().unwrap();
            queue.push(Level::Info, "busy".into());
            assert_eq!(pending.len(), CAPACITY);
        }
        assert_eq!(queue.dropped.load(Ordering::Relaxed), 2);
        queue.pending.lock().unwrap().pop_front();
        queue.push(Level::Warn, "resumed".into());
        assert_eq!(queue.pending.lock().unwrap().back(), Some(&(Level::Warn, "resumed".into())));
        assert_eq!(queue.dropped.load(Ordering::Relaxed), 2);
    }
}
