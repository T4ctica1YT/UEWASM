//! Bytes a 32-bit system function pops on return (stdcall), read from the `ret N` in Windows' own SysWOW64 copy.
//! Results are cached in uewasm/recomp/stdcall32.json, so builds on machines without SysWOW64 still get them.
use crate::pe::{self, Export, Pe};
use iced_x86::{Decoder, DecoderOptions, FlowControl, Mnemonic, OpKind, Register};
use std::collections::{BTreeMap, HashMap, HashSet};
use std::path::{Path, PathBuf};

// where API-set forwarders and runtime-only names usually land
const FALLBACK: &[&str] = &[
    "kernelbase.dll", "kernel32.dll", "ntdll.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "sechost.dll", "combase.dll", "ole32.dll",
    "oleaut32.dll", "shell32.dll", "shlwapi.dll", "ws2_32.dll", "winmm.dll", "wininet.dll", "version.dll", "dbghelp.dll", "comdlg32.dll",
    "comctl32.dll", "gdiplus.dll", "imm32.dll", "setupapi.dll", "dsound.dll", "dinput8.dll", "xinput1_4.dll", "d3d9.dll", "dwmapi.dll",
    "winhttp.dll", "iphlpapi.dll", "crypt32.dll", "bcrypt.dll", "uxtheme.dll", "shcore.dll", "powrprof.dll", "psapi.dll", "dxgi.dll",
    "d3d11.dll", "hid.dll", "cfgmgr32.dll", "msacm32.dll", "avrt.dll", "mmdevapi.dll", "propsys.dll", "userenv.dll", "secur32.dll",
    "wtsapi32.dll", "ucrtbase.dll",
];
// where API sets (api-ms-win-*, ext-ms-win-*) are implemented: tried first for those
const API_SET_HOSTS: &[&str] =
    &["kernelbase.dll", "windows.storage.dll", "gdi32full.dll", "winmmbase.dll", "win32u.dll", "rpcrt4.dll", "shcore.dll", "sechost.dll", "combase.dll", "ucrtbase.dll"];
const CDECL_DLLS: &[&str] = &["ucrtbase", "msvcr", "msvcp", "vcruntime", "api-ms-win-crt-", "msvcrt"];
// cdecl families: LDAP (LDAPAPI/BERAPI), the Steam API
const CDECL_NAMES: &[&str] = &["ldap_", "ber_", "SteamAPI_", "SteamInternal_", "SteamGameServer"];
const CDECL_FUNCS: &[&str] = &[
    "LdapGetLastError", "acosf", "asinf", "atan2f", "atanf", "ceilf", "cosf", "expf", "floorf", "fmodf", "hypotf", "log10f", "logf", "modff",
    "sinf", "sqrtf", "tanf", "SteamApps", "SteamFriends", "SteamMatchmaking", "SteamMatchmakingServers", "SteamNetworking",
    "SteamRemoteStorage", "SteamUser", "SteamUserStats", "SteamUtils", "SteamScreenshots", "SteamHTTP", "SteamUGC",
];
/// documented argument bytes for functions whose Windows copy can't be read for it: bodies reached through pointers
/// filled in at run time, noreturn calls, DLLs that aren't in SysWOW64 (CUDA, XAudio2 2.8+, Media Foundation on N
/// editions), and cdecl libraries that don't look like one by name
const KNOWN: &[(&str, u32)] = &[
    ("DefWindowProcA", 16), ("DefWindowProcW", 16), ("DebugBreak", 0), ("ExitThread", 4), ("FreeLibraryAndExitThread", 8),
    ("GetWindowLongPtrA", 8), ("GetWindowLongPtrW", 8), ("SetWindowLongPtrA", 12), ("SetWindowLongPtrW", 12), ("GetClassLongPtrA", 8),
    ("GetClassLongPtrW", 8), ("SetClassLongPtrA", 12), ("SetClassLongPtrW", 12), ("AlphaBlend", 44), ("TransparentBlt", 44),
    ("RpcStringFreeA", 4), ("RpcStringFreeW", 4), ("UuidCreate", 4), ("UuidCreateNil", 4), ("UuidCreateSequential", 4), ("UuidToStringA", 8),
    ("UuidToStringW", 8), ("GetNamedSecurityInfoW", 32), ("SetNamedSecurityInfoW", 28), ("SetEntriesInAclW", 16), ("NCryptFreeObject", 4),
    ("NCryptGetProperty", 24), ("WinVerifyTrust", 12), ("Tbsi_GetDeviceInfo", 8), ("wglCreateContext", 4), ("wglDeleteContext", 4),
    ("wglGetCurrentContext", 0), ("wglGetCurrentDC", 0), ("wglGetProcAddress", 4), ("wglMakeCurrent", 8), ("XInputGetStateEx", 8),
    ("XAudio2Create", 12), ("XAudio2CreateWithVersionInfo", 16), ("CreateAudioReverb", 4), ("CreateAudioVolumeMeter", 4), ("CreateFX", 16),
    ("X3DAudioInitialize", 0), ("X3DAudioCalculate", 0), ("MFStartup", 8), ("MFShutdown", 0), ("MFCreateAttributes", 8),
    ("MFCreateMediaType", 4), ("MFCreateMemoryBuffer", 8), ("MFCreateSample", 4), ("MFCreateSourceReaderFromByteStream", 12),
    ("MFCreateSourceReaderFromURL", 12), ("MFCreateSourceResolver", 4), ("MFCreateTopology", 4), ("MFCreateTopologyNode", 8),
    ("MFGetService", 16), ("MFCreateMediaSession", 8), ("MFCreateSampleGrabberSinkActivate", 12), ("DXVA2CreateDirect3DDeviceManager9", 8),
    ("D3DDisassemble", 20), ("UiaClientsAreListening", 0), ("UiaHostProviderFromHwnd", 8), ("UiaRaiseAutomationEvent", 8),
    ("UiaRaiseAutomationPropertyChangedEvent", 40), ("UiaReturnRawElementProvider", 16), ("__cudaRegisterFatBinary", 4),
    ("__cudaUnregisterFatBinary", 4), ("__cudaRegisterFunction", 40), ("__cudaRegisterVar", 32), ("__cudaRegisterTexture", 28),
    ("__cudaRegisterShared", 8), ("cudaCreateChannelDesc", 24), ("cudaEventCreate", 4), ("cudaGetDeviceProperties", 8),
    ("cudaGetErrorString", 4), ("cudaGetLastError", 0), ("cudaMalloc", 8), ("cudaMallocHost", 8), ("cudaStreamCreate", 4),
    ("RtlAddFunctionTable", 0), ("RtlDeleteFunctionTable", 0), ("RtlInstallFunctionTableCallback", 0), ("RtlLookupFunctionEntry", 0),
    ("RtlVirtualUnwind", 0), ("RtlUnwindEx", 0), ("__C_specific_handler", 0), ("D3D12SerializeRootSignature", 0),
    ("D3D12SerializeVersionedRootSignature", 0),
];

