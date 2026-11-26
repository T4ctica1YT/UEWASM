//! Unreal Engine 2 install and renderer detection, plus INI patching for the web renderer. UE2 config files are
//! commonly ANSI/Windows-1252, so patching works on bytes and only rewrites the ASCII keys it owns.

use std::path::{Path, PathBuf};

/// The renderer settings the fixed-function path needs, in the order they are written into the INI.
const DRIVER_KEYS: [(&str, &str); 3] =
    [("UseHardwareTL", "True"), ("UseHardwareVS", "False"), ("MaxPixelShaderVersion", "0")];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Ue2Renderer {
    D3D8,
    D3D9,
}

impl Ue2Renderer {
    pub fn module(self) -> &'static str {
        match self {
            Ue2Renderer::D3D8 => "D3DDrv.D3DRenderDevice",
            Ue2Renderer::D3D9 => "D3D9Drv.D3D9RenderDevice",
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            Ue2Renderer::D3D8 => "Direct3D 8",
            Ue2Renderer::D3D9 => "Direct3D 9",
        }
    }
}

/// A Unreal Engine 2 install's renderer path and the INI files that select it. The game's own executable may be
/// 32-bit (D3D8) or 64-bit (D3D9), and UE2 titles put the renderer package under System with slightly different names.
#[derive(Clone, Debug)]
pub struct Ue2Profile {
    pub bits: u32,
    pub renderer: Ue2Renderer,
    pub version: Option<u16>,
    pub configs: Vec<String>,
}

fn ue2_root(exe: &Path) -> Option<PathBuf> {
    for top in exe.ancestors().skip(1).take(5) {
        if top.join("System").is_dir()
            && (top.join("Maps").is_dir()
                || top.join("Textures").is_dir()
                || top.join("System/Engine.u").is_file()
                || top.join("System/D3DDrv.u").is_file()
                || top.join("System/D3D9Drv.u").is_file())
        {
            return Some(top.to_path_buf());
        }
    }
    None
}

fn pe_bits(path: &Path) -> Option<u32> {
    let d = std::fs::read(path).ok()?;
    if d.len() < 0x40 || &d[..2] != b"MZ" {
        return None;
    }
    let pe = u32::from_le_bytes(d.get(0x3c..0x40)?.try_into().ok()?) as usize;
    if d.get(pe..pe + 4)? != b"PE\0\0" {
        return None;
    }
    match u16::from_le_bytes(d.get(pe + 4..pe + 6)?.try_into().ok()?) {
        0x14c => Some(32),
        0x8664 => Some(64),
        _ => None,
    }
}

fn package_version(path: &Path) -> Option<u16> {
    let d = std::fs::read(path).ok()?;
    if d.len() < 8 || u32::from_le_bytes(d.get(..4)?.try_into().ok()?) != 0x9E2A83C1 {
        return None;
    }
    Some(u16::from_le_bytes(d.get(4..6)?.try_into().ok()?))
}

fn has_renderer(system: &Path, stem: &str) -> bool {
    // UE2 renderers are UnrealScript packages; the DLL beside them is the platform driver.
    system.join(format!("{stem}.u")).is_file()
}

/// Whether a game's `System` directory is Unreal Engine 2. `Engine.u`'s package version decides when it can be read:
/// UE1 is 61-68 and UE2 is 90 up (Deus Ex: Invisible War 95, UT2003 99, UT2004 118, Killing Floor 129). A renderer
/// package alone is not enough, because UE1 also ships a `D3DDrv.u`; only the D3D9 renderer is UE2-only, so it
/// settles an install whose `Engine.u` cannot be read.
pub fn is_ue2_system(system: &Path) -> bool {
    if let Some(version) = package_version(&system.join("Engine.u")) {
        return (90..500).contains(&version);
    }
    if has_renderer(system, "D3D9Drv") {
        return true;
    }
    std::fs::read_dir(system)
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| e.path().extension().map_or(false, |x| x.eq_ignore_ascii_case("u")))
        .any(|e| package_version(&e.path()).map_or(false, |v| (90..500).contains(&v)))
}

fn contains_ascii_ci(haystack: &[u8], needle: &[u8]) -> bool {
    !needle.is_empty()
        && haystack
            .windows(needle.len())
            .any(|w| w.iter().zip(needle).all(|(a, b)| a.eq_ignore_ascii_case(b)))
}

