//! A WISP v1 server (github.com/MercuryWorkshop/wisp-protocol): TCP and UDP streams for a page, multiplexed over one
//! WebSocket. The page's game opens real connections through it (its own TLS stays end to end; this only relays bytes).
//! Packets: type u8, stream id u32 LE, payload. CONNECT: stream type u8 (1 TCP, 2 UDP), port u16 LE, host name. DATA:
//! bytes. CONTINUE: buffer u32 (how many more DATA packets a TCP stream may send). CLOSE: reason u8.
//! Policy: private, loopback and link-local destinations are refused (so a public server isn't a way into its own
//! network) unless allowed; an allowlist of host names narrows it further.
use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{IpAddr, Shutdown, SocketAddr, TcpStream, ToSocketAddrs, UdpSocket};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{channel, Sender};
use std::sync::{Arc, Mutex};
use std::time::Duration;

const CONNECT: u8 = 1;
const DATA: u8 = 2;
const CONTINUE: u8 = 3;
const CLOSE: u8 = 4;
const BUFFER: u32 = 128;
// close reasons
const VOLUNTARY: u8 = 0x02;
const NETWORK_ERROR: u8 = 0x03;
const INVALID: u8 = 0x41;
const UNREACHABLE: u8 = 0x42;
const REFUSED: u8 = 0x44;
const TIMEOUT: u8 = 0x47;
const BLOCKED: u8 = 0x48;

#[derive(Default, Clone)]
pub struct Policy {
    /// host patterns ("example.com", "*.example.com"); empty means any public host
    pub allow: Vec<String>,
    /// also reach private and loopback addresses
    pub private: bool,
}

impl Policy {
    fn host_ok(&self, host: &str) -> bool {
        let h = host.trim_end_matches('.').to_ascii_lowercase();
        self.allow.is_empty()
            || self.allow.iter().any(|p| match p.strip_prefix("*.") {
                Some(s) => h == s || h.ends_with(&format!(".{s}")),
                None => h == *p,
            })
    }
    fn addr_ok(&self, a: &IpAddr) -> bool {
        if self.private {
            return true;
        }
        match a {
            IpAddr::V4(v) => !(v.is_private() || v.is_loopback() || v.is_link_local() || v.is_unspecified() || v.is_multicast() || v.is_broadcast()
                || v.octets()[0] == 100 && (v.octets()[1] & 0xC0) == 64),
            IpAddr::V6(v) => {
                let s = v.segments();
                !(v.is_loopback() || v.is_unspecified() || v.is_multicast() || (s[0] & 0xFE00) == 0xFC00 || (s[0] & 0xFFC0) == 0xFE80
                    || v.to_ipv4_mapped().map_or(false, |m| !self.addr_ok(&IpAddr::V4(m))))
            }
        }
    }
}

type Ws = Arc<Mutex<TcpStream>>;

fn packet(ws: &Ws, t: u8, id: u32, payload: &[u8]) -> bool {
    let mut p = Vec::with_capacity(5 + payload.len());
    p.push(t);
    p.extend_from_slice(&id.to_le_bytes());
    p.extend_from_slice(payload);
    ws.lock().unwrap().write_all(&super::ws_frame(&p, 2)).is_ok()
}
fn close(ws: &Ws, id: u32, why: u8) {
    packet(ws, CLOSE, id, &[why]);
}

enum Stream {
    Tcp { tx: Sender<Vec<u8>>, sent: u32, sock: TcpStream },
    Udp { sock: Arc<UdpSocket>, stop: Arc<AtomicBool> },
    Pending { queued: Vec<Vec<u8>> },
}

type Streams = Arc<Mutex<HashMap<u32, Stream>>>;

/// where a CONNECT goes, after the policy: (addresses, or the close reason)
fn resolve(policy: &Policy, host: &str, port: u16) -> Result<Vec<SocketAddr>, u8> {
    if !policy.host_ok(host) {
        return Err(BLOCKED);
    }
    let addrs: Vec<SocketAddr> = (host, port).to_socket_addrs().map_err(|_| UNREACHABLE)?.collect();
    if addrs.is_empty() {
        return Err(UNREACHABLE);
    }
    if addrs.iter().any(|a| !policy.addr_ok(&a.ip())) {
        return Err(BLOCKED);
    }
    Ok(addrs)
}

fn open_tcp(ws: Ws, streams: Streams, policy: Policy, id: u32, host: String, port: u16) {
    std::thread::spawn(move || {
        let conn = resolve(&policy, &host, port).and_then(|addrs| {
            let mut last = UNREACHABLE;
            for a in addrs {
                match TcpStream::connect_timeout(&a, Duration::from_secs(10)) {
                    Ok(s) => return Ok(s),
                    Err(e) => {
                        last = match e.kind() {
                            std::io::ErrorKind::ConnectionRefused => REFUSED,
                            std::io::ErrorKind::TimedOut => TIMEOUT,
                            _ => UNREACHABLE,
                        }
                    }
                }
            }
            Err(last)
        });
        let sock = match conn {
            Ok(s) => s,
            Err(why) => {
                streams.lock().unwrap().remove(&id);
                close(&ws, id, why);
                return;
            }
        };
        let _ = sock.set_nodelay(true);
        let (tx, rx) = channel::<Vec<u8>>();
        // what the page sent before the connection was up
        let Ok(keep) = sock.try_clone() else { return };
        let queued = match streams.lock().unwrap().insert(id, Stream::Tcp { tx: tx.clone(), sent: 0, sock: keep }) {
            Some(Stream::Pending { queued }) => queued,
            // closed by the page while connecting
            _ => {
                streams.lock().unwrap().remove(&id);
                return;
            }
        };
        for q in queued {
            let _ = tx.send(q);
        }
        drop(tx);
        let mut w = sock.try_clone().expect("socket");
        std::thread::spawn(move || {
            for d in rx {
                if w.write_all(&d).is_err() {
                    break;
                }
            }
            let _ = w.shutdown(Shutdown::Write);
        });
        let mut r = sock;
        let mut buf = vec![0u8; 64 * 1024];
        let why = loop {
            match r.read(&mut buf) {
                Ok(0) => break VOLUNTARY,
                Ok(n) => {
                    if !packet(&ws, DATA, id, &buf[..n]) {
                        break NETWORK_ERROR;
                    }
                }
                Err(_) => break NETWORK_ERROR,
            }
        };
        if streams.lock().unwrap().remove(&id).is_some() {
            close(&ws, id, why);
        }
    });
}

