use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_uint};
use serde::Serialize;
use tauri::{AppHandle, Emitter};

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

// ── Progress event system ────────────────────────────────────────────────

/// Progress event payload emitted to the frontend via Tauri events.
/// Mirrors the WASM worker progress structure for consistency.
#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ProgressEvent {
    pub percent: u32,
    pub phase: String,
    pub current_strategy: String,
    pub best_strategy: String,
    pub best_ratio: f64,
    pub current_block: u32,
    pub total_blocks: u32,
    pub bytes_processed: u64,
    pub total_bytes: u64,
    pub current_file: String,
    pub detail: String,
}

/// Parse a stderr line from the C engine and emit a Tauri progress event.
/// Returns true if the line was recognized as a progress line.
fn parse_and_emit_progress(line: &str, state: &mut NativeProgressState, app: &AppHandle) {
    let trimmed = line.trim();
    if trimmed.is_empty() {
        return;
    }

    // [HP5] Compressing ... (170.07 MB, 2 blocks of 128 MB)
    if let Some(caps) = regex_lite::Regex::new(r"\[HP5\] Compressing .* \(([\d.]+) MB, (\d+) blocks? of (\d+) MB\)")
        .ok()
        .and_then(|re| re.captures(trimmed))
    {
        if let (Some(mb), Some(blocks)) = (caps.get(1), caps.get(2)) {
            state.total_bytes = (mb.as_str().parse::<f64>().unwrap_or(0.0) * 1048576.0) as u64;
            state.total_blocks = blocks.as_str().parse().unwrap_or(1);
            state.phase = "analyzing".into();
            emit_state(state, app);
        }
        return;
    }

    // [HPK6] N files, M dirs, X.XX MB total, B blocks
    if let Some(caps) = regex_lite::Regex::new(r"\[HPK6\] (\d+) files, (\d+) dirs, ([\d.]+) MB total, (\d+) blocks")
        .ok()
        .and_then(|re| re.captures(trimmed))
    {
        if let (Some(mb), Some(blocks)) = (caps.get(3), caps.get(4)) {
            state.total_bytes = (mb.as_str().parse::<f64>().unwrap_or(0.0) * 1048576.0) as u64;
            state.total_blocks = blocks.as_str().parse().unwrap_or(1);
            state.phase = "analyzing".into();
            emit_state(state, app);
        }
        return;
    }

    // [HPK6] Scanned N entries
    if trimmed.contains("[HPK6] Scanned") {
        state.phase = "scanning".into();
        emit_state(state, app);
        return;
    }

    // PNG detection
    if trimmed.contains("[HP5] PNG detected") {
        state.phase = "analyzing".into();
        state.current_strategy = "PNG pre-transform".into();
        emit_state(state, app);
        return;
    }

    // Entropy analysis
    if trimmed.contains("[Entropy") {
        state.phase = "analyzing".into();
        state.current_strategy = "Entropy analysis".into();
        emit_state(state, app);
        return;
    }

    // Strategy result: "[LZMA] 8388608 -> 2345678 (3.58x) *** NEW BEST ***"
    if let Some(caps) = regex_lite::Regex::new(r"\[([A-Za-z0-9+_]+)\]\s+(\d+)\s*->\s*(\d+)\s*\(([\d.]+)x\)")
        .ok()
        .and_then(|re| re.captures(trimmed))
    {
        if let (Some(name), Some(ratio_str)) = (caps.get(1), caps.get(4)) {
            let ratio = ratio_str.as_str().parse::<f64>().unwrap_or(0.0);
            state.phase = "testing".into();
            state.current_strategy = name.as_str().into();
            if trimmed.contains("NEW BEST") || ratio > state.best_ratio {
                state.best_ratio = ratio;
                state.best_strategy = name.as_str().into();
            }
            emit_state(state, app);
        }
        return;
    }

    // Sub-stream results: "[Sub PPM] N -> M (Xx)"
    if let Some(caps) = regex_lite::Regex::new(r"\[Sub ([A-Za-z0-9+_]+)\]")
        .ok()
        .and_then(|re| re.captures(trimmed))
    {
        if let Some(name) = caps.get(1) {
            state.phase = "testing".into();
            state.current_strategy = name.as_str().into();
            emit_state(state, app);
        }
        return;
    }

    // Strategy skipped
    if trimmed.contains("not detected, skipping") || trimmed.contains("skipped by heuristic") {
        if let Some(caps) = regex_lite::Regex::new(r"\[([A-Za-z0-9+_]+)\]")
            .ok()
            .and_then(|re| re.captures(trimmed))
        {
            if let Some(name) = caps.get(1) {
                state.phase = "testing".into();
                state.current_strategy = format!("{} (skip)", name.as_str());
                emit_state(state, app);
            }
        }
        return;
    }

    // Block completion: "Block 1/2: 134217728 -> 41234567 (3.25x) [LZP+BWT+O1]"
    if let Some(caps) = regex_lite::Regex::new(r"Block (\d+)/(\d+):\s+(\d+)\s*->\s*(\d+)\s*\(([\d.]+)x\)\s*\[([^\]]+)\]")
        .ok()
        .and_then(|re| re.captures(trimmed))
    {
        if let (Some(cur), Some(tot), Some(bin), Some(ratio_str), Some(strat)) =
            (caps.get(1), caps.get(2), caps.get(3), caps.get(5), caps.get(6))
        {
            state.current_block = cur.as_str().parse().unwrap_or(0);
            state.total_blocks = tot.as_str().parse().unwrap_or(1);
            state.bytes_processed += bin.as_str().parse::<u64>().unwrap_or(0);
            state.phase = "block-done".into();
            state.current_strategy = strat.as_str().into();
            state.best_strategy = strat.as_str().into();
            state.best_ratio = ratio_str.as_str().parse().unwrap_or(0.0);
            emit_state(state, app);
        }
        return;
    }

    // Block DUP
    if trimmed.contains("DUP of") {
        if let Some(caps) = regex_lite::Regex::new(r"Block (\d+)")
            .ok()
            .and_then(|re| re.captures(trimmed))
        {
            if let Some(cur) = caps.get(1) {
                state.current_block = cur.as_str().parse().unwrap_or(0);
                state.phase = "block-done".into();
                state.current_strategy = "DUP (dedup)".into();
                emit_state(state, app);
            }
        }
        return;
    }

    // HPK6 block with file: "Block 5 [file.txt:1/3]: ..."
    if let Some(caps) = regex_lite::Regex::new(r"Block (\d+) \[([^\]]+):\d+/\d+\]:\s+(\d+)\s*->\s*\d+\s*\(([\d.]+)x\)\s*\[([^\]]+)\]")
        .ok()
        .and_then(|re| re.captures(trimmed))
    {
        if let (Some(blk), Some(file), Some(bin), Some(ratio_str), Some(strat)) =
            (caps.get(1), caps.get(2), caps.get(3), caps.get(4), caps.get(5))
        {
            state.current_block = blk.as_str().parse().unwrap_or(0);
            state.current_file = file.as_str().into();
            state.bytes_processed += bin.as_str().parse::<u64>().unwrap_or(0);
            state.phase = "block-done".into();
            state.current_strategy = strat.as_str().into();
            state.best_strategy = strat.as_str().into();
            state.best_ratio = ratio_str.as_str().parse().unwrap_or(0.0);
            emit_state(state, app);
        }
        return;
    }

    // Done messages
    if trimmed.contains("[HP5] Done:") || trimmed.contains("[HPK6] Done:") {
        state.phase = "done".into();
        emit_state(state, app);
    }
}