/// Find the renderer a UE2 game can use without editing it by hand. A 64-bit game uses D3D9; a 32-bit game uses the
/// D3D8 compatibility path unless its config selects D3D9 or D3D9 is the only renderer shipped. The INI list is every
/// System/*.ini that carries an [Engine.Engine] section (the game config, Default.ini and language variants).
pub fn ue2_profile(exe: &Path) -> Option<Ue2Profile> {
    let root = ue2_root(exe)?;
    let system = root.join("System");
    if !is_ue2_system(&system) {
        return None;
    }
    let bits = pe_bits(exe).unwrap_or(0);
    let has8 = has_renderer(&system, "D3DDrv");
    let has9 = has_renderer(&system, "D3D9Drv");
    let mut configs = Vec::new();
    let (mut wants8, mut wants9) = (false, false);
    for e in std::fs::read_dir(&system).into_iter().flatten().flatten() {
        let p = e.path();
        if !p.is_file() || !p.extension().map_or(false, |x| x.eq_ignore_ascii_case("ini")) {
            continue;
        }
        // Read bytes rather than UTF-8: retail UE2 configs are often ANSI/Windows-1252.
        let Ok(data) = std::fs::read(&p) else { continue };
        if !contains_ascii_ci(&data, b"[engine.engine]") {
            continue;
        }
        wants8 |= contains_ascii_ci(&data, b"d3ddrv.d3drenderdevice");
        wants9 |= contains_ascii_ci(&data, b"d3d9drv.d3d9renderdevice");
        if let Ok(rel) = p.strip_prefix(&root) {
            configs.push(rel.to_string_lossy().replace('\\', "/"));
        }
    }
    configs.sort();
    let renderer = if bits == 64 || (wants9 && !wants8) || (has9 && !has8) {
        Ue2Renderer::D3D9
    } else {
        Ue2Renderer::D3D8
    };
    Some(Ue2Profile { bits, renderer, version: package_version(&system.join("Engine.u")), configs })
}

#[derive(Clone, Debug)]
struct IniLine {
    body: Vec<u8>,
    ending: Vec<u8>,
}

fn trim_ascii(mut s: &[u8]) -> &[u8] {
    while let Some((first, rest)) = s.split_first() {
        if first.is_ascii_whitespace() {
            s = rest;
        } else {
            break;
        }
    }
    while let Some((last, rest)) = s.split_last() {
        if last.is_ascii_whitespace() {
            s = rest;
        } else {
            break;
        }
    }
    s
}

fn ini_header(line: &[u8]) -> Option<&[u8]> {
    let mut t = trim_ascii(line);
    if t.starts_with(&[0xef, 0xbb, 0xbf]) {
        t = trim_ascii(&t[3..]);
    }
    let name = t.strip_prefix(b"[")?.strip_suffix(b"]")?;
    Some(trim_ascii(name))
}

fn ini_key(line: &[u8]) -> Option<&[u8]> {
    let t = trim_ascii(line);
    if t.first().map_or(false, |b| matches!(b, b';' | b'#' | b'[')) {
        return None;
    }
    let end = t.iter().position(|b| *b == b'=')?;
    Some(trim_ascii(&t[..end]))
}

fn eq_ascii(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).all(|(x, y)| x.eq_ignore_ascii_case(y))
}

fn split_lines(data: &[u8]) -> Vec<IniLine> {
    let mut lines = Vec::new();
    let mut start = 0;
    while start < data.len() {
        if let Some(rel) = data[start..].iter().position(|b| *b == b'\n') {
            let nl = start + rel;
            let body_end = if nl > start && data[nl - 1] == b'\r' { nl - 1 } else { nl };
            lines.push(IniLine { body: data[start..body_end].to_vec(), ending: data[body_end..=nl].to_vec() });
            start = nl + 1;
        } else {
            lines.push(IniLine { body: data[start..].to_vec(), ending: Vec::new() });
            break;
        }
    }
    lines
}

/// The ending new lines are inserted with: the file's own if it has any, otherwise the Windows convention.
fn default_ending(data: &[u8]) -> &'static [u8] {
    if data.windows(2).any(|w| w == b"\r\n") {
        b"\r\n"
    } else {
        b"\n"
    }
}

fn append_line(out: &mut Vec<IniLine>, body: &[u8], ending: &[u8]) {
    out.push(IniLine { body: body.to_vec(), ending: ending.to_vec() });
}