fn open_udp(ws: Ws, streams: Streams, policy: Policy, id: u32, host: String, port: u16) {
    std::thread::spawn(move || {
        let sock = resolve(&policy, &host, port).and_then(|addrs| {
            let a = addrs[0];
            let s = UdpSocket::bind(if a.is_ipv4() { "0.0.0.0:0" } else { "[::]:0" }).map_err(|_| NETWORK_ERROR)?;
            s.connect(a).map_err(|_| UNREACHABLE)?;
            Ok(s)
        });
        let sock = match sock {
            Ok(s) => Arc::new(s),
            Err(why) => {
                streams.lock().unwrap().remove(&id);
                close(&ws, id, why);
                return;
            }
        };
        let _ = sock.set_read_timeout(Some(Duration::from_millis(500)));
        let stop = Arc::new(AtomicBool::new(false));
        let queued = match streams.lock().unwrap().insert(id, Stream::Udp { sock: sock.clone(), stop: stop.clone() }) {
            Some(Stream::Pending { queued }) => queued,
            _ => {
                streams.lock().unwrap().remove(&id);
                return;
            }
        };
        for q in queued {
            let _ = sock.send(&q);
        }
        let mut buf = vec![0u8; 65536];
        while !stop.load(Ordering::Relaxed) {
            match sock.recv(&mut buf) {
                Ok(n) => {
                    if !packet(&ws, DATA, id, &buf[..n]) {
                        break;
                    }
                }
                Err(e) if matches!(e.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut) => {}
                Err(_) => {}
            }
        }
    });
}

/// one page's WISP session, after the WebSocket handshake
pub fn session(policy: &Policy, mut r: impl Read, w: TcpStream) {
    let ws: Ws = Arc::new(Mutex::new(w));
    let streams: Streams = Arc::new(Mutex::new(HashMap::new()));
    if !packet(&ws, CONTINUE, 0, &BUFFER.to_le_bytes()) {
        return;
    }
    while let Some((op, msg)) = super::ws_read(&mut r) {
        match op {
            8 => break,
            9 => {
                let _ = ws.lock().unwrap().write_all(&super::ws_frame(&msg, 10));
                continue;
            }
            1 | 2 => {}
            _ => continue,
        }
        if msg.len() < 5 {
            continue;
        }
        let (t, id, p) = (msg[0], u32::from_le_bytes(msg[1..5].try_into().unwrap()), &msg[5..]);
        match t {
            CONNECT => {
                if p.len() < 4 || streams.lock().unwrap().contains_key(&id) {
                    close(&ws, id, INVALID);
                    continue;
                }
                let port = u16::from_le_bytes([p[1], p[2]]);
                let host = String::from_utf8_lossy(&p[3..]).trim().to_string();
                streams.lock().unwrap().insert(id, Stream::Pending { queued: vec![] });
                match p[0] {
                    1 => open_tcp(ws.clone(), streams.clone(), policy.clone(), id, host, port),
                    2 => open_udp(ws.clone(), streams.clone(), policy.clone(), id, host, port),
                    _ => {
                        streams.lock().unwrap().remove(&id);
                        close(&ws, id, INVALID);
                    }
                }
            }
            DATA => {
                let mut m = streams.lock().unwrap();
                let mut grant = false;
                match m.get_mut(&id) {
                    Some(Stream::Tcp { tx, sent, .. }) => {
                        let _ = tx.send(p.to_vec());
                        *sent += 1;
                        // the page may send BUFFER packets per grant: renew it halfway
                        if *sent >= BUFFER / 2 {
                            *sent = 0;
                            grant = true;
                        }
                    }
                    Some(Stream::Udp { sock, .. }) => {
                        let _ = sock.send(p);
                    }
                    Some(Stream::Pending { queued }) => queued.push(p.to_vec()),
                    None => {}
                }
                drop(m);
                if grant {
                    packet(&ws, CONTINUE, id, &BUFFER.to_le_bytes());
                }
            }
            CLOSE => match streams.lock().unwrap().remove(&id) {
                Some(Stream::Udp { stop, .. }) => stop.store(true, Ordering::Relaxed),
                Some(Stream::Tcp { sock, .. }) => {
                    let _ = sock.shutdown(Shutdown::Both);
                }
                _ => {}
            },
            _ => {}
        }
    }
    for (_, s) in streams.lock().unwrap().drain() {
        match s {
            Stream::Udp { stop, .. } => stop.store(true, Ordering::Relaxed),
            Stream::Tcp { sock, .. } => {
                let _ = sock.shutdown(Shutdown::Both);
            }
            _ => {}
        }
    }
}
