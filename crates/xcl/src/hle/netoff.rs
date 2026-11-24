//! `XCL_NETWORK=0`: the title runs as on a console without a network. Its WinHTTP requests and
//! its sockets fail as they do there (name not resolved / network unreachable); nothing is
//! answered in a server's place. Loopback connections stay (titles make socket pairs), and so
//! do lookups of the local names (none, this machine's, localhost). Only
//! the title's own imports are concerned (the game's exe and DLLs); off by default.
//!
//! For titles under bring-up: a crash reporter otherwise uploads a memory dump of the process
//! on every crash.
//!
//! The title's imports are not the only way out: a retail game's crash reporter reached the
//! system's WinHTTP another way and uploaded a 21.5 MB minidump with `XCL_NETWORK=0` (#305).
//! So the same refusals are also hooked into the system's own winhttp / wininet / ws2_32
//! (`install_system_hooks`): whatever finds those functions – an import, GetProcAddress, a DLL
//! of the title's – gets no network. What goes through (loopback, local names) calls the
//! original through the hook's trampoline.
//!
//! Nothing receives from the network either: a bind to a network address (the wildcard 0.0.0.0
//! or :: among them) is made on loopback, 127.0.0.1 or ::1 with the same port, and so is the
//! implicit bind of an unbound socket's sendto (Winsock's takes 0.0.0.0); a listen on a socket
//! bound to the network some other way fails. Otherwise Windows Defender Firewall asks an
//! administrator to let each new exe path listen. A receive on an unbound socket binds nothing
//! (it fails with WSAEINVAL).

use std::ffi::c_void;
use std::sync::atomic::{AtomicUsize, Ordering};

use windows_sys::Win32::Foundation::SetLastError;

pub const NETOFF: &str = "xcl-netoff.dll";

const ERROR_WINHTTP_NAME_NOT_RESOLVED: u32 = 12007;
const WSAEINVAL: i32 = 10022;
const WSAENETUNREACH: i32 = 10051;
const WSAHOST_NOT_FOUND: i32 = 11001;

#[link(name = "ws2_32", kind = "raw-dylib")]
extern "system" {
    #[link_name = "connect"]
    fn real_connect(s: usize, name: *const u8, len: i32) -> i32;
    #[link_name = "sendto"]
    fn real_sendto(s: usize, buf: *const u8, len: i32, flags: i32, to: *const u8, tolen: i32) -> i32;
    #[link_name = "bind"]
    fn real_bind(s: usize, name: *const u8, len: i32) -> i32;
    #[link_name = "listen"]
    fn real_listen(s: usize, backlog: i32) -> i32;
    fn getsockname(s: usize, name: *mut u8, len: *mut i32) -> i32;
    fn WSAGetLastError() -> i32;
    fn WSASetLastError(e: i32);
    #[link_name = "getaddrinfo"]
    fn real_getaddrinfo(node: *const u8, service: *const u8, hints: *const c_void, out: *mut *mut c_void) -> i32;
    #[link_name = "GetAddrInfoW"]
    fn real_getaddrinfo_w(node: *const u16, service: *const u16, hints: *const c_void, out: *mut *mut c_void) -> i32;
    #[link_name = "gethostname"]
    fn real_gethostname(name: *mut u8, len: i32) -> i32;
}

/// A name that resolves without a network: none (the local addresses), this machine's own
/// name, localhost – a console without a network still answers these (a retail game asks for
/// its own name and none at start).
fn local_name(name: &str) -> bool {
    if name.is_empty() || name.eq_ignore_ascii_case("localhost") || name.starts_with("127.") || name == "::1" {
        return true;
    }
    let mut buf = [0u8; 256];
    let own = unsafe { real_gethostname(buf.as_mut_ptr(), buf.len() as i32) } == 0;
    own && {
        let n = buf.iter().position(|&b| b == 0).unwrap_or(0);
        std::str::from_utf8(&buf[..n]).is_ok_and(|h| h.eq_ignore_ascii_case(name))
    }
}

pub fn enabled() -> bool {
    std::env::var("XCL_NETWORK").is_ok_and(|v| v.trim() == "0")
}

