//! Links a game exe with the DLLs it ships (and the Microsoft C runtime) into one guest address space: each module gets
//! a base, its imports point at their targets, and what nothing provides goes to a runtime thunk.
use crate::pe::{self, Export, Pe};
use regex::Regex;
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

pub const THUNK_BASE: u64 = 0x10000;
const FIRST_BASE: u64 = 0x0040_0000;
const RUNTIME_DLLS: [&str; 10] = ["ucrtbase.dll", "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
                                  "msvcp140_atomic_wait.dll", "concrt140.dll", "vccorlib140.dll", "msvcrt.dll"];
/// stateless functions that are faster as host code; anything that touches CRT state stays recompiled
const PURE: &[&str] = &[
    "memcpy", "memmove", "memset", "memcmp", "memchr", "strlen", "strcmp", "strncmp", "strchr", "strrchr", "strstr", "wcslen", "sin", "cos",
    "tan", "asin", "acos", "atan", "atan2", "sqrt", "exp", "log", "log10", "pow", "fmod", "floor", "ceil", "sinf", "cosf", "tanf", "asinf",
    "acosf", "atanf", "atan2f", "sqrtf", "expf", "logf", "log10f", "powf", "fmodf", "floorf", "ceilf", "hypot", "hypotf", "sinh", "cosh",
    "tanh", "round", "roundf", "trunc", "truncf", "log2", "log2f", "exp2", "exp2f", "cbrt", "ldexp", "frexp", "modf", "modff",
];

fn ordinal_name(dll: &str, n: u32) -> Option<&'static str> {
    let t: &[(u32, &str)] = match dll {
        "xaudio2_9redist.dll" => &[(1, "XAudio2Create"), (2, "CreateAudioReverb"), (3, "CreateAudioVolumeMeter"), (4, "CreateFX"),
                                   (5, "X3DAudioCalculate"), (6, "X3DAudioInitialize")],
        "ws2_32.dll" => &[(1, "accept"), (2, "bind"), (3, "closesocket"), (4, "connect"), (5, "getpeername"), (6, "getsockname"),
                          (7, "getsockopt"), (8, "htonl"), (9, "htons"), (10, "ioctlsocket"), (11, "inet_addr"), (12, "inet_ntoa"),
                          (13, "listen"), (14, "ntohl"), (15, "ntohs"), (16, "recv"), (17, "recvfrom"), (18, "select"), (19, "send"),
                          (20, "sendto"), (21, "setsockopt"), (22, "shutdown"), (23, "socket"), (51, "gethostbyaddr"),
                          (52, "gethostbyname"), (53, "getprotobyname"), (55, "getservbyname"), (57, "gethostname"),
                          (111, "WSAGetLastError"), (112, "WSASetLastError"), (115, "WSAStartup"), (116, "WSACleanup"),
                          (151, "__WSAFDIsSet")],
        "dsound.dll" => &[(1, "DirectSoundCreate"), (2, "DirectSoundEnumerateA"), (3, "DirectSoundEnumerateW"),
                          (6, "DirectSoundCaptureCreate"), (11, "DirectSoundCreate8"), (12, "DirectSoundCaptureCreate8")],
        _ => return None,
    };
    t.iter().find(|e| e.0 == n).map(|e| e.1)
}

/// shipped DLLs that are served by the runtime instead (XAudio2 2.9, dbghelp) or never used in a browser build
/// left out of a build: what skip_dlls names, except the EOS SDK when the build runs the real one
pub fn skipped(name: &str, real_eos: bool) -> bool {
    skip_dlls().is_match(name) && !(real_eos && name.to_ascii_lowercase().starts_with("eossdk-"))
}

pub fn skip_dlls() -> &'static Regex {
    static R: OnceLock<Regex> = OnceLock::new();
    R.get_or_init(|| {
        Regex::new(
            r"(?i)(libcef|chrome_elf|libegl|libglesv2|d3dcompiler|gfsdk_aftermath|winpixeventruntime|dxgi|d3d1\d|steam_api|nvtt|embree|tbb|libsndfile|ue4prereq|crashreport|unrealcefsubprocess|steamvr|openvr|oculus|libovr|xaudio2_\d|openimagedenoise|gfnruntimesdk|dbghelp|nvngx|^sl\.|libxess|libxell|amd_fidelityfx|ffx_|directml|crashpad|msquic|nvlowlatency|reflex|^api-ms-win-|^ext-ms-win-|^xinput[0-9]|^x3daudio|beclient|easyanticheat|gfesdk|^dsetup|^d3dx9_|^steamapiupdater|^cudart|^wxmsw|^eossdk-)",
        )
        .unwrap()
    })
}

