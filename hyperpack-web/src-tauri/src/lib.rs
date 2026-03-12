use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_uint};
use serde::Serialize;

// ── FFI declarations ──────────────────────────────────────────────────────

extern "C" {
    fn hp_lib_detect_format(inpath: *const c_char) -> c_int;
    fn hp_lib_compress(
        inpath: *const c_char,
        outpath: *const c_char,
        block_mb: c_int,
        nthreads: c_int,
    ) -> c_int;
    fn hp_lib_compress_with_strategy(
        inpath: *const c_char,
        outpath: *const c_char,
        block_mb: c_int,
        nthreads: c_int,
        force_strategy: c_int,
    ) -> c_int;
    fn hp_lib_compress_filtered(
        inpath: *const c_char,
        outpath: *const c_char,
        block_mb: c_int,
        nthreads: c_int,
        allowed_mask: c_uint,
    ) -> c_int;
    fn hp_lib_decompress(inpath: *const c_char, outpath: *const c_char) -> c_int;
    fn hp_lib_archive_compress(
        npaths: c_int,
        paths: *const *const c_char,
        outpath: *const c_char,
        block_mb: c_int,
        nthreads: c_int,
    ) -> c_int;
    fn hp_lib_archive_compress_with_strategy(
        npaths: c_int,
        paths: *const *const c_char,
        outpath: *const c_char,
        block_mb: c_int,
        nthreads: c_int,
        force_strategy: c_int,
    ) -> c_int;
    fn hp_lib_archive_compress_filtered(
        npaths: c_int,
        paths: *const *const c_char,
        outpath: *const c_char,
        block_mb: c_int,
        nthreads: c_int,
        allowed_mask: c_uint,
    ) -> c_int;
    fn hp_lib_archive_decompress(
        inpath: *const c_char,
        outdir: *const c_char,
        pattern: *const c_char,
    ) -> c_int;
    fn hp_lib_archive_list(inpath: *const c_char) -> c_int;
    fn hp_lib_num_strategies() -> c_int;
    fn hp_lib_strategy_name(idx: c_int) -> *const c_char;
}

// ── Helpers ───────────────────────────────────────────────────────────────

fn auto_threads(requested: u32) -> c_int {
    if requested == 0 {
        std::thread::available_parallelism()
            .map(|n| n.get())
            .unwrap_or(1) as c_int
    } else {
        requested as c_int
    }
}

fn to_c(s: &str) -> Result<CString, String> {
    CString::new(s).map_err(|e| e.to_string())
}

/// BUG-9 fix: Recursively compute size for directories
fn file_size(path: &str) -> u64 {
    let meta = match std::fs::metadata(path) {
        Ok(m) => m,
        Err(_) => return 0,
    };
    if meta.is_dir() {
        dir_size(std::path::Path::new(path))
    } else {
        meta.len()
    }
}

fn dir_size(path: &std::path::Path) -> u64 {
    let mut total: u64 = 0;
    if let Ok(entries) = std::fs::read_dir(path) {
        for entry in entries.flatten() {
            let p = entry.path();
            if p.is_dir() {
                total += dir_size(&p);
            } else if let Ok(m) = p.metadata() {
                total += m.len();
            }
        }
    }
    total
}