/// (name, address, ws2_32 ordinal – 0: imported by name only)
fn table() -> Vec<(&'static str, usize, u16)> {
    vec![
        ("WinHttpSendRequest", send_request as *const () as usize, 0),
        ("connect", connect as *const () as usize, 4),
        ("sendto", sendto as *const () as usize, 20),
        ("bind", bind as *const () as usize, 2),
        ("listen", listen as *const () as usize, 13),
        ("gethostbyname", gethostbyname as *const () as usize, 52),
        ("WSAConnect", wsa_connect as *const () as usize, 0),
        ("WSASendTo", wsa_send_to as *const () as usize, 0),
        ("getaddrinfo", getaddrinfo as *const () as usize, 0),
        ("GetAddrInfoW", getaddrinfo_w as *const () as usize, 0),
        ("GetAddrInfoExW", getaddrinfo_ex as *const () as usize, 0),
    ]
}

pub fn exports() -> Vec<(&'static str, usize)> {
    table().into_iter().map(|(n, a, _)| (n, a)).collect()
}

pub fn ordinals() -> Vec<(u16, &'static str)> {
    table().into_iter().filter(|t| t.2 != 0).map(|(n, _, o)| (o, n)).collect()
}

fn note(what: &str) {
    static N: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
    if N.fetch_add(1, std::sync::atomic::Ordering::Relaxed) < 16 {
        crate::xlog_info!("XCL_NETWORK=0: {what} fails (no network)");
    }
}

/// What is done in the title's place rather than refused: each logged once (16 at most, apart
/// from `note`'s).
fn note_once(text: String) {
    static SEEN: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());
    let mut seen = SEEN.lock().unwrap_or_else(|e| e.into_inner());
    if seen.len() < 16 && !seen.contains(&text) {
        crate::xlog_info!("XCL_NETWORK=0: {text}");
        seen.push(text);
    }
}

/// 127.0.0.0/8 or ::1 (sockaddr_in / sockaddr_in6)
unsafe fn loopback(name: *const u8, len: i32) -> bool {
    if name.is_null() || len < 8 {
        return false;
    }
    match (name as *const u16).read_unaligned() {
        2 => *name.add(4) == 127,
        23 if len >= 24 => {
            let a = std::slice::from_raw_parts(name.add(8), 16);
            a[..15].iter().all(|&b| b == 0) && a[15] == 1
        }
        _ => false,
    }
}

/// `name` (sockaddr_in / sockaddr_in6) with a loopback address in place of its own: 127.0.0.1,
/// ::1, or for an IPv4 address on a dual-stack socket the mapped 127.0.0.1. The port and the
/// rest are kept but an IPv6 scope (::1 takes none: WSAEADDRNOTAVAIL). None: on loopback
/// already, or not an IPv4 / IPv6 address.
unsafe fn to_loopback(name: *const u8, len: i32) -> Option<[u8; 128]> {
    let n = len.clamp(0, 128);
    if name.is_null() || loopback(name, n) {
        return None;
    }
    let mut a = [0u8; 128];
    std::ptr::copy_nonoverlapping(name, a.as_mut_ptr(), n as usize);
    match u16::from_le_bytes([a[0], a[1]]) {
        2 if n >= 16 => a[4..8].copy_from_slice(&[127, 0, 0, 1]),
        23 if n >= 24 => {
            let mapped = a[8..18] == [0; 10] && a[18..20] == [0xff; 2];
            match mapped {
                true if a[20] == 127 => return None,
                true => a[20..24].copy_from_slice(&[127, 0, 0, 1]),
                false => a[8..24].copy_from_slice(&std::net::Ipv6Addr::LOCALHOST.octets()),
            }
            a[24..28].fill(0);
        }
        _ => return None,
    }
    Some(a)
}

/// a sockaddr_in / sockaddr_in6 as address:port, for the log
unsafe fn socket_addr(a: *const u8) -> std::net::SocketAddr {
    let port = u16::from_be_bytes([*a.add(2), *a.add(3)]);
    match *a {
        2 => (std::net::Ipv4Addr::from(*(a.add(4) as *const [u8; 4])), port).into(),
        _ => (std::net::Ipv6Addr::from(*(a.add(8) as *const [u8; 16])), port).into(),
    }
}