/// Add an inserted line, making sure it starts on a new line even when the original file had no final newline.
fn insert_line(out: &mut Vec<IniLine>, body: &[u8], ending: &[u8]) {
    if let Some(last) = out.last_mut() {
        if last.ending.is_empty() {
            last.ending = ending.to_vec();
        }
    }
    append_line(out, body, ending);
}

fn append_missing_driver_keys(out: &mut Vec<IniLine>, keys: &mut [bool; 3], ending: &[u8]) {
    for (i, (key, value)) in DRIVER_KEYS.iter().enumerate() {
        if !keys[i] {
            insert_line(out, format!("{key}={value}").as_bytes(), ending);
        }
    }
    *keys = [false; 3];
}

/// Rewrite a UE2 INI without decoding it: the renderer selection and fixed-function keys are ASCII replacements,
/// while every other byte (including Windows-1252 text) and every original line ending is preserved.
pub fn patch_ue2_ini_bytes(data: &[u8], renderer: Ue2Renderer) -> Option<Vec<u8>> {
    let lines = split_lines(data);
    let ending = default_ending(data);
    let mut out = Vec::new();
    let (mut engine, mut engine_seen, mut engine_render) = (false, false, false);
    let (mut driver, mut driver_seen) = (false, false);
    let mut driver_keys = [false; 3];

    for line in lines {
        let body = &line.body;
        if let Some(name) = ini_header(body) {
            if engine && !engine_render {
                insert_line(&mut out, format!("RenderDevice={}", renderer.module()).as_bytes(), ending);
            }
            if driver {
                append_missing_driver_keys(&mut out, &mut driver_keys, ending);
            }
            append_line(&mut out, body, &line.ending);
            engine = eq_ascii(name, b"Engine.Engine");
            engine_seen |= engine;
            driver = eq_ascii(name, renderer.module().as_bytes());
            driver_seen |= driver;
            continue;
        }
        if engine && ini_key(body).map_or(false, |k| eq_ascii(k, b"RenderDevice")) {
            append_line(&mut out, format!("RenderDevice={}", renderer.module()).as_bytes(), &line.ending);
            engine_render = true;
            continue;
        }
        if driver {
            if let Some(key) = ini_key(body) {
                if let Some(i) = DRIVER_KEYS.iter().position(|(name, _)| eq_ascii(key, name.as_bytes())) {
                    append_line(&mut out, format!("{}={}", DRIVER_KEYS[i].0, DRIVER_KEYS[i].1).as_bytes(), &line.ending);
                    driver_keys[i] = true;
                    continue;
                }
            }
        }
        append_line(&mut out, body, &line.ending);
    }

    if engine && !engine_render {
        insert_line(&mut out, format!("RenderDevice={}", renderer.module()).as_bytes(), ending);
    }
    if driver {
        append_missing_driver_keys(&mut out, &mut driver_keys, ending);
    }
    if !engine_seen {
        return None;
    }
    if !driver_seen {
        insert_line(&mut out, b"", ending);
        insert_line(&mut out, format!("[{}]", renderer.module()).as_bytes(), ending);
        for (key, value) in DRIVER_KEYS {
            insert_line(&mut out, format!("{key}={value}").as_bytes(), ending);
        }
    }

    let mut result = Vec::with_capacity(data.len() + 128);
    for line in out {
        result.extend_from_slice(&line.body);
        result.extend_from_slice(&line.ending);
    }
    Some(result)
}

