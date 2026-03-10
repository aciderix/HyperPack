/**
 * HyperPack native bridge for Tauri desktop builds.
 * Wraps Tauri invoke() calls behind the same interface used by the WASM worker.
 */

// Tauri 2 lazy imports — only loaded when running inside Tauri
let _invoke: ((cmd: string, args?: Record<string, unknown>) => Promise<unknown>) | null = null;
let _open: ((options: Record<string, unknown>) => Promise<string | string[] | null>) | null = null;
let _save: ((options: Record<string, unknown>) => Promise<string | null>) | null = null;
let _revealItem: ((path: string) => Promise<void>) | null = null;

async function getInvoke() {
  if (!_invoke) {
    const m = await import('@tauri-apps/api/core');
    _invoke = m.invoke as typeof _invoke;
  }
  return _invoke!;
}

async function getDialog() {
  if (!_open) {
    const m = await import('@tauri-apps/plugin-dialog');
    _open = m.open as typeof _open;
    _save = m.save as typeof _save;
  }
  return { open: _open!, save: _save! };
}

async function getOpener() {
  if (!_revealItem) {
    const m = await import('@tauri-apps/plugin-opener');
    _revealItem = m.revealItemInDir as typeof _revealItem;
  }
  return _revealItem!;
}

/** Returns true when running inside the Tauri webview. */
export const isNative = (): boolean =>
  typeof window !== 'undefined' && '__TAURI_INTERNALS__' in window;

// ── Result types ──────────────────────────────────────────────────────────

export interface NativeCompressResult {
  outputPath: string;
  inputBytes: number;
  outputBytes: number;
  ratio: number;
  elapsedMs: number;
}

export interface NativeDecompressResult {
  outputPath: string;
  inputBytes: number;
  outputBytes: number;
  elapsedMs: number;
}

// ── File dialogs ──────────────────────────────────────────────────────────

/** Open a native file picker. Returns selected path(s) or null. */
export async function openFilePicker(opts: {
  multiple?: boolean;
  filters?: Array<{ name: string; extensions: string[] }>;
}): Promise<string[] | null> {
  const { open } = await getDialog();
  const result = await open({ multiple: opts.multiple ?? false, filters: opts.filters ?? [] });
  if (!result) return null;
  return Array.isArray(result) ? result : [result];
}

/** Open a native folder picker. Returns selected path or null. */
export async function openFolderPicker(): Promise<string | null> {
  const { open } = await getDialog();
  const result = await open({ directory: true, multiple: false });
  if (!result) return null;
  return Array.isArray(result) ? result[0] : result;
}

/** Open a native save dialog. Returns chosen path or null. */
export async function saveFilePicker(opts: {
  defaultPath?: string;
  filters?: Array<{ name: string; extensions: string[] }>;
}): Promise<string | null> {
  const { save } = await getDialog();
  return save({ defaultPath: opts.defaultPath, filters: opts.filters ?? [] });
}

/** Reveal a file/folder in the system file manager. */
export async function revealInFinder(path: string): Promise<void> {
  const reveal = await getOpener();
  await reveal(path);
}

// ── Engine commands ───────────────────────────────────────────────────────

/** Returns the number of logical CPU cores. */
export async function getCpuCount(): Promise<number> {
  const invoke = await getInvoke();
  return invoke('get_cpu_count') as Promise<number>;
}

/** Detect HPK format. Returns 5, 6, or 0. */
export async function detectFormat(inputPath: string): Promise<number> {
  const invoke = await getInvoke();
  return invoke('hp_detect_format', { inputPath }) as Promise<number>;
}

/** Compress a single file (HPK5). */
export async function compress(
  inputPath: string,
  outputPath: string,
  blockMb: number,
  nthreads: number,
): Promise<NativeCompressResult> {
  const invoke = await getInvoke();
  return invoke('hp_compress', {
    inputPath,
    outputPath,
    blockMb,
    nthreads,
  }) as Promise<NativeCompressResult>;
}

/** Decompress an HPK5 file. */
export async function decompress(
  inputPath: string,
  outputPath: string,
): Promise<NativeDecompressResult> {
  const invoke = await getInvoke();
  return invoke('hp_decompress', { inputPath, outputPath }) as Promise<NativeDecompressResult>;
}

/** Compress multiple files/folders into an HPK6 archive. */
export async function archiveCompress(
  inputPaths: string[],
  outputPath: string,
  blockMb: number,
  nthreads: number,
): Promise<NativeCompressResult> {
  const invoke = await getInvoke();
  return invoke('hp_archive_compress', {
    inputPaths,
    outputPath,
    blockMb,
    nthreads,
  }) as Promise<NativeCompressResult>;
}

/** Decompress an HPK6 archive to a directory. */
export async function archiveDecompress(
  inputPath: string,
  outputDir: string,
): Promise<NativeDecompressResult> {
  const invoke = await getInvoke();
  return invoke('hp_archive_decompress', { inputPath, outputDir }) as Promise<NativeDecompressResult>;
}

/** Derive a default output path for compression (adds .hpk). */
export function defaultCompressOutput(inputPath: string): string {
  return inputPath + '.hpk';
}

/** Derive a default output path for decompression (removes .hpk, adds _out if needed). */
export function defaultDecompressOutput(inputPath: string): string {
  if (inputPath.endsWith('.hpk')) {
    return inputPath.slice(0, -4);
  }
  return inputPath + '_out';
}

/** Derive a default output directory for archive decompression. */
export function defaultDecompressDir(inputPath: string): string {
  const base = inputPath.endsWith('.hpk') ? inputPath.slice(0, -4) : inputPath + '_out';
  return base;
}