unsafe fn unreachable_net(what: &str) -> i32 {
    note(what);
    WSASetLastError(WSAENETUNREACH);
    -1
}

/// the originals of the system functions hooked (`install_system_hooks`; 0: not hooked – the
/// linked import is the original)
static O_CONNECT: AtomicUsize = AtomicUsize::new(0);
static O_SENDTO: AtomicUsize = AtomicUsize::new(0);
static O_BIND: AtomicUsize = AtomicUsize::new(0);
static O_LISTEN: AtomicUsize = AtomicUsize::new(0);
static O_GETADDRINFO: AtomicUsize = AtomicUsize::new(0);
static O_GETADDRINFO_W: AtomicUsize = AtomicUsize::new(0);
/// trampolines of hooks whose original is never called
static O_UNUSED: [AtomicUsize; 16] = [const { AtomicUsize::new(0) }; 16];

unsafe fn orig_connect(s: usize, name: *const u8, len: i32) -> i32 {
    match O_CONNECT.load(Ordering::Acquire) {
        0 => real_connect(s, name, len),
        f => std::mem::transmute::<usize, unsafe extern "system" fn(usize, *const u8, i32) -> i32>(f)(s, name, len),
    }
}

unsafe fn orig_sendto(s: usize, buf: *const u8, len: i32, flags: i32, to: *const u8, tolen: i32) -> i32 {
    match O_SENDTO.load(Ordering::Acquire) {
        0 => real_sendto(s, buf, len, flags, to, tolen),
        f => std::mem::transmute::<usize, unsafe extern "system" fn(usize, *const u8, i32, i32, *const u8, i32) -> i32>(f)(s, buf, len, flags, to, tolen),
    }
}

unsafe fn orig_bind(s: usize, name: *const u8, len: i32) -> i32 {
    match O_BIND.load(Ordering::Acquire) {
        0 => real_bind(s, name, len),
        f => std::mem::transmute::<usize, unsafe extern "system" fn(usize, *const u8, i32) -> i32>(f)(s, name, len),
    }
}

unsafe fn orig_listen(s: usize, backlog: i32) -> i32 {
    match O_LISTEN.load(Ordering::Acquire) {
        0 => real_listen(s, backlog),
        f => std::mem::transmute::<usize, unsafe extern "system" fn(usize, i32) -> i32>(f)(s, backlog),
    }
}

unsafe fn orig_getaddrinfo(node: *const u8, service: *const u8, hints: *const c_void, out: *mut *mut c_void) -> i32 {
    match O_GETADDRINFO.load(Ordering::Acquire) {
        0 => real_getaddrinfo(node, service, hints, out),
        f => std::mem::transmute::<usize, unsafe extern "system" fn(*const u8, *const u8, *const c_void, *mut *mut c_void) -> i32>(f)(node, service, hints, out),
    }
}

unsafe fn orig_getaddrinfo_w(node: *const u16, service: *const u16, hints: *const c_void, out: *mut *mut c_void) -> i32 {
    match O_GETADDRINFO_W.load(Ordering::Acquire) {
        0 => real_getaddrinfo_w(node, service, hints, out),
        f => std::mem::transmute::<usize, unsafe extern "system" fn(*const u16, *const u16, *const c_void, *mut *mut c_void) -> i32>(f)(node, service, hints, out),
    }
}

