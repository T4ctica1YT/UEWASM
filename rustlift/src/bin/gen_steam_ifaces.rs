//! Writes runtime/steam_ifaces.h: each Steam interface version's vtable, slot by slot: the method's name and the bytes
//! a 32-bit (thiscall) call to it leaves on the stack for the method to pop.
//!
//!     cargo run --release --manifest-path rustlift/Cargo.toml --bin gen_steam_ifaces -- <SDK header dir> [out.h]
//!
//! The header dir holds the Steamworks SDK headers with every old interface version (the ISteamFoo00N classes, as
//! the Goldberg emulator's sdk/steam keeps them); a header's current version is the one its *_INTERFACE_VERSION names.
//! MSVC lays out a class's overloaded virtuals together, at the first one's place, in reverse order of declaration
//! (GetStat(float*) before GetStat(int32*)); the table does the same. Platform-only sections (_PS3, other OSes) are
//! left out as Windows builds leave them out.
use regex::Regex;
use std::collections::{BTreeMap, HashMap, HashSet};
use std::path::{Path, PathBuf};

/// 8-byte types passed by value; everything else by value is 4 bytes on x86 (ints, enums, bools, floats, pointers)
const WIDE: &[&str] = &[
    "uint64", "int64", "uint64_t", "int64_t", "double", "CSteamID", "CGameID", "SteamAPICall_t", "PublishedFileId_t",
    "UGCHandle_t", "SteamLeaderboard_t", "SteamLeaderboardEntries_t", "UGCQueryHandle_t", "UGCUpdateHandle_t",
    "PublishedFileUpdateHandle_t", "UGCFileWriteStreamHandle_t", "SteamItemInstanceID_t", "SteamInventoryUpdateHandle_t",
    "ManifestId_t", "InputHandle_t", "ControllerHandle_t", "InputActionSetHandle_t", "InputDigitalActionHandle_t",
    "InputAnalogActionHandle_t", "ControllerActionSetHandle_t", "ControllerDigitalActionHandle_t",
    "ControllerAnalogActionHandle_t", "PartyBeaconID_t", "SteamNetworkingMicroseconds", "unsigned long long", "long long",
    "GID_t", "JobID_t", "TxnID_t", "ClientUnifiedMessageHandle", "PublishedFileId",
];
/// classes returned by value come back through a hidden pointer, one more 4-byte argument
const RETURNED_CLASSES: &[&str] = &["CSteamID", "CGameID", "SteamNetworkingIdentity", "SteamIPAddress_t"];

fn strip_platform(text: &str) -> String {
    let skip = Regex::new(r"(?i)^#\s*(if|ifdef)\s+(.*_PS3|.*__PS3__|.*_PS3_|.*OSX|.*POSIX|.*LINUX|.*__linux__)").unwrap();
    let (iff, endif, els) = (Regex::new(r"^#\s*if").unwrap(), Regex::new(r"^#\s*endif").unwrap(), Regex::new(r"^#\s*else").unwrap());
    let mut out = Vec::new();
    let mut depth = 0;
    for line in text.lines() {
        let s = line.trim();
        if depth > 0 {
            if iff.is_match(s) {
                depth += 1;
            } else if endif.is_match(s) {
                depth -= 1;
            } else if els.is_match(s) && depth == 1 {
                depth = 0;
            }
            continue;
        }
        if skip.is_match(s) {
            depth = 1;
            continue;
        }
        out.push(line);
    }
    out.join("\n")
}

/// the text after `open` up to its balanced `close` (exclusive), and the index just past it
fn balanced(s: &[u8], start: usize, open: u8, close: u8) -> (usize, usize) {
    let (mut i, mut depth) = (start, 1);
    while depth > 0 && i < s.len() {
        if s[i] == open {
            depth += 1;
        } else if s[i] == close {
            depth -= 1;
        }
        i += 1;
    }
    (start, i - 1)
}