pub fn windir() -> PathBuf {
    PathBuf::from(std::env::var("SystemRoot").unwrap_or_else(|_| r"C:\Windows".into()))
}

/// side-by-side runtime folders next to an exe (Microsoft.VC90.CRT and the like), which older games ship their CRT in
fn sxs_dirs(d: &Path) -> Vec<PathBuf> {
    let re = Regex::new(r"(?i)^Microsoft\.VC\d+\.\w+$").unwrap();
    std::fs::read_dir(d)
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| e.path().is_dir() && re.is_match(&e.file_name().to_string_lossy()))
        .map(|e| e.path())
        .collect()
}

/// Visual C++ 2005/2008 runtime DLLs a game doesn't ship, from Windows' side-by-side store, newest version first
fn sxs_crt(name: &str, bits: u32) -> Option<PathBuf> {
    let m = Regex::new(r"(?i)^msvc[rpm](\d)0\.dll$").unwrap().captures(name)?;
    let winsxs = windir().join("WinSxS");
    let prefix = format!("{}_microsoft.vc{}0.crt_", if bits == 32 { "x86" } else { "amd64" }, &m[1]);
    let ver = Regex::new(r"_(\d+)\.(\d+)\.(\d+)\.(\d+)_").unwrap();
    let mut dirs: Vec<(Vec<u64>, PathBuf)> = std::fs::read_dir(&winsxs)
        .ok()?
        .flatten()
        .filter(|e| e.file_name().to_string_lossy().to_lowercase().starts_with(&prefix))
        .map(|e| {
            let n = e.file_name().to_string_lossy().into_owned();
            let v = ver.captures(&n).map(|c| (1..5).map(|i| c[i].parse().unwrap_or(0)).collect()).unwrap_or_else(|| vec![0]);
            (v, e.path())
        })
        .collect();
    dirs.sort_by(|a, b| b.0.cmp(&a.0));
    dirs.into_iter().map(|(_, d)| d.join(name)).find(|p| p.is_file())
}

fn bits_of(p: &Path) -> u32 {
    // only the headers are needed: read the first 4 KB
    use std::io::Read;
    let mut b = vec![0u8; 4096];
    let n = std::fs::File::open(p).and_then(|mut f| f.read(&mut b)).unwrap_or(0);
    b.truncate(n);
    let pe = b.get(0x3C..0x40).map_or(0, |x| u32::from_le_bytes(x.try_into().unwrap()) as usize);
    match b.get(pe + 24..pe + 26) {
        Some([0x0B, 0x02]) if b.get(..2) == Some(b"MZ") => 64,
        Some([0x0B, 0x01]) if b.get(..2) == Some(b"MZ") => 32,
        _ => 0,
    }
}

/// DLLs shipped with the game that it may load at run time (PhysX, codecs, middleware): (install root, DLLs)
pub fn game_dlls(exe: &Path, real_eos: bool) -> (PathBuf, Vec<PathBuf>) {
    let bits = match bits_of(exe) {
        0 => 64,
        b => b,
    };
    let mut root = exe.parent().unwrap_or(Path::new(".")).to_path_buf();
    let mut found_engine = false;
    for _ in 0..4 {
        if root.join("Engine").is_dir() {
            found_engine = true;
            break;
        }
        match root.parent() {
            Some(p) => root = p.to_path_buf(),
            None => break,
        }
    }
    if !found_engine {
        root = exe.parent().unwrap_or(Path::new(".")).to_path_buf();
    }
    let editor = Regex::new(r"(?i)^(UE4|Unreal)Editor").unwrap();
    let vc = Regex::new(r"(?i)^Microsoft\.VC\d+\.").unwrap();
    let mut out = Vec::new();
    let mut stack = vec![root.clone()];
    while let Some(d) = stack.pop() {
        for e in std::fs::read_dir(&d).into_iter().flatten().flatten() {
            let p = e.path();
            if p.is_dir() {
                stack.push(p);
                continue;
            }
            let name = e.file_name().to_string_lossy().into_owned();
            if !name.to_lowercase().ends_with(".dll") || skipped(&name, real_eos) || editor.is_match(&name) {
                continue;
            }
            let rel = p.strip_prefix(&root).unwrap_or(&p);
            // plugin binaries are runtime middleware in shipped games; mods and private CRT copies aren't loaded
            if rel.components().any(|c| {
                let c = c.as_os_str().to_string_lossy();
                c == "Mods" || vc.is_match(&c)
            }) {
                continue;
            }
            // only DLLs the exe could load: games often ship leftovers of the other bitness
            if bits_of(&p) == bits {
                out.push(p);
            }
        }
    }
    out.sort();
    (root, out)
}