struct NativeProgressState {
    total_blocks: u32,
    current_block: u32,
    total_bytes: u64,
    bytes_processed: u64,
    phase: String,
    current_strategy: String,
    best_strategy: String,
    best_ratio: f64,
    current_file: String,
}

impl Default for NativeProgressState {
    fn default() -> Self {
        Self {
            total_blocks: 1,
            current_block: 0,
            total_bytes: 0,
            bytes_processed: 0,
            phase: "init".into(),
            current_strategy: String::new(),
            best_strategy: String::new(),
            best_ratio: 0.0,
            current_file: String::new(),
        }
    }
}

fn emit_state(state: &NativeProgressState, app: &AppHandle) {
    let percent = if state.total_blocks > 0 {
        ((state.current_block as f64 / state.total_blocks as f64) * 100.0) as u32
    } else {
        0
    };
    let _ = app.emit("hp-progress", ProgressEvent {
        percent,
        phase: state.phase.clone(),
        current_strategy: state.current_strategy.clone(),
        best_strategy: state.best_strategy.clone(),
        best_ratio: state.best_ratio,
        current_block: state.current_block,
        total_blocks: state.total_blocks,
        bytes_processed: state.bytes_processed,
        total_bytes: state.total_bytes,
        current_file: state.current_file.clone(),
        detail: state.current_strategy.clone(),
    });
}