/// (class name, body) for every ISteam* class with a body
fn classes(text: &str) -> Vec<(String, String)> {
    let re = Regex::new(r"\bclass\s+(ISteam\w+)\s*(?::[^{;]*)?\{").unwrap();
    let b = text.as_bytes();
    re.captures_iter(text)
        .map(|c| {
            let m = c.get(0).unwrap();
            let (s, e) = balanced(b, m.end(), b'{', b'}');
            (c[1].to_string(), text[s..e].to_string())
        })
        .collect()
}

fn split_args(s: &str) -> Vec<String> {
    let (mut out, mut depth, mut cur) = (Vec::new(), 0, String::new());
    for ch in s.chars() {
        match ch {
            '(' | '<' => depth += 1,
            ')' | '>' => depth -= 1,
            _ => {}
        }
        if ch == ',' && depth == 0 {
            out.push(std::mem::take(&mut cur));
        } else {
            cur.push(ch);
        }
    }
    if !cur.trim().is_empty() {
        out.push(cur);
    }
    out
}

struct Re {
    macro_call: Regex,
    macro_word: Regex,
    word: Regex,
    ws: Regex,
    comments: Regex,
    virt: Regex,
    quals: Regex,
}

fn arg_bytes(re: &Re, arg: &str) -> u32 {
    let a = re.macro_call.replace_all(arg, " ");
    let a = re.macro_word.replace_all(&a, " ");
    let a = a.split('=').next().unwrap_or("").trim().to_string();
    if a.is_empty() || a == "void" {
        return 0;
    }
    if a.contains('*') || a.contains('&') || a.contains('[') {
        return 4;
    }
    let words: Vec<&str> = re.word.find_iter(&a).map(|m| m.as_str())
        .filter(|w| !matches!(*w, "const" | "volatile" | "struct" | "enum" | "class")).collect();
    if words.is_empty() {
        return 4;
    }
    let typ = if words.len() > 1 { words[..words.len() - 1].join(" ") } else { words[0].to_string() };
    if WIDE.contains(&typ.as_str()) || WIDE.contains(&words[0]) { 8 } else { 4 }
}

/// [(slot name, x86 argument bytes)] in vtable order
fn methods(re: &Re, body: &str) -> Vec<(String, u32)> {
    let body = re.comments.replace_all(body, "").to_string();
    let b = body.as_bytes();
    let mut decls: Vec<(String, String, u32)> = Vec::new();
    for c in re.virt.captures_iter(&body) {
        let m = c.get(0).unwrap();
        let (s, e) = balanced(b, m.end(), b'(', b')');
        let name = c[2].to_string();
        if name.starts_with('~') {
            continue;
        }
        let args = &body[s..e];
        let mut n: u32 = split_args(args).iter().map(|a| arg_bytes(re, a)).sum();
        let rt = re.quals.replace_all(&c[1], "").trim().to_string();
        if RETURNED_CLASSES.contains(&rt.as_str()) {
            n += 4;
        }
        decls.push((name, re.ws.replace_all(args, " ").trim().to_string(), n));
    }
    let mut count: HashMap<&str, usize> = HashMap::new();
    for (n, _, _) in &decls {
        *count.entry(n.as_str()).or_insert(0) += 1;
    }
    let (mut out, mut done) = (Vec::new(), HashSet::new());
    for (n, _, _) in &decls {
        if !done.insert(n.clone()) {
            continue;
        }
        // an overloaded method keeps its parameters ("GetStat|const char *pchName, float *pData") to tell them apart
        let group: Vec<&(String, String, u32)> = decls.iter().filter(|d| &d.0 == n).collect();
        for d in group.iter().rev() {
            let slot = if count[n.as_str()] == 1 { n.clone() } else { format!("{}|{}", n, d.1) };
            out.push((slot, d.2));
        }
    }
    out
}

