//! Serves a web build (the headers a threaded wasm page needs, byte ranges, an optional page log) and, for online games,
//! the built-in relay: the part of the WebWorks API the runtime uses (guest sessions, lobbies with data, members and
//! chat, P2P packets over a realtime WebSocket), and a WISP server at /wisp/ for the game's real connections (wisp.rs).
//!   uewserve <web dir> [--port N] [--host ADDR] [--log FILE] [--relay | --no-relay] [--wisp | --no-wisp]
//!            [--wisp-allow HOSTS] [--wisp-private] [--no-ranges]
//!   uewserve --relay-only [--wisp] [--port N] [--host ADDR]
//! The relay and the WISP server run when the build was made with them (its folder holds `uew-relay` / `uew-wisp`).
mod wisp;

use serde_json::{json, Map, Value};
use std::collections::{BTreeMap, HashMap};
use std::fs::{File, OpenOptions};
use std::io::{BufRead, BufReader, Read, Seek, SeekFrom, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{SystemTime, UNIX_EPOCH};

const LOG_SCRIPT: &str = include_str!("logscript.html");
const WS_GUID: &str = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const STEAM_BASE: u64 = 76561197960265728;
const LOBBY_BASE: u64 = 0x0186_0000_0000_0000;
const VISIBILITY: [&str; 4] = ["private", "friends", "public", "invisible"];

struct Server {
    root: Option<PathBuf>,
    log: Option<Mutex<PathBuf>>,
    relay: Option<Relay>,
    wisp: Option<wisp::Policy>,
    ranges: bool,
}

struct Request {
    method: String,
    path: String,
    query: String,
    headers: HashMap<String, String>,
    body: Vec<u8>,
}
impl Request {
    fn header(&self, k: &str) -> &str {
        self.headers.get(k).map(|s| s.as_str()).unwrap_or("")
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut root = None;
    let (mut port, mut host, mut log, mut relay, mut relay_only, mut ranges) = (8080u16, "127.0.0.1".to_string(), None, None, false, true);
    let (mut wisp_on, mut policy) = (None, wisp::Policy::default());
    let mut i = 0;
    while i < args.len() {
        let next = |i: &mut usize| -> String {
            *i += 1;
            args.get(*i).cloned().unwrap_or_else(|| die(&format!("{} needs a value", args[*i - 1])))
        };
        match args[i].as_str() {
            "--port" => port = next(&mut i).parse().unwrap_or_else(|_| die("bad --port")),
            "--host" => host = next(&mut i),
            "--log" => log = Some(PathBuf::from(next(&mut i))),
            "--relay" => relay = Some(true),
            "--no-relay" => relay = Some(false),
            "--relay-only" => relay_only = true,
            "--no-ranges" => ranges = false,
            "--wisp" => wisp_on = Some(true),
            "--no-wisp" => wisp_on = Some(false),
            "--wisp-private" => policy.private = true,
            "--wisp-allow" => policy.allow = next(&mut i).split(',').map(|h| h.trim().to_ascii_lowercase()).filter(|h| !h.is_empty()).collect(),
            a if a.starts_with("--") => die(&format!("unknown option {a}")),
            a => root = Some(PathBuf::from(a)),
        }
        i += 1;
    }
    if relay_only {
        root = None;
        relay = Some(true);
    } else if root.is_none() {
        die("usage: uewserve <web dir> [--port N] [--host ADDR] [--log FILE] [--relay | --no-relay] | uewserve --relay-only");
    }
    let root = root.map(|r| r.canonicalize().unwrap_or_else(|_| die(&format!("no folder {}", r.display()))));
    let relay_on = relay.unwrap_or_else(|| root.as_ref().map_or(false, |r| r.join("uew-relay").is_file()));
    let wisp_on = wisp_on.unwrap_or_else(|| root.as_ref().map_or(false, |r| r.join("uew-wisp").is_file()));
    if let Some(l) = &log {
        if let Some(p) = l.parent() {
            let _ = std::fs::create_dir_all(p);
        }
        if let Ok(mut f) = OpenOptions::new().create(true).append(true).open(l) {
            let _ = writeln!(f, "=== uewasm serve {} on port {port}", root.as_ref().map_or("".into(), |r| r.display().to_string()));
        }
    }
    let listener = TcpListener::bind((host.as_str(), port)).unwrap_or_else(|e| die(&format!("can't listen on {host}:{port}: {e}")));
    let port = listener.local_addr().map(|a| a.port()).unwrap_or(port);
    let shown = if host == "127.0.0.1" || host == "0.0.0.0" { "localhost" } else { host.as_str() };
    match &root {
        Some(r) => println!("Serving {} at http://{shown}:{port}/  (Ctrl+C to stop)", r.display()),
        None => println!("Relay listening on http://{shown}:{port}/  (Ctrl+C to stop)"),
    }
    if root.is_some() && relay_on {
        println!("The built-in relay is on: players who open this address play together");
    }
    if wisp_on {
        let reach = if policy.allow.is_empty() { "any public host".to_string() } else { policy.allow.join(", ") };
        println!("The WISP server is on at /wisp/: the game connects to real servers ({reach}{})",
                 if policy.private { ", private addresses too" } else { "" });
    }
    if let Some(l) = &log {
        println!("The page's log is written to {}", l.display());
    }
    println!("@@port {port}");
    let _ = std::io::stdout().flush();
    let srv = Arc::new(Server { root, log: log.map(Mutex::new), relay: relay_on.then(Relay::default), wisp: wisp_on.then_some(policy), ranges });
    for conn in listener.incoming().flatten() {
        let srv = srv.clone();
        thread::spawn(move || {
            let _ = conn.set_nodelay(true);
            handle_conn(&srv, conn);
        });
    }
}

fn die(msg: &str) -> ! {
    eprintln!("uewserve: {msg}");
    std::process::exit(2)
}

fn read_request(r: &mut BufReader<TcpStream>) -> Option<Request> {
    let mut line = String::new();
    if r.read_line(&mut line).ok()? == 0 {
        return None;
    }
    let mut parts = line.split_whitespace();
    let method = parts.next()?.to_string();
    let target = parts.next()?.to_string();
    let mut headers = HashMap::new();
    loop {
        let mut h = String::new();
        if r.read_line(&mut h).ok()? == 0 {
            return None;
        }
        let h = h.trim_end();
        if h.is_empty() {
            break;
        }
        if let Some((k, v)) = h.split_once(':') {
            headers.insert(k.trim().to_ascii_lowercase(), v.trim().to_string());
        }
    }
    let n: usize = headers.get("content-length").and_then(|v| v.parse().ok()).unwrap_or(0);
    let mut body = vec![0u8; n];
    r.read_exact(&mut body).ok()?;
    let (path, query) = match target.split_once('?') {
        Some((p, q)) => (p.to_string(), q.to_string()),
        None => (target, String::new()),
    };
    Some(Request { method, path: percent_decode(&path), query, headers, body })
}

fn handle_conn(srv: &Server, stream: TcpStream) {
    let Ok(read_half) = stream.try_clone() else { return };
    let mut r = BufReader::new(read_half);
    let mut w = stream;
    while let Some(req) = read_request(&mut r) {
        if let Some(policy) = &srv.wisp {
            if req.path.starts_with("/wisp") && req.header("upgrade").eq_ignore_ascii_case("websocket") {
                if ws_accept(&req, &mut w) {
                    wisp::session(policy, r, w);
                }
                return;
            }
        }
        if let Some(relay) = &srv.relay {
            if req.path == "/realtime" {
                relay.realtime(&req, r, w);
                return;
            }
            if req.path.starts_with("/api/") {
                let (status, out) = if req.method == "OPTIONS" { (204, Value::Null) } else { relay.api(&req) };
                let body = if out.is_null() { Vec::new() } else { out.to_string().into_bytes() };
                if respond(&mut w, status, &[("Content-Type", "application/json")], &body, true).is_err() {
                    return;
                }
                continue;
            }
        }
        let ok = match (req.method.as_str(), &srv.root) {
            ("POST", _) if req.path == "/__uewlog" && srv.log.is_some() => {
                if let Some(l) = &srv.log {
                    let p = l.lock().unwrap();
                    if let Ok(mut f) = OpenOptions::new().create(true).append(true).open(&*p) {
                        let _ = f.write_all(&req.body);
                    }
                }
                respond(&mut w, 204, &[], &[], false)
            }
            ("GET" | "HEAD", Some(root)) => serve_file(srv, root, &req, &mut w),
            _ => respond(&mut w, 404, &[("Content-Type", "text/plain")], b"not found", false),
        };
        if ok.is_err() || req.header("connection").eq_ignore_ascii_case("close") {
            return;
        }
    }
}

fn status_text(s: u16) -> &'static str {
    match s {
        200 => "OK",
        204 => "No Content",
        206 => "Partial Content",
        400 => "Bad Request",
        401 => "Unauthorized",
        403 => "Forbidden",
        404 => "Not Found",
        409 => "Conflict",
        416 => "Range Not Satisfiable",
        _ => "Error",
    }
}

fn head(w: &mut TcpStream, status: u16, extra: &[(&str, &str)], len: u64, cors: bool) -> std::io::Result<()> {
    let mut h = format!("HTTP/1.1 {status} {}\r\nContent-Length: {len}\r\n", status_text(status));
    if cors {
        h += "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Content-Type, X-Webworks-Session\r\n\
              Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n";
    } else {
        // threaded wasm needs a cross-origin isolated page; js-profiling lets the page sample itself (wasm frames too)
        h += "Cross-Origin-Opener-Policy: same-origin\r\nCross-Origin-Embedder-Policy: require-corp\r\n\
              Cross-Origin-Resource-Policy: same-origin\r\nDocument-Policy: js-profiling\r\n\
              Cache-Control: no-cache\r\nAccept-Ranges: bytes\r\n";
    }
    for (k, v) in extra {
        h += &format!("{k}: {v}\r\n");
    }
    h += "\r\n";
    w.write_all(h.as_bytes())
}

fn respond(w: &mut TcpStream, status: u16, extra: &[(&str, &str)], body: &[u8], cors: bool) -> std::io::Result<()> {
    head(w, status, extra, body.len() as u64, cors)?;
    w.write_all(body)
}

fn mime(p: &Path) -> &'static str {
    match p.extension().and_then(|e| e.to_str()).unwrap_or("").to_ascii_lowercase().as_str() {
        "wasm" => "application/wasm",
        "js" | "mjs" => "text/javascript",
        "html" | "htm" => "text/html",
        "css" => "text/css",
        "json" => "application/json",
        "txt" => "text/plain",
        "png" => "image/png",
        "jpg" | "jpeg" => "image/jpeg",
        "ico" => "image/x-icon",
        "svg" => "image/svg+xml",
        "mp4" => "video/mp4",
        "webm" => "video/webm",
        "ogg" => "audio/ogg",
        _ => "application/octet-stream",
    }
}

