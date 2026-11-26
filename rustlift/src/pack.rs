//! The uewpack command line, shared by the web build and the UE2 packaging test so both use the same arguments.

use std::path::{Path, PathBuf};

/// Arguments for `uewpack`: the install root, the page directory, the module image, and every file the build
/// substitutes for one of the game's own (patched configs, decrypted paks, re-encoded videos) as `REL=PATH`.
pub fn game_files_args(
    root: &Path,
    web: &Path,
    part_mb: u64,
    workers: usize,
    compress: bool,
    image: &Path,
    replace: &[(String, PathBuf)],
) -> Vec<String> {
    let mut args = vec![
        root.display().to_string(),
        web.display().to_string(),
        "--part-mb".to_string(),
        part_mb.to_string(),
        "--workers".to_string(),
        workers.to_string(),
    ];
    if compress {
        args.push("--compress".to_string());
    }
    args.push("--extra".to_string());
    args.push(format!("/image.bin={}", image.display()));
    for (rel, path) in replace {
        args.push("--replace".to_string());
        args.push(format!("{rel}={}", path.display()));
    }
    args
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn replacements_are_passed_as_relative_equal_path() {
        let args = game_files_args(
            Path::new("C:/game"),
            Path::new("C:/out/web"),
            16,
            2,
            true,
            Path::new("C:/out/image.bin"),
            &[("System/game.ini".to_string(), PathBuf::from("C:/out/ue2/System/game.ini"))],
        );
        assert_eq!(
            args,
            ["C:/game", "C:/out/web", "--part-mb", "16", "--workers", "2", "--compress", "--extra", "/image.bin=C:/out/image.bin",
             "--replace", "System/game.ini=C:/out/ue2/System/game.ini"]
        );
    }
}