fn family(name: &str) -> String {
    let l = name.to_lowercase();
    let l = l.strip_prefix('i').unwrap_or(&l);
    l.trim_end_matches(|c: char| c.is_ascii_digit()).to_string()
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!("usage: gen_steam_ifaces <SDK header dir> [out.h]");
        std::process::exit(2);
    }
    let src = PathBuf::from(&args[1]);
    let out = args.get(2).map(PathBuf::from)
        .unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join("runtime").join("steam_ifaces.h"));
    let re = Re {
        macro_call: Regex::new(r"STEAM_\w+\s*\([^()]*(\([^()]*\))?[^()]*\)").unwrap(),
        macro_word: Regex::new(r"\bSTEAM_\w+\b").unwrap(),
        word: Regex::new(r"[A-Za-z_]\w*").unwrap(),
        ws: Regex::new(r"\s+").unwrap(),
        comments: Regex::new(r"(?s)//[^\n]*|/\*.*?\*/").unwrap(),
        virt: Regex::new(r"\bvirtual\b([^;{(]*?)\b(~?\w+)\s*\(").unwrap(),
        quals: Regex::new(r"\b(const|inline)\b").unwrap(),
    };
    let defs_re = Regex::new(r#"#define\s+\w*INTERFACE_VERSION\w*\s+"([^"]+)""#).unwrap();
    let ver_strip = Regex::new(r"_?interface_?(version|v)").unwrap();
    let num_re = Regex::new(r"(\d{3})$").unwrap();
    let tail_re = Regex::new(r"(\d+)$").unwrap();

    let mut files: Vec<PathBuf> = std::fs::read_dir(&src).expect("header dir").filter_map(|e| e.ok()).map(|e| e.path())
        .filter(|p| p.file_name().and_then(|n| n.to_str()).map_or(false, |n| n.starts_with("isteam") && n.ends_with(".h")))
        .collect();
    files.sort();
    let mut tables: BTreeMap<(String, u32), Vec<(String, u32)>> = BTreeMap::new();
    for h in files {
        let raw = std::fs::read(&h).expect("read header");
        let text = strip_platform(&String::from_utf8_lossy(&raw));
        let defs: Vec<String> = defs_re.captures_iter(&text).map(|c| c[1].to_string()).collect();
        for (cls, body) in classes(&text) {
            let ms = methods(&re, &body);
            if ms.is_empty() {
                continue;
            }
            let key = if let Some(n) = num_re.captures(&cls) {
                (family(&cls), n[1].parse().unwrap())
            } else {
                let fam = family(&cls);
                let Some(v) = defs.iter().find(|v| family(&ver_strip.replace(&v.to_lowercase(), "")) == fam) else { continue };
                (fam, tail_re.captures(v).map(|c| c[1].parse().unwrap()).unwrap_or(0))
            };
            tables.entry(key).or_insert(ms);
        }
    }

    let mut lines = vec![
        "/* generated by gen_steam_ifaces (rustlift/src/bin) from the Steamworks SDK headers: each interface version's".to_string(),
        "   vtable, one method name per slot (overloads repeat their name, in MSVC's order), and the bytes a 32-bit".to_string(),
        "   call to each leaves on the stack for the method to pop (thiscall) */".to_string(),
        "typedef struct { const char* family; int version; const char* const* slots; const unsigned char* pops; int count; } SteamIfaceTable;".to_string(),
        String::new(),
    ];
    let mut names = Vec::new();
    for ((fam, ver), ms) in &tables {
        let ident = format!("st_{}_{:03}", fam, ver);
        names.push((fam.clone(), *ver, ident.clone(), ms.len()));
        let slots: Vec<String> = ms.iter().map(|(m, _)| format!("\"{}\"", m)).collect();
        let pops: Vec<String> = ms.iter().map(|(_, b)| b.to_string()).collect();
        lines.push(format!("static const char* const {}[] = {{ {} }};", ident, slots.join(", ")));
        lines.push(format!("static const unsigned char {}_pops[] = {{ {} }};", ident, pops.join(", ")));
    }
    lines.push(String::new());
    lines.push("static const SteamIfaceTable steam_iface_tables[] = {".to_string());
    for (fam, ver, ident, n) in &names {
        lines.push(format!("    {{ \"{}\", {}, {}, {}_pops, {} }},", fam, ver, ident, ident, n));
    }
    lines.push("};".to_string());
    lines.push(String::new());
    std::fs::write(&out, lines.join("\n")).expect("write header");
    println!("{}: {} interface versions", out.display(), names.len());
}