fn percent_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' && i + 2 < b.len() {
            if let Ok(v) = u8::from_str_radix(&s[i + 1..i + 3], 16) {
                out.push(v);
                i += 3;
                continue;
            }
        }
        out.push(b[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn serve_file(srv: &Server, root: &Path, req: &Request, w: &mut TcpStream) -> std::io::Result<()> {
    let rel: PathBuf = req.path.split('/').filter(|c| !c.is_empty() && *c != "." && *c != "..").collect();
    let mut path = root.join(rel);
    if path.is_dir() {
        path = path.join("index.html");
    }
    let is_page = path == root.join("index.html");
    let send_body = req.method != "HEAD";
    if is_page && srv.log.is_some() {
        if let Ok(html) = std::fs::read_to_string(&path) {
            let page = match html.find("<head>") {
                Some(i) => format!("{}\n{LOG_SCRIPT}{}", &html[..i + 6], &html[i + 6..]),
                None => format!("{LOG_SCRIPT}{html}"),
            };
            head(w, 200, &[("Content-Type", "text/html")], page.len() as u64, false)?;
            return if send_body { w.write_all(page.as_bytes()) } else { Ok(()) };
        }
    }
    let Ok(mut f) = File::open(&path) else {
        return respond(w, 404, &[("Content-Type", "text/plain")], b"not found", false);
    };
    let size = f.metadata().map(|m| m.len()).unwrap_or(0);
    let ty = mime(&path);
    let range = if !srv.ranges { None } else { req.header("range").strip_prefix("bytes=").and_then(|r| r.split_once('-')).and_then(|(a, b)| {
        let start: u64 = a.parse().ok()?;
        let end = if b.is_empty() { size.saturating_sub(1) } else { b.parse::<u64>().ok()?.min(size.saturating_sub(1)) };
        Some((start, end))
    }) };
    let (start, len) = match range {
        Some((s, e)) if s > e || s >= size => return respond(w, 416, &[], &[], false),
        Some((s, e)) => {
            let cr = format!("bytes {s}-{e}/{size}");
            head(w, 206, &[("Content-Type", ty), ("Content-Range", &cr)], e - s + 1, false)?;
            (s, e - s + 1)
        }
        None => {
            head(w, 200, &[("Content-Type", ty)], size, false)?;
            (0, size)
        }
    };
    if !send_body {
        return Ok(());
    }
    f.seek(SeekFrom::Start(start))?;
    let mut left = len;
    let mut buf = vec![0u8; 1 << 20];
    while left > 0 {
        let n = f.read(&mut buf[..left.min(1 << 20) as usize])?;
        if n == 0 {
            break;
        }
        w.write_all(&buf[..n])?;
        left -= n as u64;
    }
    Ok(())
}

// ---- the relay ----

type Sock = Arc<Mutex<TcpStream>>;

struct Player {
    name: String,
    app: String,
    sockets: Vec<Sock>,
}

struct Lobby {
    id: u64,
    owner: u64,
    app: String,
    max: i64,
    visibility: String,
    joinable: bool,
    members: Vec<u64>,
    data: BTreeMap<String, String>,
    member_data: BTreeMap<u64, BTreeMap<String, String>>,
    chat_id: u64,
}

#[derive(Default)]
struct State {
    players: HashMap<u64, Player>,
    tokens: HashMap<String, u64>,
    lobbies: BTreeMap<u64, Lobby>,
    next_lobby: u64,
}

#[derive(Default)]
struct Relay {
    st: Mutex<State>,
}

fn s(v: &Value) -> String {
    match v {
        Value::String(x) => x.clone(),
        Value::Null => String::new(),
        x => x.to_string(),
    }
}
fn num(v: &Value) -> Option<i64> {
    match v {
        Value::Number(n) => n.as_i64(),
        Value::String(x) => x.trim().parse().ok(),
        _ => None,
    }
}
fn id(v: &Value) -> u64 {
    match v {
        Value::Number(n) => n.as_u64().unwrap_or(0),
        Value::String(x) => x.trim().parse().unwrap_or(0),
        _ => 0,
    }
}

fn token() -> String {
    use std::hash::{BuildHasher, Hasher};
    (0..3)
        .map(|i| {
            let mut h = std::collections::hash_map::RandomState::new().build_hasher();
            h.write_u128(SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0) + i);
            format!("{:016x}", h.finish())
        })
        .collect()
}

impl Lobby {
    fn summary(&self, st: &State) -> Value {
        let members: Vec<Value> = self
            .members
            .iter()
            .map(|m| json!({ "steamId": m.to_string(), "name": st.players.get(m).map_or("Player", |p| p.name.as_str()) }))
            .collect();
        let md: Map<String, Value> = self.member_data.iter().map(|(k, v)| (k.to_string(), json!(v))).collect();
        json!({ "id": self.id.to_string(), "ownerId": self.owner.to_string(), "maxMembers": self.max,
                "visibility": self.visibility, "joinable": self.joinable, "memberCount": self.members.len(),
                "data": self.data, "members": members, "memberData": md })
    }
}

impl State {
    fn send(&self, to: u64, msg: &Value) {
        if let Some(p) = self.players.get(&to) {
            let frame = ws_frame(msg.to_string().as_bytes(), 1);
            for s in &p.sockets {
                let _ = s.lock().unwrap().write_all(&frame);
            }
        }
    }
    fn to_lobby(&self, lid: u64, msg: &Value, skip: u64) {
        if let Some(l) = self.lobbies.get(&lid) {
            for &m in &l.members {
                if m != skip {
                    self.send(m, msg);
                }
            }
        }
    }
    fn chat(&mut self, lid: u64, me: u64, data: &str) {
        let Some(l) = self.lobbies.get_mut(&lid) else { return };
        l.chat_id += 1;
        let msg = json!({ "type": "LobbyChat", "lobbyId": lid.to_string(), "from": me.to_string(), "chatId": l.chat_id,
                          "data": data, "entryType": 1 });
        self.to_lobby(lid, &msg, 0);
    }
    fn leave(&mut self, lid: u64, me: u64, why: &str) {
        let Some(l) = self.lobbies.get_mut(&lid) else { return };
        let Some(i) = l.members.iter().position(|&m| m == me) else { return };
        l.members.remove(i);
        l.member_data.remove(&me);
        if l.members.is_empty() {
            self.lobbies.remove(&lid);
            return;
        }
        if l.owner == me {
            l.owner = l.members[0];
        }
        let msg = json!({ "type": why, "lobbyId": lid.to_string(), "steamId": me.to_string(), "ownerId": l.owner.to_string() });
        self.to_lobby(lid, &msg, 0);
    }
}

impl Relay {
    fn api(&self, req: &Request) -> (u16, Value) {
        let body: Value = serde_json::from_slice(&req.body).unwrap_or(Value::Null);
        let b = |k: &str| body.get(k).cloned().unwrap_or(Value::Null);
        let path = &req.path[5..];
        let mut st = self.st.lock().unwrap();
        if path == "User/GuestSession" {
            let key = s(&b("deviceKey"));
            let acct = u64::from_str_radix(key.get(..8).unwrap_or(""), 16)
                .unwrap_or_else(|_| u64::from_be_bytes(sha1(key.as_bytes())[..8].try_into().unwrap()))
                & 0x7FFF_FFFF;
            // a page signed in to a Steam account says who it is (the relay has no accounts of its own to check)
            let given = id(&b("steamId"));
            let sid = if given > STEAM_BASE { given } else { STEAM_BASE + acct };
            let mut name = s(&b("name"));
            if name.is_empty() {
                name = format!("Player{:04}", acct % 10000);
            }
            name.truncate(32);
            let app = s(&b("appId"));
            let p = st.players.entry(sid).or_insert(Player { name: name.clone(), app: app.clone(), sockets: Vec::new() });
            p.name = name.clone();
            p.app = app;
            let t = token();
            st.tokens.insert(t.clone(), sid);
            return (200, json!({ "sessionToken": t, "steamId": sid.to_string(), "name": name }));
        }
        let Some(&me) = st.tokens.get(req.header("x-webworks-session")) else { return (401, json!({ "error": "no session" })) };
        let ok = json!({ "ok": true });
        if path.starts_with("Networking/") || path.starts_with("Friends/") {
            return (200, ok);
        }
        if path == "Matchmaking/CreateLobby" {
            st.next_lobby += 1;
            let vis = s(&b("visibility"));
            let lid = LOBBY_BASE + st.next_lobby;
            let app = st.players.get(&me).map(|p| p.app.clone()).unwrap_or_default();
            let l = Lobby { id: lid, owner: me, app, max: num(&b("maxMembers")).unwrap_or(4),
                            visibility: if VISIBILITY.contains(&vis.as_str()) { vis } else { "public".into() },
                            joinable: true, members: vec![me], data: BTreeMap::new(), member_data: BTreeMap::new(), chat_id: 0 };
            let out = l.summary(&st);
            st.lobbies.insert(lid, l);
            return (200, out);
        }
        if path == "Matchmaking/LobbyList" {
            return (200, json!({ "lobbies": list(&st, me, &body) }));
        }
        let q = req.query.split('&').find_map(|kv| kv.strip_prefix("lobbyId=")).unwrap_or("");
        let lid = if body.get("lobbyId").is_some() { id(&b("lobbyId")) } else { q.parse().unwrap_or(0) };
        let Some(l) = st.lobbies.get(&lid) else { return (404, json!({ "error": "no such lobby" })) };
        if path == "Matchmaking/Lobby" {
            return (200, l.summary(&st));
        }
        if path == "Matchmaking/JoinLobby" {
            if !l.members.contains(&me) {
                if !l.joinable || l.members.len() as i64 >= l.max {
                    return (409, json!({ "error": "lobby full or closed" }));
                }
                let name = st.players.get(&me).map(|p| p.name.clone()).unwrap_or_default();
                st.to_lobby(lid, &json!({ "type": "LobbyMemberJoined", "lobbyId": lid.to_string(), "steamId": me.to_string(), "name": name }), me);
                st.lobbies.get_mut(&lid).unwrap().members.push(me);
            }
            return (200, st.lobbies[&lid].summary(&st));
        }
        if !l.members.contains(&me) {
            return (403, json!({ "error": "not in that lobby" }));
        }
        let lids = lid.to_string();
        match path {
            "Matchmaking/LeaveLobby" => st.leave(lid, me, "LobbyMemberLeave"),
            "Matchmaking/LobbyData" => {
                let key = s(&b("key"));
                let l = st.lobbies.get_mut(&lid).unwrap();
                let value = if req.method == "DELETE" {
                    l.data.remove(&key);
                    Value::Null
                } else {
                    let v = s(&b("value"));
                    l.data.insert(key.clone(), v.clone());
                    Value::String(v)
                };
                st.to_lobby(lid, &json!({ "type": "LobbyDataChanged", "lobbyId": lids, "key": key, "value": value }), me);
            }
            "Matchmaking/MemberData" => {
                let (key, value) = (s(&b("key")), s(&b("value")));
                st.lobbies.get_mut(&lid).unwrap().member_data.entry(me).or_default().insert(key.clone(), value.clone());
                st.to_lobby(lid, &json!({ "type": "LobbyMemberDataChanged", "lobbyId": lids, "steamId": me.to_string(),
                                          "key": key, "value": value }), me);
            }
            "Matchmaking/LobbyVisibility" => {
                let m = s(&b("mode"));
                if VISIBILITY.contains(&m.as_str()) {
                    st.lobbies.get_mut(&lid).unwrap().visibility = m;
                }
            }
            "Matchmaking/LobbyJoinable" => st.lobbies.get_mut(&lid).unwrap().joinable = b("joinable").as_bool().unwrap_or(true),
            "Matchmaking/LobbyMaxMembers" => {
                if let Some(n) = num(&b("maxMembers")) {
                    st.lobbies.get_mut(&lid).unwrap().max = n;
                }
            }
            "Matchmaking/LobbyOwner" => {
                let new = id(&b("ownerId"));
                let l = st.lobbies.get_mut(&lid).unwrap();
                if l.owner == me && l.members.contains(&new) {
                    l.owner = new;
                    st.to_lobby(lid, &json!({ "type": "LobbyOwnerChanged", "lobbyId": lids, "ownerId": new.to_string() }), me);
                }
            }
            "Matchmaking/ChatSend" => st.chat(lid, me, &s(&b("data"))),
            _ => return (404, json!({ "error": "unknown call" })),
        }
        (200, ok)
    }

    fn realtime(&self, req: &Request, mut r: BufReader<TcpStream>, mut w: TcpStream) {
        let key = req.header("sec-websocket-key").to_string();
        let tok = req.query.split('&').find_map(|kv| kv.strip_prefix("session=")).map(percent_decode).unwrap_or_default();
        let me = self.st.lock().unwrap().tokens.get(&tok).copied();
        let Some(me) = me.filter(|_| !key.is_empty()) else {
            let _ = respond(&mut w, if key.is_empty() { 400 } else { 401 }, &[], &[], true);
            return;
        };
        if !ws_accept(req, &mut w) {
            return;
        }
        let sock: Sock = Arc::new(Mutex::new(w));
        if let Some(p) = self.st.lock().unwrap().players.get_mut(&me) {
            p.sockets.push(sock.clone());
        }
        while let Some((op, payload)) = ws_read(&mut r) {
            match op {
                8 => break,
                9 => {
                    let _ = sock.lock().unwrap().write_all(&ws_frame(&payload, 10));
                }
                1 | 2 => {
                    if let Ok(m) = serde_json::from_slice::<Value>(&payload) {
                        self.message(me, &m);
                    }
                }
                _ => {}
            }
        }
        let mut st = self.st.lock().unwrap();
        let gone = match st.players.get_mut(&me) {
            Some(p) => {
                p.sockets.retain(|s| !Arc::ptr_eq(s, &sock));
                p.sockets.is_empty()
            }
            None => true,
        };
        if gone {
            let lids: Vec<u64> = st.lobbies.keys().copied().collect();
            for lid in lids {
                st.leave(lid, me, "LobbyMemberDisconnected");
            }
            let others: Vec<u64> = st.players.keys().copied().filter(|&p| p != me).collect();
            for p in others {
                st.send(p, &json!({ "type": "PeerDisconnected", "steamId": me.to_string() }));
            }
        }
    }

    fn message(&self, me: u64, m: &Value) {
        let mut st = self.st.lock().unwrap();
        match m.get("type").and_then(|t| t.as_str()) {
            Some("P2PPacket") => {
                let to = id(m.get("to").unwrap_or(&Value::Null));
                let name = st.players.get(&me).map(|p| p.name.clone()).unwrap_or_default();
                let msg = json!({ "type": "P2PPacket", "from": me.to_string(), "steamId": me.to_string(), "name": name,
                                  "channel": m.get("channel").and_then(num).unwrap_or(0), "data": m.get("data").map(s).unwrap_or_default() });
                st.send(to, &msg);
            }
            Some("LobbyChat") => {
                let lid = id(m.get("lobbyId").unwrap_or(&Value::Null));
                if st.lobbies.get(&lid).map_or(false, |l| l.members.contains(&me)) {
                    st.chat(lid, me, &m.get("data").map(s).unwrap_or_default());
                }
            }
            _ => {}
        }
    }
}

fn list(st: &State, me: u64, body: &Value) -> Vec<Value> {
    let app = st.players.get(&me).map(|p| p.app.as_str()).unwrap_or("");
    let empty = Map::new();
    let filters = body.get("filters").and_then(|f| f.as_object()).unwrap_or(&empty);
    let none = Vec::new();
    let numeric = body.get("numeric").and_then(|f| f.as_array()).unwrap_or(&none);
    let near = body.get("near").and_then(|f| f.as_array()).unwrap_or(&none);
    let slots = body.get("slots").and_then(num).unwrap_or(0);
    let val = |l: &Lobby, k: &Value| l.data.get(&s(k)).and_then(|v| v.trim().parse::<i64>().ok());
    let mut out: Vec<(&Lobby, i64)> = st
        .lobbies
        .values()
        .filter(|l| l.app == app && l.visibility == "public" && l.joinable && l.max - l.members.len() as i64 >= slots)
        .filter(|l| filters.iter().all(|(k, v)| l.data.get(k).map(|x| x.as_str()) == Some(s(v).as_str())))
        .filter(|l| {
            numeric.iter().all(|f| {
                let (Some(a), Some(b)) = (val(l, &f["key"]), num(&f["value"])) else { return false };
                match f.get("op").and_then(|o| o.as_str()).unwrap_or("eq") {
                    "ne" => a != b,
                    "lt" => a < b,
                    "lte" => a <= b,
                    "gt" => a > b,
                    "gte" => a >= b,
                    _ => a == b,
                }
            })
        })
        .map(|l| (l, near.iter().map(|f| (val(l, &f["key"]).unwrap_or(0) - num(&f["value"]).unwrap_or(0)).abs()).sum()))
        .collect();
    out.sort_by_key(|&(_, d)| d);
    let max = body.get("max").and_then(num).filter(|&m| m > 0).unwrap_or(50) as usize;
    out.into_iter().take(max).map(|(l, _)| l.summary(st)).collect()
}

// ---- WebSocket frames, SHA-1, base64 ----

/// answers a WebSocket upgrade; false when the request lacks a key or the answer can't be sent
fn ws_accept(req: &Request, w: &mut TcpStream) -> bool {
    let key = req.header("sec-websocket-key");
    if key.is_empty() {
        let _ = respond(w, 400, &[], &[], true);
        return false;
    }
    let accept = base64(&sha1(format!("{key}{WS_GUID}").as_bytes()));
    let hs = format!("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n");
    w.write_all(hs.as_bytes()).is_ok()
}

fn ws_read(r: &mut impl Read) -> Option<(u8, Vec<u8>)> {
    let (mut data, mut first_op) = (Vec::new(), 0u8);
    loop {
        let mut h = [0u8; 2];
        r.read_exact(&mut h).ok()?;
        let (fin, op, masked) = (h[0] & 0x80 != 0, h[0] & 0x0F, h[1] & 0x80 != 0);
        let mut n = (h[1] & 0x7F) as u64;
        if n == 126 {
            let mut e = [0u8; 2];
            r.read_exact(&mut e).ok()?;
            n = u16::from_be_bytes(e) as u64;
        } else if n == 127 {
            let mut e = [0u8; 8];
            r.read_exact(&mut e).ok()?;
            n = u64::from_be_bytes(e);
        }
        if n > 64 << 20 {
            return None;
        }
        let mut mask = [0u8; 4];
        if masked {
            r.read_exact(&mut mask).ok()?;
        }
        let mut body = vec![0u8; n as usize];
        r.read_exact(&mut body).ok()?;
        if masked {
            for (i, c) in body.iter_mut().enumerate() {
                *c ^= mask[i & 3];
            }
        }
        if op >= 8 {
            return Some((op, body));
        }
        if op != 0 {
            first_op = op;
        }
        data.extend_from_slice(&body);
        if fin {
            return Some((first_op, data));
        }
    }
}

fn ws_frame(payload: &[u8], op: u8) -> Vec<u8> {
    let n = payload.len();
    let mut f = vec![0x80 | op];
    if n < 126 {
        f.push(n as u8);
    } else if n < 65536 {
        f.push(126);
        f.extend_from_slice(&(n as u16).to_be_bytes());
    } else {
        f.push(127);
        f.extend_from_slice(&(n as u64).to_be_bytes());
    }
    f.extend_from_slice(payload);
    f
}

fn sha1(msg: &[u8]) -> [u8; 20] {
    let mut h: [u32; 5] = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0];
    let mut m = msg.to_vec();
    m.push(0x80);
    while m.len() % 64 != 56 {
        m.push(0);
    }
    m.extend_from_slice(&((msg.len() as u64) * 8).to_be_bytes());
    for block in m.chunks(64) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes(block[4 * i..4 * i + 4].try_into().unwrap());
        }
        for i in 16..80 {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
        }
        let [mut a, mut b, mut c, mut d, mut e] = h;
        for (i, &wi) in w.iter().enumerate() {
            let (f, k) = match i {
                0..=19 => ((b & c) | (!b & d), 0x5A827999),
                20..=39 => (b ^ c ^ d, 0x6ED9EBA1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1BBCDC),
                _ => (b ^ c ^ d, 0xCA62C1D6),
            };
            let t = a.rotate_left(5).wrapping_add(f).wrapping_add(e).wrapping_add(k).wrapping_add(wi);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = t;
        }
        for (x, y) in h.iter_mut().zip([a, b, c, d, e]) {
            *x = x.wrapping_add(y);
        }
    }
    let mut out = [0u8; 20];
    for (i, v) in h.iter().enumerate() {
        out[4 * i..4 * i + 4].copy_from_slice(&v.to_be_bytes());
    }
    out
}

fn base64(b: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut s = String::new();
    for c in b.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for i in 0..4 {
            s.push(if i <= c.len() { T[(n >> (18 - 6 * i)) as usize & 63] as char } else { '=' });
        }
    }
    s
}