fn documented(name: &str) -> Option<u32> {
    if let Some(&(_, v)) = KNOWN.iter().find(|k| k.0 == name) {
        return Some(v);
    }
    (CDECL_FUNCS.contains(&name) || CDECL_NAMES.iter().any(|p| name.starts_with(p))).then_some(0)
}

struct Loaded {
    pe: Pe,
    img: Vec<u8>,
    iat: HashMap<u64, (String, String)>,
}

pub struct Stdcall {
    wow: PathBuf,
    cache_path: PathBuf,
    cache: BTreeMap<String, u32>,
    dirty: bool,
    loaded: HashMap<String, Option<Loaded>>,
    /// the game's own folders: DLLs a game ships but that aren't recompiled (CUDA, Steam) are read from there
    pub search: Vec<PathBuf>,
}

impl Stdcall {
    pub fn new(cache_path: &Path) -> Stdcall {
        let cache = std::fs::read_to_string(cache_path).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default();
        Stdcall { wow: crate::link::windir().join("SysWOW64"), cache_path: cache_path.into(), cache, dirty: false, loaded: HashMap::new(), search: vec![] }
    }

    fn load(&mut self, dll: &str) -> bool {
        let dll = dll.to_lowercase();
        if !self.loaded.contains_key(&dll) {
            let mut got = None;
            for d in std::iter::once(self.wow.clone()).chain(self.search.iter().cloned()) {
                let p = d.join(&dll);
                if p.is_file() {
                    if let Ok(pe) = pe::load(&p) {
                        let img = pe.image(pe.image_base);
                        let iat = pe
                            .delay_imports
                            .iter()
                            .chain(pe.imports.iter())
                            .map(|i| (pe.image_base + i.iat_rva as u64, (i.dll.clone(), i.name.clone())))
                            .collect();
                        got = Some(Loaded { pe, img, iat });
                        break;
                    }
                }
            }
            self.loaded.insert(dll.clone(), got);
        }
        self.loaded[&dll].is_some()
    }