fn apiset(name: &str) -> String {
    let n = name.to_lowercase();
    if n.starts_with("api-ms-win-crt-") { "ucrtbase.dll".into() } else { n }
}

pub struct Module {
    pub name: String,
    pub path: PathBuf,
    pub pe: Pe,
    pub base: u64,
    pub deps: Vec<String>,
}
impl Module {
    pub fn is_dll(&self) -> bool {
        self.path.extension().map_or(false, |e| e.eq_ignore_ascii_case("dll"))
    }
}

pub struct Linker {
    pub search: Vec<PathBuf>,
    hle: HashSet<String>,
    recompile_system: bool,
    real_eos: bool,
    pub modules: Vec<Module>,
    index: HashMap<String, usize>,
    pub thunks: Vec<(String, String)>,
    thunk_index: HashMap<String, usize>,
    pub missing: Vec<String>,
    pub bits: u32,
    sysdir: PathBuf,
    pub end: u64,
    pub init_order: Vec<usize>,
    sys_ords: HashMap<String, HashMap<u32, String>>,
}

impl Linker {
    pub fn new(exe: &Path, search: &[PathBuf], hle: HashSet<String>, recompile_system: bool, real_eos: bool, extra: &[PathBuf]) -> Result<Linker, String> {
        let dir = exe.parent().unwrap_or(Path::new(".")).to_path_buf();
        let mut s = vec![dir.clone()];
        s.extend(sxs_dirs(&dir));
        s.extend(search.iter().cloned());
        let bits = bits_of(exe);
        if bits == 0 {
            pe::load(exe)?;
        }
        let sysdir = windir().join(if bits == 64 { "System32" } else { "SysWOW64" });
        let mut l = Linker { search: s, hle, recompile_system, real_eos, modules: vec![], index: HashMap::new(), thunks: vec![], thunk_index: HashMap::new(),
                             missing: vec![], bits, sysdir, end: 0, init_order: vec![], sys_ords: HashMap::new() };
        l.add(exe)?;
        for e in extra {
            l.add(e)?;
        }
        l.assign_bases();
        l.init_order = l.dependency_order();
        Ok(l)
    }

    fn find(&self, dll: &str) -> Option<PathBuf> {
        let n = apiset(dll);
        // left to the runtime: API-set forwarders a game ships, and DLLs the browser can't use
        if skipped(&n, self.real_eos) {
            return None;
        }
        let runtime = RUNTIME_DLLS.contains(&n.as_str());
        if !runtime && self.sysdir.join(&n).is_file() {
            return None;
        }
        for d in &self.search {
            let p = d.join(&n);
            if p.is_file() && bits_of(&p) == self.bits {
                return Some(p);
            }
        }
        if self.recompile_system && runtime && self.sysdir.join(&n).is_file() {
            return Some(self.sysdir.join(&n));
        }
        sxs_crt(&n, self.bits)
    }

    fn add(&mut self, path: &Path) -> Result<String, String> {
        let key = path.file_name().unwrap_or_default().to_string_lossy().to_lowercase();
        if self.index.contains_key(&key) {
            return Ok(key);
        }
        let pe = pe::load(path)?;
        let mut dlls: Vec<String> = Vec::new();
        for i in &pe.imports {
            let d = apiset(&i.dll);
            if !dlls.contains(&d) {
                dlls.push(d);
            }
        }
        self.index.insert(key.clone(), self.modules.len());
        self.modules.push(Module { name: key.clone(), path: path.to_path_buf(), pe, base: 0, deps: vec![] });
        let me = self.modules.len() - 1;
        for d in dlls {
            if let Some(p) = self.find(&d) {
                let dep = self.add(&p)?;
                self.modules[me].deps.push(dep);
            }
        }
        Ok(key)
    }