/// Run a C compression function while capturing stderr and emitting progress events.
/// The closure receives no args and should call the appropriate hp_lib_* function.
/// stderr is redirected via a pipe so progress lines are captured in real-time.
#[cfg(unix)]
fn run_with_progress<F: FnOnce() -> c_int + Send + 'static>(
    app: AppHandle,
    f: F,
) -> Result<c_int, String> {
    use std::os::unix::io::FromRawFd;
    use std::io::{BufRead, BufReader};

    // Create a pipe for stderr
    let mut pipe_fds = [0i32; 2];
    if unsafe { libc::pipe(pipe_fds.as_mut_ptr()) } != 0 {
        // Fallback: run without progress
        let ret = f();
        return Ok(ret);
    }
    let read_fd = pipe_fds[0];
    let write_fd = pipe_fds[1];

    // Save original stderr
    let orig_stderr = unsafe { libc::dup(2) };
    // Redirect stderr to pipe
    unsafe { libc::dup2(write_fd, 2); }

    // Spawn reader thread to parse progress from stderr pipe
    let app_clone = app.clone();
    let reader_handle = std::thread::spawn(move || {
        let file = unsafe { std::fs::File::from_raw_fd(read_fd) };
        let reader = BufReader::new(file);
        let mut state = NativeProgressState::default();
        for line in reader.lines() {
            if let Ok(line) = line {
                parse_and_emit_progress(&line, &mut state, &app_clone);
            }
        }
    });

    // Run the actual compression
    let ret = f();

    // Restore stderr and close write end to signal reader thread
    unsafe {
        libc::dup2(orig_stderr, 2);
        libc::close(orig_stderr);
        libc::close(write_fd);
    }

    // Wait for reader thread
    let _ = reader_handle.join();

    Ok(ret)
}

/// Windows version: uses _pipe and _dup2
#[cfg(windows)]
fn run_with_progress<F: FnOnce() -> c_int + Send + 'static>(
    app: AppHandle,
    f: F,
) -> Result<c_int, String> {
    use std::io::{BufRead, BufReader};
    use std::os::windows::io::FromRawHandle;

    let mut read_fd: i32 = 0;
    let mut write_fd: i32 = 0;

    extern "C" {
        fn _pipe(pfds: *mut i32, psize: u32, textmode: i32) -> i32;
        fn _dup(fd: i32) -> i32;
        fn _dup2(fd1: i32, fd2: i32) -> i32;
        fn _close(fd: i32) -> i32;
        fn _get_osfhandle(fd: i32) -> isize;
    }

    unsafe {
        if _pipe(&mut read_fd as *mut i32, 65536, 0x8000 /* _O_BINARY */) != 0 {
            let ret = f();
            return Ok(ret);
        }
    }

    let orig_stderr = unsafe { _dup(2) };
    unsafe { _dup2(write_fd, 2); }

    let app_clone = app.clone();
    let reader_handle = std::thread::spawn(move || {
        let handle = unsafe { _get_osfhandle(read_fd) };
        let file = unsafe { std::fs::File::from_raw_handle(handle as *mut std::ffi::c_void) };
        let reader = BufReader::new(file);
        let mut state = NativeProgressState::default();
        for line in reader.lines() {
            if let Ok(line) = line {
                parse_and_emit_progress(&line, &mut state, &app_clone);
            }
        }
    });

    let ret = f();

    unsafe {
        _dup2(orig_stderr, 2);
        _close(orig_stderr);
        _close(write_fd);
    }

    let _ = reader_handle.join();

    Ok(ret)
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
    app: AppHandle,
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
    let ret = tauri::async_runtime::spawn_blocking(move || {
        run_with_progress(app, move || unsafe {
            if fs >= 0 {
                hp_lib_compress_with_strategy(in_c.as_ptr(), out_c.as_ptr(), bm, threads, fs as c_int)
            } else if am != 0xFFFFFFFF {
                hp_lib_compress_filtered(in_c.as_ptr(), out_c.as_ptr(), bm, threads, am as c_uint)
            } else {
                hp_lib_compress(in_c.as_ptr(), out_c.as_ptr(), bm, threads)
            }
        })
    })
    .await
    .map_err(|e| e.to_string())?
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
    app: AppHandle,
    input_path: String,
    output_path: String,
) -> Result<DecompressResult, String> {
    let in_bytes = file_size(&input_path);
    let in_c = to_c(&input_path)?;
    let out_c = to_c(&output_path)?;

    let start = std::time::Instant::now();
    let ret = tauri::async_runtime::spawn_blocking(move || {
        run_with_progress(app, move || unsafe {
            hp_lib_decompress(in_c.as_ptr(), out_c.as_ptr())
        })
    })
    .await
    .map_err(|e| e.to_string())?
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
    app: AppHandle,
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
        run_with_progress(app, move || {
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
    })
    .await
    .map_err(|e| e.to_string())?
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
    app: AppHandle,
    input_path: String,
    output_dir: String,
) -> Result<DecompressResult, String> {
    let in_bytes = file_size(&input_path);
    let in_c = to_c(&input_path)?;
    let out_c = to_c(&output_dir)?;
    let pat_c = to_c("")?;

    let start = std::time::Instant::now();
    let ret = tauri::async_runtime::spawn_blocking(move || {
        run_with_progress(app, move || unsafe {
            hp_lib_archive_decompress(in_c.as_ptr(), out_c.as_ptr(), pat_c.as_ptr())
        })
    })
    .await
    .map_err(|e| e.to_string())?
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