/// `XCL_NETWORK=0` for the whole process: the system's own network functions refuse as the
/// title's imports do (see the module's comment). Call once, before the title runs.
pub fn install_system_hooks() {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| unsafe {
        let mut unused = O_UNUSED.iter();
        let mut spare = || unused.next().expect("enough spare trampolines");
        let hooks: [(&str, &str, usize, &AtomicUsize); 20] = [
            ("winhttp.dll", "WinHttpSendRequest", send_request as usize, spare()),
            ("winhttp.dll", "WinHttpConnect", null_handle_winhttp as usize, spare()),
            ("wininet.dll", "InternetConnectA", null_handle_inet8 as usize, spare()),
            ("wininet.dll", "InternetConnectW", null_handle_inet8 as usize, spare()),
            ("wininet.dll", "InternetOpenUrlA", null_handle_inet6 as usize, spare()),
            ("wininet.dll", "InternetOpenUrlW", null_handle_inet6 as usize, spare()),
            ("wininet.dll", "HttpSendRequestA", false_inet5 as usize, spare()),
            ("wininet.dll", "HttpSendRequestW", false_inet5 as usize, spare()),
            ("wininet.dll", "HttpSendRequestExA", false_inet5 as usize, spare()),
            ("wininet.dll", "HttpSendRequestExW", false_inet5 as usize, spare()),
            ("ws2_32.dll", "connect", connect as usize, &O_CONNECT),
            ("ws2_32.dll", "WSAConnect", wsa_connect as usize, spare()),
            ("ws2_32.dll", "sendto", sendto as usize, &O_SENDTO),
            ("ws2_32.dll", "WSASendTo", wsa_send_to as usize, spare()),
            ("ws2_32.dll", "bind", bind as usize, &O_BIND),
            ("ws2_32.dll", "listen", listen as usize, &O_LISTEN),
            ("ws2_32.dll", "gethostbyname", gethostbyname as usize, spare()),
            ("ws2_32.dll", "getaddrinfo", getaddrinfo as usize, &O_GETADDRINFO),
            ("ws2_32.dll", "GetAddrInfoW", getaddrinfo_w as usize, &O_GETADDRINFO_W),
            ("ws2_32.dll", "GetAddrInfoExW", getaddrinfo_ex as usize, spare()),
        ];
        let mut failed = Vec::new();
        for (dll, name, detour, orig) in hooks {
            if let Err(e) = crate::hooks::hook(dll, name, detour, orig) {
                failed.push(e);
            }
        }
        if failed.is_empty() {
            crate::xlog_info!("XCL_NETWORK=0: the system's WinHTTP, WinINet and Winsock refuse the network for the whole process (loopback and local names excepted; binds are made on loopback)");
        } else {
            crate::xlog_warn!("XCL_NETWORK=0: not every system network function could be hooked – {}", failed.join("; "));
        }
    });
}

unsafe extern "system" fn null_handle_winhttp(_session: *mut c_void, _server: *const u16, _port: u16, _reserved: u32) -> *mut c_void {
    note("WinHttpConnect");
    SetLastError(ERROR_WINHTTP_NAME_NOT_RESOLVED);
    std::ptr::null_mut()
}

/// InternetConnectA/W (8 arguments): no connection (ERROR_INTERNET_NAME_NOT_RESOLVED – the
/// same number as WinHTTP's)
unsafe extern "system" fn null_handle_inet8(_h: *mut c_void, _server: *const c_void, _port: u16, _user: *const c_void, _pass: *const c_void, _service: u32, _flags: u32, _ctx: usize) -> *mut c_void {
    note("InternetConnect");
    SetLastError(ERROR_WINHTTP_NAME_NOT_RESOLVED);
    std::ptr::null_mut()
}

/// InternetOpenUrlA/W (6 arguments)
unsafe extern "system" fn null_handle_inet6(_h: *mut c_void, _url: *const c_void, _headers: *const c_void, _hlen: u32, _flags: u32, _ctx: usize) -> *mut c_void {
    note("InternetOpenUrl");
    SetLastError(ERROR_WINHTTP_NAME_NOT_RESOLVED);
    std::ptr::null_mut()
}

/// HttpSendRequestA/W and HttpSendRequestExA/W (5 arguments each): FALSE
unsafe extern "system" fn false_inet5(_req: *mut c_void, _a: *const c_void, _b: usize, _c: *const c_void, _d: usize) -> i32 {
    note("HttpSendRequest");
    SetLastError(ERROR_WINHTTP_NAME_NOT_RESOLVED);
    0
}

unsafe extern "system" fn send_request(_req: *mut c_void, _headers: *const u16, _hlen: u32, _opt: *const c_void, _olen: u32, _total: u32, _ctx: usize) -> i32 {
    note("WinHttpSendRequest");
    SetLastError(ERROR_WINHTTP_NAME_NOT_RESOLVED);
    0
}

unsafe extern "system" fn connect(s: usize, name: *const u8, len: i32) -> i32 {
    if loopback(name, len) {
        return orig_connect(s, name, len);
    }
    unreachable_net("connect")
}