/// Write the patched INIs into the build output and return them as uewpack replacements (relative path, file).
pub fn ue2_compat_files(
    root: &Path,
    out: &Path,
    profile: &Ue2Profile,
    log: &mut dyn FnMut(&str),
) -> Result<Vec<(String, PathBuf)>, String> {
    let mut out_files = Vec::new();
    if profile.configs.is_empty() {
        log("[ue2] no System/*.ini with [Engine.Engine]; leaving the game config as it is");
        return Ok(out_files);
    }
    for rel in &profile.configs {
        let src = root.join(rel);
        let Ok(data) = std::fs::read(&src) else {
            log(&format!("[ue2] {rel}: can't read it; leaving it as it is"));
            continue;
        };
        let Some(patched) = patch_ue2_ini_bytes(&data, profile.renderer) else { continue };
        let dst = out.join("ue2").join(rel);
        if let Some(dir) = dst.parent() {
            std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
        }
        std::fs::write(&dst, patched).map_err(|e| e.to_string())?;
        log(&format!("[ue2] {rel}: {} fixed-function", profile.renderer.label()));
        out_files.push((rel.clone(), dst));
    }
    Ok(out_files)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn patch_ue2_ini(text: &str, renderer: Ue2Renderer) -> Option<String> {
        String::from_utf8(patch_ue2_ini_bytes(text.as_bytes(), renderer)?).ok()
    }

    #[test]
    fn patched_ini_selects_renderer_and_keeps_other_settings() {
        let src = "[Engine.Engine]\r\nRenderDevice=OpenGLDrv.OpenGLRenderDevice\r\nCacheSizeMegs=32\r\n\r\n[Core.System]\r\nPaths=..\\System\\*.u\r\n";
        let out = patch_ue2_ini(src, Ue2Renderer::D3D8).expect("Engine.Engine is present");
        assert!(out.contains("RenderDevice=D3DDrv.D3DRenderDevice"));
        assert!(out.contains("CacheSizeMegs=32"));
        assert!(out.contains("Paths=..\\System\\*.u"));
        assert!(out.contains("[D3DDrv.D3DRenderDevice]\r\nUseHardwareTL=True\r\nUseHardwareVS=False\r\nMaxPixelShaderVersion=0"));
        assert!(!out.contains("OpenGLRenderDevice"));
    }

    #[test]
    fn existing_renderer_section_is_updated_not_duplicated() {
        let src = "[Engine.Engine]\nRenderDevice=OpenGLDrv.OpenGLRenderDevice\n[D3D9Drv.D3D9RenderDevice]\nUseHardwareVS=True\nMaxPixelShaderVersion=3\nDetailTextures=True\n";
        let out = patch_ue2_ini(src, Ue2Renderer::D3D9).expect("Engine.Engine is present");
        assert!(out.contains("RenderDevice=D3D9Drv.D3D9RenderDevice"));
        assert!(out.contains("UseHardwareVS=False"));
        assert!(out.contains("MaxPixelShaderVersion=0"));
        assert!(out.contains("UseHardwareTL=True"));
        assert!(out.contains("DetailTextures=True"));
        assert_eq!(out.matches("[D3D9Drv.D3D9RenderDevice]").count(), 1);
    }

    #[test]
    fn config_without_engine_section_is_left_alone() {
        assert!(patch_ue2_ini("[Core.System]\nPaths=..\\System\\*.u\n", Ue2Renderer::D3D8).is_none());
    }

    #[test]
    fn windows_1252_bytes_and_line_endings_are_preserved() {
        let src: &[u8] = b"[Engine.Engine]\r\nRenderDevice=OpenGLDrv.OpenGLRenderDevice\r\nFriendlyName=Caf\xe9 \x80 \x92\r\n[Core.System]\r\nPaths=..\\System\\*.u\r\n";
        let out = patch_ue2_ini_bytes(src, Ue2Renderer::D3D8).expect("Engine.Engine is present");
        assert!(out.windows(b"Caf\xe9 \x80 \x92".len()).any(|w| w == b"Caf\xe9 \x80 \x92"));
        assert!(out.windows(b"RenderDevice=D3DDrv.D3DRenderDevice\r\n".len()).any(|w| w == b"RenderDevice=D3DDrv.D3DRenderDevice\r\n"));
        assert!(!out.windows(b"OpenGLRenderDevice".len()).any(|w| w == b"OpenGLRenderDevice"));
        assert!(out.windows(b"FriendlyName=Caf\xe9 \x80 \x92\r\n".len()).any(|w| w == b"FriendlyName=Caf\xe9 \x80 \x92\r\n"));
        assert!(out.windows(b"Paths=..\\System\\*.u\r\n".len()).any(|w| w == b"Paths=..\\System\\*.u\r\n"));
    }

    #[test]
    fn a_config_without_a_final_newline_still_gets_whole_lines() {
        let out = patch_ue2_ini("[Engine.Engine]\nRenderDevice=OpenGLDrv.OpenGLRenderDevice", Ue2Renderer::D3D8)
            .expect("Engine.Engine is present");
        // The replaced line becomes two whole lines, and the appended section starts on its own line.
        assert!(out.starts_with("[Engine.Engine]\nRenderDevice=D3DDrv.D3DRenderDevice\n\n[D3DDrv.D3DRenderDevice]\n"),
                "{out:?}");
        assert!(out.contains("\nUseHardwareTL=True\nUseHardwareVS=False\nMaxPixelShaderVersion=0\n"));
    }

    fn write_package(path: &Path, version: u16) {
        let mut d = vec![0u8; 64];
        d[..4].copy_from_slice(&0x9E2A83C1u32.to_le_bytes());
        d[4..6].copy_from_slice(&version.to_le_bytes());
        std::fs::write(path, d).unwrap();
    }

    fn make_install(root: &Path, bits: u16, configs: &[(&str, &[u8])]) -> PathBuf {
        std::fs::create_dir_all(root.join("System")).unwrap();
        std::fs::create_dir_all(root.join("Maps")).unwrap();
        let mut exe = vec![0u8; 0x80];
        exe[..2].copy_from_slice(b"MZ");
        exe[0x3c..0x40].copy_from_slice(&0x40u32.to_le_bytes());
        exe[0x40..0x44].copy_from_slice(b"PE\0\0");
        exe[0x44..0x46].copy_from_slice(&bits.to_le_bytes());
        let exe_path = root.join("System/game.exe");
        std::fs::write(&exe_path, exe).unwrap();
        write_package(&root.join("System/Engine.u"), 118);
        write_package(&root.join("System/D3DDrv.u"), 118);
        for (name, data) in configs {
            std::fs::write(root.join("System").join(name), data).unwrap();
        }
        exe_path
    }

    #[test]
    fn detects_ue2_renderer_and_bitness() {
        let root = std::env::temp_dir().join(format!("uewasm-ue2-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let exe = make_install(&root, 0x14c, &[("game.ini", b"[Engine.Engine]\nRenderDevice=OpenGLDrv.OpenGLRenderDevice\n")]);
        let p = ue2_profile(&exe).expect("UE2 install");
        assert_eq!(p.bits, 32);
        assert_eq!(p.renderer, Ue2Renderer::D3D8);
        assert_eq!(p.version, Some(118));
        assert_eq!(p.configs, vec!["System/game.ini"]);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn ue2_64_bit_prefers_the_d3d9_renderer() {
        let root = std::env::temp_dir().join(format!("uewasm-ue2-test64-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let exe = make_install(&root, 0x8664, &[("game.ini", b"[Engine.Engine]\nRenderDevice=D3DDrv.D3DRenderDevice\n")]);
        write_package(&root.join("System/D3D9Drv.u"), 118);
        let p = ue2_profile(&exe).expect("UE2 install");
        assert_eq!(p.bits, 64);
        assert_eq!(p.renderer, Ue2Renderer::D3D9);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn ue1_install_with_a_d3d8_package_is_not_ue2() {
        let root = std::env::temp_dir().join(format!("uewasm-ue1-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let exe = make_install(&root, 0x14c, &[]);
        // UE1 (61-68) also ships a D3DDrv.u, so only the engine package version keeps it apart from UE2.
        write_package(&root.join("System/Engine.u"), 68);
        assert!(!is_ue2_system(&root.join("System")));
        assert!(ue2_profile(&exe).is_none());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn ansi_configs_are_detected_and_patched() {
        let root = std::env::temp_dir().join(format!("uewasm-ue2-ansi-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let ini: &[u8] = b"[Engine.Engine]\r\nRenderDevice=OpenGLDrv.OpenGLRenderDevice\r\nName=Th\xe9\x80tre\r\n";
        let exe = make_install(&root, 0x14c, &[("game.ini", ini)]);
        let p = ue2_profile(&exe).expect("ANSI UE2 config is detected");
        assert_eq!(p.configs, vec!["System/game.ini"]);
        let out = root.parent().unwrap().join(format!("uewasm-ue2-ansi-out-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&out);
        ue2_compat_files(&root, &out, &p, &mut |_| {}).unwrap();
        let patched = std::fs::read(out.join("ue2/System/game.ini")).unwrap();
        assert!(patched.windows(b"Th\xe9\x80tre".len()).any(|w| w == b"Th\xe9\x80tre"));
        assert!(patched.windows(b"RenderDevice=D3DDrv.D3DRenderDevice".len()).any(|w| w == b"RenderDevice=D3DDrv.D3DRenderDevice"));
        let _ = std::fs::remove_dir_all(&root);
        let _ = std::fs::remove_dir_all(&out);
    }
}
