//! End-to-end UE2 packaging: build a synthetic install, find its root with the real recompiler, patch an ANSI config
//! with the build's own code, and package it with uewpack using the build's own argument list.

use std::path::{Path, PathBuf};
use std::process::Command;

fn temp_dir(label: &str) -> PathBuf {
    let root = std::env::temp_dir().join(format!("uewasm-ue2-e2e-{}-{label}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    root
}

fn write_pe(path: &Path, machine: u16) {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).unwrap();
    }
    let mut data = vec![0u8; 4096];
    data[..2].copy_from_slice(b"MZ");
    data[0x3c..0x40].copy_from_slice(&0x40u32.to_le_bytes());
    data[0x40..0x44].copy_from_slice(b"PE\0\0");
    data[0x44..0x46].copy_from_slice(&machine.to_le_bytes());
    data[0x58..0x5a].copy_from_slice(&0x010bu16.to_le_bytes());
    std::fs::write(path, data).unwrap();
}

fn write_package(path: &Path, version: u16) {
    let mut data = vec![0u8; 64];
    data[..4].copy_from_slice(&0x9E2A83C1u32.to_le_bytes());
    data[4..6].copy_from_slice(&version.to_le_bytes());
    std::fs::write(path, data).unwrap();
}

fn output(cmd: &mut Command) -> std::process::Output {
    let out = cmd.output().unwrap();
    assert!(out.status.success(), "command failed: {}\n{}", cmd.get_program().to_string_lossy(),
            String::from_utf8_lossy(&out.stderr));
    out
}

#[test]
fn synthetic_ue2_install_is_patched_and_packaged_from_its_install_root() {
    // Keep the install and the web output under separate temp roots: uewpack deliberately excludes files under the
    // web directory's parent, because a real build output often lives beside the game install.
    let install_base = temp_dir("install-base");
    let root = install_base.join("install");
    std::fs::create_dir_all(root.join("System")).unwrap();
    std::fs::create_dir_all(root.join("Maps")).unwrap();
    std::fs::create_dir_all(root.join("Textures")).unwrap();
    let exe = root.join("System/game.exe");
    write_pe(&exe, 0x014c);
    write_pe(&root.join("System/plugin.dll"), 0x014c);
    write_package(&root.join("System/Engine.u"), 118);
    write_package(&root.join("System/D3DDrv.u"), 118);
    let ini = b"[Engine.Engine]\r\nRenderDevice=OpenGLDrv.OpenGLRenderDevice\r\nFriendlyName=Th\xe9\x80tre\r\n[Core.System]\r\nPaths=..\\System\\*.u\r\n";
    std::fs::write(root.join("System/game.ini"), ini).unwrap();
    std::fs::write(root.join("Maps/Test.ut2"), b"map").unwrap();
    std::fs::write(root.join("Textures/Test.utx"), b"texture").unwrap();

    // The recompiler must treat System's parent as the install root, not System itself, or Maps/Textures are lost.
    let out = output(Command::new(env!("CARGO_BIN_EXE_uewrecomp")).arg("game-dlls").arg(&exe));
    let report: serde_json::Value = serde_json::from_slice(&out.stdout).unwrap();
    let install_root = PathBuf::from(report["root"].as_str().unwrap());
    assert_eq!(install_root, root);
    let dlls: Vec<&str> = report["dlls"].as_array().unwrap().iter().filter_map(|d| d.as_str()).collect();
    assert!(dlls.iter().any(|d| d.ends_with("plugin.dll")), "shipped DLL was not found: {dlls:?}");

    // The ANSI config must be detected from its UE2 package version and patched without decoding its text.
    let profile = uewlift::ue2::ue2_profile(&install_root.join("System/game.exe")).expect("UE2 install was not detected");
    assert_eq!(profile.bits, 32);
    assert_eq!(profile.renderer, uewlift::ue2::Ue2Renderer::D3D8);
    assert_eq!(profile.version, Some(118));
    assert_eq!(profile.configs, vec!["System/game.ini"]);
    let build = temp_dir("build-base").join("build");
    let replacements = uewlift::ue2::ue2_compat_files(&install_root, &build, &profile, &mut |_| {}).unwrap();
    assert_eq!(replacements.len(), 1);
    let patched = std::fs::read(build.join("ue2/System/game.ini")).unwrap();
    assert!(patched.windows(b"Th\xe9\x80tre".len()).any(|w| w == b"Th\xe9\x80tre"));
    assert!(patched.windows(b"RenderDevice=D3DDrv.D3DRenderDevice".len()).any(|w| w == b"RenderDevice=D3DDrv.D3DRenderDevice"));

    // Package exactly as build_wasm does, through the same argument list, with the patched config replacing the
    // game's original.
    let image = build.join("image.bin");
    std::fs::write(&image, b"image").unwrap();
    let web = temp_dir("web-base").join("web");
    let args = uewlift::pack::game_files_args(&install_root, &web, 16, 1, false, &image, &replacements);
    output(Command::new(env!("CARGO_BIN_EXE_uewpack")).args(&args));
    let index = std::fs::read_to_string(web.join("data/index.txt")).unwrap();
    for path in ["/game/System/game.ini", "/game/Maps/Test.ut2", "/game/Textures/Test.utx", "/image.bin"] {
        assert!(index.contains(path), "{path} was not packaged from the install root:\n{index}");
    }
    let mut packed_config = None;
    for entry in std::fs::read_dir(web.join("data")).unwrap().flatten() {
        let data = std::fs::read(entry.path()).unwrap_or_default();
        if data.windows(b"Th\xe9\x80tre".len()).any(|w| w == b"Th\xe9\x80tre")
            && data.windows(b"RenderDevice=D3DDrv.D3DRenderDevice".len()).any(|w| w == b"RenderDevice=D3DDrv.D3DRenderDevice")
        {
            packed_config = Some(data);
            break;
        }
    }
    assert!(packed_config.is_some(), "the patched ANSI config was not written to the packaged data");

    let _ = std::fs::remove_dir_all(&install_base);
    let _ = std::fs::remove_dir_all(&build);
    let _ = std::fs::remove_dir_all(build.parent().unwrap());
    let _ = std::fs::remove_dir_all(&web);
    let _ = std::fs::remove_dir_all(web.parent().unwrap());
}