unsafe extern "system" fn sendto(s: usize, buf: *const u8, len: i32, flags: i32, to: *const u8, tolen: i32) -> i32 {
    if to.is_null() || loopback(to, tolen) {
        if !to.is_null() {
            bind_unbound(s, to, tolen);
        }
        return orig_sendto(s, buf, len, flags, to, tolen);
    }
    unreachable_net("sendto")
}

/// An unbound socket sending to loopback (`to`) is bound there first, port 0: the implicit bind
/// of its sendto would take the wildcard address, which receives from the network too.
unsafe fn bind_unbound(s: usize, to: *const u8, tolen: i32) {
    let mut a = [0u8; 128];
    let mut n = a.len() as i32;
    if getsockname(s, a.as_mut_ptr(), &mut n) == 0 || WSAGetLastError() != WSAEINVAL {
        return;
    }
    let n = tolen.min(28);
    std::ptr::copy_nonoverlapping(to, a.as_mut_ptr(), n as usize);
    a[2..4].fill(0);
    a[24..28].fill(0);
    if orig_bind(s, a.as_ptr(), n) == 0 {
        note_once(format!("an unbound socket's sendto binds it to {} first, not to the wildcard address", socket_addr(a.as_ptr()).ip()));
    }
}

/// A bind to a network address – the wildcard (0.0.0.0, ::) among them – is made on loopback,
/// the port kept: the socket still works locally, and the firewall has nothing to ask.
unsafe extern "system" fn bind(s: usize, name: *const u8, len: i32) -> i32 {
    let Some(a) = to_loopback(name, len) else {
        return orig_bind(s, name, len);
    };
    note_once(format!("bind to {} is made on {}", socket_addr(name), socket_addr(a.as_ptr())));
    orig_bind(s, a.as_ptr(), len.min(a.len() as i32))
}

/// A socket bound to the network some other way (before the hooks, or not through `bind`)
/// does not listen there.
unsafe extern "system" fn listen(s: usize, backlog: i32) -> i32 {
    let mut a = [0u8; 128];
    let mut n = a.len() as i32;
    if getsockname(s, a.as_mut_ptr(), &mut n) == 0 && to_loopback(a.as_ptr(), n).is_some() {
        return unreachable_net(&format!("listen on {}", socket_addr(a.as_ptr())));
    }
    orig_listen(s, backlog)
}

unsafe extern "system" fn wsa_connect(s: usize, name: *const u8, len: i32, _a: *const c_void, _b: *mut c_void, _c: *const c_void, _d: *const c_void) -> i32 {
    connect(s, name, len)
}

#[allow(clippy::too_many_arguments)]
unsafe extern "system" fn wsa_send_to(_s: usize, _bufs: *const c_void, _n: u32, _sent: *mut u32, _flags: u32, _to: *const u8, _tolen: i32, _ov: *mut c_void, _cb: *const c_void) -> i32 {
    unreachable_net("WSASendTo")
}

unsafe extern "system" fn gethostbyname(_name: *const u8) -> *mut c_void {
    note("gethostbyname");
    WSASetLastError(WSAHOST_NOT_FOUND);
    std::ptr::null_mut()
}

unsafe extern "system" fn getaddrinfo(node: *const c_void, _service: *const c_void, _hints: *const c_void, out: *mut *mut c_void) -> i32 {
    let name = if node.is_null() { String::new() } else { crate::util::from_ansi_ptr(node as *const u8).unwrap_or_default() };
    if local_name(&name) {
        return orig_getaddrinfo(node as *const u8, _service as *const u8, _hints, out);
    }
    note(&format!("getaddrinfo({name})"));
    if !out.is_null() {
        *out = std::ptr::null_mut();
    }
    WSAHOST_NOT_FOUND
}

unsafe extern "system" fn getaddrinfo_w(node: *const u16, _service: *const u16, _hints: *const c_void, out: *mut *mut c_void) -> i32 {
    let name = if node.is_null() { String::new() } else { crate::util::from_wide_ptr(node).unwrap_or_default() };
    if local_name(&name) {
        return orig_getaddrinfo_w(node, _service, _hints, out);
    }
    note(&format!("GetAddrInfoW({name})"));
    if !out.is_null() {
        *out = std::ptr::null_mut();
    }
    WSAHOST_NOT_FOUND
}