    fn assign_bases(&mut self) {
        let mut base = FIRST_BASE;
        let exe = &self.modules[0].pe;
        // an image without relocations has to sit at its own base
        if exe.relocs.is_empty() && exe.image_base >= FIRST_BASE && exe.image_base < 0x4000_0000 {
            base = exe.image_base;
        }
        for m in &mut self.modules {
            m.base = base;
            base = (base + m.pe.image_size as u64 + 0xFFFF) & !0xFFFF;
        }
        self.end = base;
    }

    fn dependency_order(&self) -> Vec<usize> {
        fn visit(l: &Linker, i: usize, seen: &mut HashSet<usize>, out: &mut Vec<usize>) {
            if !seen.insert(i) {
                return;
            }
            for d in &l.modules[i].deps {
                visit(l, l.index[d], seen, out);
            }
            out.push(i);
        }
        let (mut seen, mut out) = (HashSet::new(), Vec::new());
        for i in 0..self.modules.len() {
            visit(self, i, &mut seen, &mut out);
        }
        out
    }

    fn thunk(&mut self, name: &str, dll: &str) -> u64 {
        let i = match self.thunk_index.get(name) {
            Some(&i) => i,
            None => {
                let i = self.thunks.len();
                self.thunk_index.insert(name.to_string(), i);
                self.thunks.push((name.to_string(), dll.to_string()));
                if !self.hle.contains(name) {
                    self.missing.push(format!("{dll}!{name}"));
                }
                i
            }
        };
        THUNK_BASE + 8 * i as u64
    }

    /// the name Windows' own copy of a DLL exports under an ordinal, for imports by number
    fn system_ord_name(&mut self, dll: &str, ord: u32) -> Option<String> {
        if !self.sys_ords.contains_key(dll) {
            let p = self.sysdir.join(dll);
            let names = if p.is_file() { pe::load(&p).map(|pe| pe.ord_names).unwrap_or_default() } else { HashMap::new() };
            self.sys_ords.insert(dll.to_string(), names);
        }
        self.sys_ords[dll].get(&ord).cloned()
    }

    fn resolve(&mut self, dll: &str, name: &str, depth: u32) -> u64 {
        let n = apiset(dll);
        let mut name = name.to_string();
        if let Some(num) = name.strip_prefix('#').and_then(|x| x.parse::<u32>().ok()) {
            if !self.index.contains_key(&n) {
                if let Some(s) = ordinal_name(&n, num) {
                    name = s.to_string();
                } else if let Some(s) = self.system_ord_name(&n, num) {
                    name = s;
                }
            }
        }
        if PURE.contains(&name.as_str()) && self.hle.contains(&name) {
            return self.thunk(&name, &n);
        }
        let Some(&mi) = self.index.get(&n) else { return self.thunk(&name, &n) };
        if depth > 8 {
            return self.thunk(&name, &n);
        }
        let m = &self.modules[mi];
        let t = match name.strip_prefix('#').and_then(|x| x.parse::<u32>().ok()) {
            Some(o) => m.pe.exports_ord.get(&o).cloned(),
            None => m.pe.exports.get(&name).cloned(),
        };
        match t {
            None => self.thunk(&name, &n),
            Some(Export::Forward(f)) => {
                let (fdll, fname) = f.split_once('.').unwrap_or((&f, ""));
                let (fdll, fname) = (format!("{fdll}.dll"), fname.to_string());
                self.resolve(&fdll, &fname, depth + 1)
            }
            Some(Export::Rva(r)) => m.base + r as u64,
        }
    }

    /// the relocated module images, with every import slot already pointing at its target
    pub fn images(&mut self) -> Vec<Vec<u8>> {
        let mut hle: Vec<String> = self.hle.iter().cloned().collect();
        hle.sort();
        for name in hle {
            self.thunk(&name, "");
        }
        let mut out = Vec::new();
        for k in 0..self.modules.len() {
            let mut img = self.modules[k].pe.image(self.modules[k].base);
            let imports = self.modules[k].pe.imports.clone();
            let wide = self.modules[k].pe.bits == 64;
            for imp in imports {
                let v = self.resolve(&imp.dll, &imp.name, 0);
                let r = imp.iat_rva as usize;
                if wide {
                    if r + 8 <= img.len() {
                        img[r..r + 8].copy_from_slice(&v.to_le_bytes());
                    }
                } else if r + 4 <= img.len() {
                    img[r..r + 4].copy_from_slice(&(v as u32).to_le_bytes());
                }
            }
            out.push(img);
        }
        out
    }
}