// ── Response types ────────────────────────────────────────────────────────

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CompressResult {
    pub output_path: String,
    pub input_bytes: u64,
    pub output_bytes: u64,
    pub ratio: f64,
    pub elapsed_ms: u64,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DecompressResult {
    pub output_path: String,
    pub input_bytes: u64,
    pub output_bytes: u64,
    pub elapsed_ms: u64,
}

// ── Tauri commands ────────────────────────────────────────────────────────

/// Compress a single file (HPK5) with optional strategy control.
#[tauri::command]
async fn hp_compress(
    input_path: String,
    output_path: String,
    block_mb: u32,
    nthreads: u32,
    force_strategy: Option<i32>,
    allowed_mask: Option<u32>,
) -> Result<CompressResult, String> {
    let in_bytes = file_size(&input_path);
    let in_c = to_c(&input_path)?;
    let out_c = to_c(&output_path)?;
    let threads = auto_threads(nthreads);
    let bm = block_mb.max(1) as c_int;
    let fs = force_strategy.unwrap_or(-1);
    let am = allowed_mask.unwrap_or(0xFFFFFFFF);

    let start = std::time::Instant::now();
    let ret = tauri::async_runtime::spawn_blocking(move || unsafe {
        if fs >= 0 {
            hp_lib_compress_with_strategy(in_c.as_ptr(), out_c.as_ptr(), bm, threads, fs as c_int)
        } else if am != 0xFFFFFFFF {
            hp_lib_compress_filtered(in_c.as_ptr(), out_c.as_ptr(), bm, threads, am as c_uint)
        } else {
            hp_lib_compress(in_c.as_ptr(), out_c.as_ptr(), bm, threads)
        }
    })
    .await
    .map_err(|e| e.to_string())?;

    if ret != 0 {
        return Err(format!("Compression failed (code {})", ret));
    }

    let out_bytes = file_size(&output_path);
    Ok(CompressResult {
        output_path,
        input_bytes: in_bytes,
        output_bytes: out_bytes,
        ratio: in_bytes as f64 / out_bytes.max(1) as f64,
        elapsed_ms: start.elapsed().as_millis() as u64,
    })
}

/// Decompress a single HPK5 file.
#[tauri::command]
async fn hp_decompress(
    input_path: String,
    output_path: String,
) -> Result<DecompressResult, String> {
    let in_bytes = file_size(&input_path);
    let in_c = to_c(&input_path)?;
    let out_c = to_c(&output_path)?;

    let start = std::time::Instant::now();
    let ret = tauri::async_runtime::spawn_blocking(move || unsafe {
        hp_lib_decompress(in_c.as_ptr(), out_c.as_ptr())
    })
    .await
    .map_err(|e| e.to_string())?;

    if ret != 0 {
        return Err(format!("Decompression failed (code {})", ret));
    }

    let out_bytes = file_size(&output_path);
    Ok(DecompressResult {
        output_path,
        input_bytes: in_bytes,
        output_bytes: out_bytes,
        elapsed_ms: start.elapsed().as_millis() as u64,
    })
}

/// Compress multiple files/folders into an HPK6 archive with optional strategy control.
#[tauri::command]
async fn hp_archive_compress(
    input_paths: Vec<String>,
    output_path: String,
    block_mb: u32,
    nthreads: u32,
    force_strategy: Option<i32>,
    allowed_mask: Option<u32>,
) -> Result<CompressResult, String> {
    let in_bytes: u64 = input_paths.iter().map(|p| file_size(p)).sum();
    let c_strings: Vec<CString> = input_paths
        .iter()
        .map(|p| to_c(p))
        .collect::<Result<_, _>>()?;
    let out_c = to_c(&output_path)?;
    let threads = auto_threads(nthreads);
    let bm = block_mb.max(1) as c_int;
    let npaths = c_strings.len() as c_int;
    let fs = force_strategy.unwrap_or(-1);
    let am = allowed_mask.unwrap_or(0xFFFFFFFF);

    let start = std::time::Instant::now();
    let ret = tauri::async_runtime::spawn_blocking(move || {
        let c_ptrs: Vec<*const c_char> = c_strings.iter().map(|s| s.as_ptr()).collect();
        unsafe {
            if fs >= 0 {
                hp_lib_archive_compress_with_strategy(npaths, c_ptrs.as_ptr(), out_c.as_ptr(), bm, threads, fs as c_int)
            } else if am != 0xFFFFFFFF {
                hp_lib_archive_compress_filtered(npaths, c_ptrs.as_ptr(), out_c.as_ptr(), bm, threads, am as c_uint)
            } else {
                hp_lib_archive_compress(npaths, c_ptrs.as_ptr(), out_c.as_ptr(), bm, threads)
            }
        }
    })
    .await
    .map_err(|e| e.to_string())?;

    if ret != 0 {
        return Err(format!("Archive compression failed (code {})", ret));
    }

    let out_bytes = file_size(&output_path);
    Ok(CompressResult {
        output_path,
        input_bytes: in_bytes,
        output_bytes: out_bytes,
        ratio: in_bytes as f64 / out_bytes.max(1) as f64,
        elapsed_ms: start.elapsed().as_millis() as u64,
    })
}

/// Decompress an HPK6 archive to a directory.
#[tauri::command]
async fn hp_archive_decompress(
    input_path: String,
    output_dir: String,
) -> Result<DecompressResult, String> {
    let in_bytes = file_size(&input_path);
    let in_c = to_c(&input_path)?;
    let out_c = to_c(&output_dir)?;
    let pat_c = to_c("")?;

    let start = std::time::Instant::now();
    let ret = tauri::async_runtime::spawn_blocking(move || unsafe {
        hp_lib_archive_decompress(in_c.as_ptr(), out_c.as_ptr(), pat_c.as_ptr())
    })
    .await
    .map_err(|e| e.to_string())?;

    if ret != 0 {
        return Err(format!("Archive decompression failed (code {})", ret));
    }

    Ok(DecompressResult {
        output_path: output_dir,
        input_bytes: in_bytes,
        output_bytes: 0,
        elapsed_ms: start.elapsed().as_millis() as u64,
    })
}

/// Detect HPK format of a file. Returns 5 (HPK5), 6 (HPK6), or 0 (unknown).
#[tauri::command]
async fn hp_detect_format(input_path: String) -> Result<u32, String> {
    let in_c = to_c(&input_path)?;
    let fmt = unsafe { hp_lib_detect_format(in_c.as_ptr()) };
    Ok(fmt as u32)
}

/// Returns the number of logical CPU cores (for auto thread detection in the UI).
#[tauri::command]
fn get_cpu_count() -> u32 {
    std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1) as u32
}

/// Return file sizes for one or more paths (used by the UI to display sizes).
#[tauri::command]
async fn hp_file_sizes(paths: Vec<String>) -> Vec<u64> {
    paths.iter().map(|p| file_size(p)).collect()
}

// ── App entry point ───────────────────────────────────────────────────────

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .invoke_handler(tauri::generate_handler![
            hp_compress,
            hp_decompress,
            hp_archive_compress,
            hp_archive_decompress,
            hp_detect_format,
            get_cpu_count,
            hp_file_sizes,
        ])
        .run(tauri::generate_context!())
        .expect("error while running HyperPack");
}