    fn scan(&mut self, dll: &str, rva: u32, depth: u32) -> Option<u32> {
        let (base, start) = {
            let l = self.loaded[dll].as_ref()?;
            (l.pe.image_base, l.pe.image_base + rva as u64)
        };
        let (mut work, mut seen, mut steps) = (vec![start], HashSet::new(), 0);
        while let Some(mut a) = work.pop() {
            while !seen.contains(&a) && steps < 4000 {
                seen.insert(a);
                steps += 1;
                let l = self.loaded[dll].as_ref()?;
                let Some(o) = a.checked_sub(base).map(|o| o as usize).filter(|&o| o < l.img.len()) else { break };
                let ins = Decoder::with_ip(32, &l.img[o..(o + 16).min(l.img.len())], a, DecoderOptions::NONE).decode();
                if ins.is_invalid() {
                    break;
                }
                match ins.flow_control() {
                    FlowControl::Return => return Some(if ins.op_count() > 0 { ins.immediate(0) as u32 } else { 0 }),
                    FlowControl::ConditionalBranch => work.push(ins.near_branch_target()),
                    FlowControl::UnconditionalBranch => {
                        a = ins.near_branch_target();
                        continue;
                    }
                    FlowControl::IndirectBranch => {
                        if ins.op0_kind() == OpKind::Memory && ins.memory_base() == Register::None && ins.memory_index() == Register::None {
                            let slot = ins.memory_displacement64() & 0xFFFF_FFFF;
                            let imp = l.iat.get(&slot).cloned();
                            if let Some((idll, iname)) = &imp {
                                if depth < 6 {
                                    if let Some(r) = self.lookup(idll, iname, depth + 1) {
                                        return Some(r);
                                    }
                                }
                            }
                            // newer Windows builds route exports through function pointers in the DLL's own data
                            let l = self.loaded[dll].as_ref()?;
                            let so = slot.wrapping_sub(base) as usize;
                            if imp.is_none() && so + 4 <= l.img.len() {
                                let t = u32::from_le_bytes(l.img[so..so + 4].try_into().unwrap()) as u64;
                                if t >= base && ((t - base) as usize) < l.img.len() {
                                    a = t;
                                    continue;
                                }
                            }
                        }
                        break;
                    }
                    FlowControl::Interrupt | FlowControl::Exception => break,
                    _ => {}
                }
                let _ = Mnemonic::Ret;
                a = ins.next_ip();
            }
        }
        None
    }

    /// bytes popped, or None when unknown
    fn lookup(&mut self, dll: &str, name: &str, depth: u32) -> Option<u32> {
        let mut dll = dll.to_lowercase();
        if !dll.ends_with(".dll") {
            dll += ".dll";
        }
        if CDECL_DLLS.iter().any(|p| dll.starts_with(p)) {
            return Some(0);
        }
        let mut cands: Vec<String> = if self.load(&dll) { vec![dll.clone()] } else { vec![] };
        if dll.starts_with("api-ms-") || dll.starts_with("ext-ms-") {
            cands.extend(API_SET_HOSTS.iter().map(|s| s.to_string()));
        }
        for f in FALLBACK {
            if !cands.iter().any(|c| c == f) {
                cands.push(f.to_string());
            }
        }
        for d in cands {
            if !self.load(&d) {
                continue;
            }
            let l = self.loaded[&d].as_ref().unwrap();
            let t = l.pe.exports.get(name).cloned().or_else(|| name.strip_prefix('#').and_then(|o| o.parse().ok()).and_then(|o| l.pe.exports_ord.get(&o).cloned()));
            let r = match t {
                None => continue,
                Some(Export::Forward(f)) => {
                    let (fdll, fname) = f.split_once('.').unwrap_or((&f, ""));
                    if depth < 6 { self.lookup(fdll, fname, depth + 1) } else { None }
                }
                Some(Export::Rva(_)) if CDECL_DLLS.iter().any(|p| d.starts_with(p)) => return Some(0),
                Some(Export::Rva(rva)) => self.scan(&d, rva, depth),
            };
            // a copy that only forwards on (or can't be read) doesn't settle it: another DLL may hold the body
            if r.is_some() {
                return r;
            }
        }
        None
    }

    pub fn pops(&mut self, dll: &str, name: &str) -> Option<u32> {
        if let Some(&v) = self.cache.get(name) {
            return Some(v);
        }
        let v = if self.wow.is_dir() { self.lookup(if dll.is_empty() { "kernel32.dll" } else { dll }, name, 0) } else { None };
        match v {
            None => documented(name),
            Some(v) => {
                self.cache.insert(name.to_string(), v);
                self.dirty = true;
                Some(v)
            }
        }
    }

    pub fn save(&self) {
        if self.dirty && self.wow.is_dir() {
            // the same layout Python's json.dumps(indent=0, sort_keys=True) wrote, so the file diffs cleanly
            let body: Vec<String> = self.cache.iter().map(|(k, v)| format!("{}: {v}", serde_json::to_string(k).unwrap())).collect();
            let _ = std::fs::write(&self.cache_path, format!("{{\n{}\n}}", body.join(",\n")));
        }
    }
}
