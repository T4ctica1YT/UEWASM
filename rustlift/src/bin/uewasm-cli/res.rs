//! How much of the PC a recompile may use. Two modes, same output:
//! - high: compile jobs on all but two logical cores, a RAM budget of about a third of the PC's memory (less when
//!   little is free), helpers below normal priority so the PC stays usable.
//! - low: about half the physical cores, a fifth of the memory, idle priority.
use crate::util::low_priority;

pub struct Plan {
    pub budget_mb: u64,
    pub compile_jobs: usize,
    pub media_workers: usize,
    pub media_threads: usize,
    pub package_workers: usize,
    pub logical: usize,
    pub physical: usize,
    pub ram_mb: u64,
}

fn machine() -> (usize, usize, u64, u64) {
    let logical = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4);
    let (mut physical, mut total, mut avail) = (0usize, 0u64, None);
    #[cfg(windows)]
    unsafe {
        let mut m = crate::util::MemStatus { len: std::mem::size_of::<crate::util::MemStatus>() as u32, ..Default::default() };
        if crate::util::GlobalMemoryStatusEx(&mut m) != 0 {
            total = m.total_phys >> 20;
            avail = Some(m.avail_phys >> 20);
        }
        // SYSTEM_LOGICAL_PROCESSOR_INFORMATION: 32 bytes each on x64, Relationship at offset 8 (0 = a core)
        let mut len = 0u32;
        crate::util::GetLogicalProcessorInformation(std::ptr::null_mut(), &mut len);
        if len > 0 {
            let mut buf = vec![0u8; len as usize];
            if crate::util::GetLogicalProcessorInformation(buf.as_mut_ptr(), &mut len) != 0 {
                let sz = if cfg!(target_pointer_width = "64") { 32 } else { 24 };
                let at = std::mem::size_of::<usize>();
                physical = buf.chunks_exact(sz).filter(|c| u32::from_le_bytes(c[at..at + 4].try_into().unwrap()) == 0).count();
            }
        }
    }
    let physical = if physical == 0 { (logical / 2).max(1) } else { physical };
    let total = if total == 0 { 8192 } else { total };
    (logical, physical, total, avail.unwrap_or(total / 2))
}

pub fn plan() -> Plan {
    let (logical, physical, total, avail) = machine();
    let (mut budget, compile_jobs, media_workers, media_threads, package_workers);
    if low_priority() {
        budget = 1024u64.max(((total as f64 * 0.20) as u64).min((avail as f64 * 0.45) as u64));
        compile_jobs = (physical / 2).max(1);
        media_workers = 1;
        media_threads = (physical / 2).max(1);
        package_workers = (physical / 2).clamp(1, 2);
    } else {
        budget = 1536u64.max(((total as f64 * 0.35) as u64).min((avail as f64 * 0.65) as u64));
        compile_jobs = logical.saturating_sub(2).max(1);
        media_workers = (physical / 4).max(1);
        media_threads = 4;
        package_workers = (physical / 2).clamp(1, 4);
    }
    if let Some(b) = std::env::var("UEW_BUILD_MB").ok().and_then(|v| v.parse().ok()) {
        budget = b;
    }
    Plan { budget_mb: budget, compile_jobs, media_workers, media_threads, package_workers, logical, physical, ram_mb: total }
}

pub fn describe() -> String {
    let p = plan();
    format!("{} priority: {} compile jobs, {:.1} GB RAM budget ({} cores / {} threads, {:.0} GB RAM)",
            if low_priority() { "low" } else { "high" }, p.compile_jobs, p.budget_mb as f64 / 1024.0, p.physical, p.logical, p.ram_mb as f64 / 1024.0)
}