#[allow(clippy::too_many_arguments)]
unsafe extern "system" fn getaddrinfo_ex(_n: *const u16, _s: *const u16, _ns: u32, _id: *const c_void, _hints: *const c_void, out: *mut *mut c_void, _t: *const c_void, _ov: *mut c_void, _cb: *const c_void, _h: *mut c_void) -> i32 {
    note("GetAddrInfoExW");
    if !out.is_null() {
        *out = std::ptr::null_mut();
    }
    WSAHOST_NOT_FOUND
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;
    use std::net::{Ipv4Addr, Ipv6Addr, SocketAddr, SocketAddrV6, TcpListener, TcpStream, UdpSocket};
    use std::os::windows::io::{AsRawSocket, FromRawSocket};

    #[test]
    fn only_loopback_goes_through() {
        let v4 = |a: [u8; 4]| [2, 0, 0, 80, a[0], a[1], a[2], a[3], 0, 0, 0, 0, 0, 0, 0, 0];
        let mut v6 = [0u8; 28];
        v6[0] = 23;
        v6[23] = 1;
        unsafe {
            assert!(loopback(v4([127, 0, 0, 1]).as_ptr(), 16));
            assert!(!loopback(v4([8, 8, 8, 8]).as_ptr(), 16));
            assert!(loopback(v6.as_ptr(), 28));
            v6[8] = 0x20;
            assert!(!loopback(v6.as_ptr(), 28));
            assert_eq!(connect(usize::MAX, v4([8, 8, 8, 8]).as_ptr(), 16), -1);
            assert_eq!(send_request(std::ptr::null_mut(), std::ptr::null(), 0, std::ptr::null(), 0, 0, 0), 0);
        }
    }

    /// With the system hooks, the system's own functions refuse – found the way a crash
    /// reporter can find them, through GetProcAddress, not the title's imports (#305) – and
    /// what may go through still reaches the original.
    #[test]
    fn the_systems_own_network_functions_refuse() {
        use windows_sys::Win32::Foundation::GetLastError;
        use windows_sys::Win32::System::LibraryLoader::{GetProcAddress, LoadLibraryW};
        install_system_hooks();
        unsafe {
            let f = |dll: &str, name: &[u8]| GetProcAddress(LoadLibraryW(crate::util::wide(dll).as_ptr()), name.as_ptr()).expect("export") as usize;
            let send: unsafe extern "system" fn(*mut c_void, *const u16, u32, *const c_void, u32, u32, usize) -> i32 = std::mem::transmute(f("winhttp.dll", b"WinHttpSendRequest\0"));
            SetLastError(0);
            assert_eq!(send(1 as *mut c_void, std::ptr::null(), 0, std::ptr::null(), 0, 0, 0), 0);
            assert_eq!(GetLastError(), ERROR_WINHTTP_NAME_NOT_RESOLVED);
            let open_url: unsafe extern "system" fn(*mut c_void, *const u16, *const u16, u32, u32, usize) -> *mut c_void = std::mem::transmute(f("wininet.dll", b"InternetOpenUrlW\0"));
            assert!(open_url(1 as *mut c_void, crate::util::wide("https://example.com/").as_ptr(), std::ptr::null(), 0, 0, 0).is_null());
            // a name lookup is refused (11001) – a lookup of localhost reaches the original,
            // which answers something else (an address, or 10093 without WSAStartup)
            let gai: unsafe extern "system" fn(*const u8, *const u8, *const c_void, *mut *mut c_void) -> i32 = std::mem::transmute(f("ws2_32.dll", b"getaddrinfo\0"));
            let mut out = std::ptr::null_mut();
            assert_eq!(gai(b"example.com\0".as_ptr(), std::ptr::null(), std::ptr::null(), &mut out), WSAHOST_NOT_FOUND);
            assert!(out.is_null());
            assert_ne!(gai(b"localhost\0".as_ptr(), std::ptr::null(), std::ptr::null(), &mut out), WSAHOST_NOT_FOUND);
        }
    }

    #[link(name = "ws2_32", kind = "raw-dylib")]
    extern "system" {
        fn WSAStartup(version: u16, data: *mut u8) -> i32;
        fn socket(af: i32, kind: i32, protocol: i32) -> usize;
    }

    /// a new socket, not bound yet (std's are), held as std's for its local_addr and its close
    fn unbound<S: FromRawSocket>(af: i32, kind: i32) -> S {
        unsafe {
            WSAStartup(0x202, [0u8; 512].as_mut_ptr());
            let s = socket(af, kind, 0);
            assert_ne!(s, usize::MAX, "socket({af}, {kind})");
            S::from_raw_socket(s as u64)
        }
    }

    fn raw_socket(s: &impl AsRawSocket) -> usize {
        s.as_raw_socket() as usize
    }

    /// `a` as a sockaddr_in / sockaddr_in6, and its length
    fn raw(a: SocketAddr) -> ([u8; 28], i32) {
        let mut b = [0u8; 28];
        b[2..4].copy_from_slice(&a.port().to_be_bytes());
        match a {
            SocketAddr::V4(a) => {
                b[0] = 2;
                b[4..8].copy_from_slice(&a.ip().octets());
                (b, 16)
            }
            SocketAddr::V6(a) => {
                b[0] = 23;
                b[4..8].copy_from_slice(&a.flowinfo().to_be_bytes());
                b[8..24].copy_from_slice(&a.ip().octets());
                b[24..28].copy_from_slice(&a.scope_id().to_le_bytes());
                (b, 28)
            }
        }
    }

    /// what `to_loopback` makes of `a`
    fn moved(a: SocketAddr) -> Option<SocketAddr> {
        let (b, n) = raw(a);
        let l = unsafe { to_loopback(b.as_ptr(), n)? };
        let port = u16::from_be_bytes([l[2], l[3]]);
        let word = |i: usize| [l[i], l[i + 1], l[i + 2], l[i + 3]];
        Some(match l[0] {
            2 => (Ipv4Addr::from(word(4)), port).into(),
            _ => SocketAddrV6::new(<[u8; 16]>::try_from(&l[8..24]).unwrap().into(), port, u32::from_be_bytes(word(4)), u32::from_le_bytes(word(24))).into(),
        })
    }

    /// Only the address changes: the port and IPv6's flow information stay, a scope goes (::1
    /// takes none), and an IPv4 address on a dual-stack socket gets the mapped 127.0.0.1.
    /// Loopback addresses and other families are left as they are.
    #[test]
    fn a_bind_keeps_its_port_on_loopback() {
        let v4 = |a: &str| a.parse::<SocketAddr>().unwrap();
        let v6 = |ip: &str, flow, scope| SocketAddr::from(SocketAddrV6::new(ip.parse().unwrap(), 3074, flow, scope));
        assert_eq!(moved(v4("0.0.0.0:3074")), Some(v4("127.0.0.1:3074")));
        assert_eq!(moved(v4("192.168.1.20:3074")), Some(v4("127.0.0.1:3074")));
        assert_eq!(moved(v6("::", 7, 0)), Some(v6("::1", 7, 0)));
        assert_eq!(moved(v6("fe80::1", 7, 12)), Some(v6("::1", 7, 0)));
        assert_eq!(moved(v6("::ffff:10.1.2.3", 0, 0)), Some(v6("::ffff:127.0.0.1", 0, 0)));
        for a in [v4("127.0.0.5:3074"), v6("::1", 0, 0), v6("::ffff:127.0.0.1", 0, 0)] {
            assert_eq!(moved(a), None, "{a}");
        }
        let unix = [1u8, 0, b'/', b't', b'm', b'p', 0, 0];
        assert!(unsafe { to_loopback(unix.as_ptr(), 8) }.is_none());
    }

    /// Under XCL_NETWORK=0 a bind to a network address ends up on loopback with its port: the
    /// title's (the module's `bind`, imported by name or by ordinal) and the system's own bind
    /// (`install_system_hooks`, as std calls it). A TCP listener there takes a connection.
    #[test]
    fn a_bind_to_the_network_ends_up_on_loopback() {
        let e: HashMap<_, _> = exports().into_iter().collect();
        let o: HashMap<_, _> = ordinals().into_iter().collect();
        assert_eq!((e["bind"], o[&2]), (bind as usize, "bind"));
        assert_eq!((e["listen"], o[&13]), (listen as usize, "listen"));
        install_system_hooks();
        assert!(O_BIND.load(Ordering::Acquire) != 0 && O_LISTEN.load(Ordering::Acquire) != 0, "bind and listen hooked");
        // the title's bind, to a port of its own
        let port = UdpSocket::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port();
        let u: UdpSocket = unbound(2, 2);
        let (a, n) = raw((Ipv4Addr::UNSPECIFIED, port).into());
        assert_eq!(unsafe { bind(raw_socket(&u), a.as_ptr(), n) }, 0);
        assert_eq!(u.local_addr().unwrap(), SocketAddr::from((Ipv4Addr::LOCALHOST, port)));
        // the system's
        assert_eq!(UdpSocket::bind("[::]:0").unwrap().local_addr().unwrap().ip(), Ipv6Addr::LOCALHOST);
        let l = TcpListener::bind("0.0.0.0:0").unwrap();
        let at = l.local_addr().unwrap();
        assert_eq!(at.ip(), Ipv4Addr::LOCALHOST);
        let c = TcpStream::connect(at).unwrap();
        assert_eq!(l.accept().unwrap().1, c.local_addr().unwrap());
    }

    /// Loopback binds are left as they are, any 127.x address and ::1: the title's and the
    /// system's.
    #[test]
    fn loopback_binds_are_untouched() {
        install_system_hooks();
        for a in ["127.0.0.5:0", "[::1]:0"] {
            let a: SocketAddr = a.parse().unwrap();
            assert_eq!(UdpSocket::bind(a).unwrap().local_addr().unwrap().ip(), a.ip());
            let u: UdpSocket = unbound(if a.is_ipv4() { 2 } else { 23 }, 2);
            let (b, n) = raw(a);
            assert_eq!(unsafe { bind(raw_socket(&u), b.as_ptr(), n) }, 0);
            assert_eq!(u.local_addr().unwrap().ip(), a.ip());
        }
    }

    /// The other ways to receive from the network: a listen on a socket bound to it before the
    /// hooks (by the original bind) fails, and an unbound socket's sendto to loopback binds it to
    /// loopback, not to 0.0.0.0 as Winsock's implicit bind does.
    #[test]
    fn no_other_way_to_receive_from_the_network() {
        install_system_hooks();
        let t: TcpListener = unbound(2, 1);
        let (a, n) = raw((Ipv4Addr::UNSPECIFIED, 0).into());
        unsafe {
            assert_eq!(orig_bind(raw_socket(&t), a.as_ptr(), n), 0);
            assert_eq!(listen(raw_socket(&t), 1), -1);
            assert_eq!(WSAGetLastError(), WSAENETUNREACH);
        }
        let peer = UdpSocket::bind("127.0.0.1:0").unwrap();
        peer.set_read_timeout(Some(std::time::Duration::from_secs(10))).unwrap();
        let u: UdpSocket = unbound(2, 2);
        let (to, n) = raw(peer.local_addr().unwrap());
        assert_eq!(unsafe { sendto(raw_socket(&u), b"x".as_ptr(), 1, 0, to.as_ptr(), n) }, 1);
        let from = u.local_addr().unwrap();
        assert_eq!(from.ip(), Ipv4Addr::LOCALHOST);
        assert_eq!(peer.recv_from(&mut [0u8; 1]).unwrap(), (1, from));
    }

    /// With the network on, nothing of it is in place: no module takes the title's ws2_32
    /// imports (its bind and listen are the system's own), and `hle::build` hooks the system
    /// only offline.
    #[test]
    fn nothing_changes_with_the_network_on() {
        let saved = std::env::var_os("XCL_NETWORK");
        std::env::remove_var("XCL_NETWORK");
        let (on, r) = (!enabled(), crate::hle::build(&Default::default()));
        if let Some(v) = saved {
            std::env::set_var("XCL_NETWORK", v);
        }
        assert!(on);
        assert!(r.hle_for("ws2_32.dll").is_none());
        assert!(r.find_any("bind").is_none() && r.find_any("listen").is_none());
    }
}
