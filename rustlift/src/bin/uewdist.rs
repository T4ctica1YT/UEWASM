//! The release: builds the tools and the desktop app, lays them out in dist/UEWASM/ beside the runtime's sources and
//! the assets, and zips the folder (dist/UEWASM-<version>.zip).
//!
//!     cargo run --release --bin uewdist [-- --version 0.1.5]
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;

const TOOLS: &[&str] = &["uewasm-cli", "uewlift", "uewrecomp", "uewbuild", "uewpack", "uewserve"];

fn die(m: impl std::fmt::Display) -> ! {
    eprintln!("uewdist: {m}");
    std::process::exit(1)
}

fn run(c: &mut Command) {
    let st = c.status().unwrap_or_else(|e| die(format!("{:?}: {e}", c.get_program())));
    if !st.success() {
        die(format!("{:?} failed", c.get_program()));
    }
}

fn copy_tree(src: &Path, dst: &Path, skip: &dyn Fn(&Path) -> bool) {
    std::fs::create_dir_all(dst).unwrap_or_else(|e| die(e));
    let mut v: Vec<_> = std::fs::read_dir(src).unwrap_or_else(|e| die(format!("{}: {e}", src.display()))).flatten().map(|e| e.path()).collect();
    v.sort();
    for p in v {
        if skip(&p) {
            continue;
        }
        let t = dst.join(p.file_name().unwrap());
        if p.is_dir() {
            copy_tree(&p, &t, skip);
        } else {
            std::fs::copy(&p, &t).unwrap_or_else(|e| die(format!("{}: {e}", p.display())));
        }
    }
}

fn crc32(d: &[u8]) -> u32 {
    let mut c = flate2::Crc::new();
    c.update(d);
    c.sum()
}

/// a deflated .zip of a folder (entries under its own name)
fn zip_dir(dir: &Path, dest: &Path) {
    let mut files = vec![];
    fn walk(d: &Path, out: &mut Vec<PathBuf>) {
        let mut v: Vec<_> = std::fs::read_dir(d).map(|r| r.flatten().map(|e| e.path()).collect()).unwrap_or_default();
        v.sort();
        for p in v {
            if p.is_dir() { walk(&p, out) } else { out.push(p) }
        }
    }
    walk(dir, &mut files);
    let base = dir.parent().unwrap();
    let mut out = std::io::BufWriter::new(std::fs::File::create(dest).unwrap_or_else(|e| die(e)));
    let (mut cd, mut off, mut n) = (vec![], 0u64, 0u16);
    for f in files {
        let name = f.strip_prefix(base).unwrap().to_string_lossy().replace('\\', "/");
        let data = std::fs::read(&f).unwrap_or_else(|e| die(e));
        let mut z = flate2::write::DeflateEncoder::new(vec![], flate2::Compression::best());
        z.write_all(&data).unwrap();
        let comp = z.finish().unwrap();
        let (method, body) = if comp.len() < data.len() { (8u16, comp) } else { (0u16, data.clone()) };
        if off > u32::MAX as u64 || data.len() > u32::MAX as usize {
            die("the release is too big for a plain zip");
        }
        let crc = crc32(&data);
        let mut h = vec![];
        h.extend(0x04034b50u32.to_le_bytes());
        h.extend([20, 0, 0, 8]);
        h.extend(method.to_le_bytes());
        h.extend([0, 0, 0x21, 0]);
        h.extend(crc.to_le_bytes());
        h.extend((body.len() as u32).to_le_bytes());
        h.extend((data.len() as u32).to_le_bytes());
        h.extend((name.len() as u16).to_le_bytes());
        h.extend(0u16.to_le_bytes());
        h.extend(name.as_bytes());
        out.write_all(&h).unwrap();
        out.write_all(&body).unwrap();
        cd.extend(0x02014b50u32.to_le_bytes());
        cd.extend([20, 0, 20, 0, 0, 8]);
        cd.extend(method.to_le_bytes());
        cd.extend([0, 0, 0x21, 0]);
        cd.extend(crc.to_le_bytes());
        cd.extend((body.len() as u32).to_le_bytes());
        cd.extend((data.len() as u32).to_le_bytes());
        cd.extend((name.len() as u16).to_le_bytes());
        cd.extend([0u8; 12]);
        cd.extend((off as u32).to_le_bytes());
        cd.extend(name.as_bytes());
        off += (h.len() + body.len()) as u64;
        n += 1;
    }
    out.write_all(&cd).unwrap();
    let mut e = vec![];
    e.extend(0x06054b50u32.to_le_bytes());
    e.extend([0u8; 4]);
    e.extend(n.to_le_bytes());
    e.extend(n.to_le_bytes());
    e.extend((cd.len() as u32).to_le_bytes());
    e.extend((off as u32).to_le_bytes());
    e.extend([0u8; 2]);
    out.write_all(&e).unwrap();
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let version = args.iter().position(|a| a == "--version").and_then(|i| args.get(i + 1)).cloned().unwrap_or_else(|| env!("CARGO_PKG_VERSION").into());
    let rl = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let repo = rl.parent().unwrap().to_path_buf();
    let cargo = std::env::var("CARGO").unwrap_or_else(|_| "cargo".into());
    let mut c = Command::new(&cargo);
    c.args(["build", "--release", "--manifest-path"]).arg(rl.join("Cargo.toml"));
    for t in TOOLS {
        c.args(["--bin", t]);
    }
    run(&mut c);
    run(Command::new(&cargo).args(["build", "--release", "--target", "x86_64-pc-windows-msvc", "--manifest-path"]).arg(repo.join("app/Cargo.toml")));
    let dist = repo.join("dist");
    let out = dist.join("UEWASM");
    let _ = std::fs::remove_dir_all(&out);
    std::fs::create_dir_all(&out).unwrap_or_else(|e| die(e));
    std::fs::copy(repo.join("app/target/x86_64-pc-windows-msvc/release/uewasm-app.exe"), out.join("UEWASM.exe")).unwrap_or_else(|e| die(e));
    for t in TOOLS {
        let n = format!("{t}.exe");
        std::fs::copy(rl.join("target/release").join(&n), out.join(&n)).unwrap_or_else(|e| die(format!("{n}: {e}")));
    }
    // only what a build needs at run time: the runtime's C sources and web glue, and the assets
    copy_tree(&repo.join("runtime"), &out.join("runtime"), &|p| {
        let e = p.extension().map(|e| e.to_string_lossy().to_lowercase()).unwrap_or_default();
        e == "o" || e == "obj"
    });
    copy_tree(&repo.join("assets"), &out.join("assets"), &|p| p.file_name().map_or(false, |n| n == "ss"));
    for f in ["README.md", "LICENSE"] {
        let _ = std::fs::copy(repo.join(f), out.join(f));
    }
    let zip = dist.join(format!("UEWASM-{version}.zip"));
    zip_dir(&out, &zip);
    println!("release in {} ({:.1} MB zipped: {})", out.display(), std::fs::metadata(&zip).map_or(0, |m| m.len()) as f64 / 1048576.0, zip.display());
}
