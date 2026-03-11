import { useState, useEffect, useRef, useCallback } from 'react';
import { CompressParams, FileEntry, ExtractedFile, WorkerResponse, computeStrategyArgs } from '../workers/bridge';
import * as native from '../lib/native';

export type { ExtractedFile } from '../workers/bridge';

export type HyperPackStatus = 'idle' | 'processing' | 'complete' | 'error';

export type HyperPackProgress = {
  percent: number;
  currentBlock: number;
  totalBlocks: number;
  strategy: string;
  speed: number;
  eta: number;
  currentRatio?: number;
  detail?: string;
};

export type HyperPackResult = {
  inputSize: number;
  outputSize: number;
  ratio: number;
  totalMs: number;
  blocks: Array<{ strategy: string; inputSize: number; outputSize: number }>;
  outputBuffer: ArrayBuffer;
  outputFileName: string;
  outputPath?: string;
  fileCount?: number;
  dedupCount?: number;
  dedupSaved?: number;
};

export type ListEntry = {
  type: string;
  path: string;
  size: number;
  crc: string;
  blocks: number;
  isDedup: boolean;
};

function createWorker(): Worker {
  return new Worker(import.meta.env.BASE_URL + 'worker.js');
}

/* ── Minimal ZIP builder (no external deps) ── */
function buildZip(files: ExtractedFile[]): ArrayBuffer {
  const textEncoder = new TextEncoder();
  const centralDir: Uint8Array[] = [];
  const localParts: Uint8Array[] = [];
  let offset = 0;

  for (const file of files) {
    const nameBytes = textEncoder.encode(file.name);
    const fileData = new Uint8Array(file.data);

    // Local file header (30 bytes + name + data)
    const local = new ArrayBuffer(30 + nameBytes.length);
    const lv = new DataView(local);
    lv.setUint32(0, 0x04034b50, true);   // signature
    lv.setUint16(4, 20, true);            // version needed
    lv.setUint16(6, 0, true);             // flags
    lv.setUint16(8, 0, true);             // compression: stored
    lv.setUint16(10, 0, true);            // mod time
    lv.setUint16(12, 0, true);            // mod date
    lv.setUint32(14, 0, true);            // crc32 (0 for stored)
    lv.setUint32(18, fileData.length, true); // compressed size
    lv.setUint32(22, fileData.length, true); // uncompressed size
    lv.setUint16(26, nameBytes.length, true); // name length
    lv.setUint16(28, 0, true);            // extra length
    new Uint8Array(local).set(nameBytes, 30);

    localParts.push(new Uint8Array(local));
    localParts.push(fileData);

    // Central directory entry (46 bytes + name)
    const central = new ArrayBuffer(46 + nameBytes.length);
    const cv = new DataView(central);
    cv.setUint32(0, 0x02014b50, true);    // signature
    cv.setUint16(4, 20, true);            // version made by
    cv.setUint16(6, 20, true);            // version needed
    cv.setUint16(8, 0, true);             // flags
    cv.setUint16(10, 0, true);            // compression: stored
    cv.setUint16(12, 0, true);            // mod time
    cv.setUint16(14, 0, true);            // mod date
    cv.setUint32(16, 0, true);            // crc32
    cv.setUint32(20, fileData.length, true); // compressed size
    cv.setUint32(24, fileData.length, true); // uncompressed size
    cv.setUint16(28, nameBytes.length, true); // name length
    cv.setUint16(30, 0, true);            // extra length
    cv.setUint16(32, 0, true);            // comment length
    cv.setUint16(34, 0, true);            // disk number
    cv.setUint16(36, 0, true);            // internal attrs
    cv.setUint32(38, 0, true);            // external attrs
    cv.setUint32(42, offset, true);       // local header offset
    new Uint8Array(central).set(nameBytes, 46);

    centralDir.push(new Uint8Array(central));
    offset += 30 + nameBytes.length + fileData.length;
  }

  const centralDirOffset = offset;
  let centralDirSize = 0;
  for (const cd of centralDir) centralDirSize += cd.length;

  // End of central directory (22 bytes)
  const eocd = new ArrayBuffer(22);
  const ev = new DataView(eocd);
  ev.setUint32(0, 0x06054b50, true);
  ev.setUint16(4, 0, true);
  ev.setUint16(6, 0, true);
  ev.setUint16(8, files.length, true);
  ev.setUint16(10, files.length, true);
  ev.setUint32(12, centralDirSize, true);
  ev.setUint32(16, centralDirOffset, true);
  ev.setUint16(20, 0, true);

  // Concatenate everything
  const totalSize = offset + centralDirSize + 22;
  const result = new Uint8Array(totalSize);
  let pos = 0;
  for (const part of localParts) { result.set(part, pos); pos += part.length; }
  for (const cd of centralDir) { result.set(cd, pos); pos += cd.length; }
  result.set(new Uint8Array(eocd), pos);

  return result.buffer;
}

export function useHyperPack() {
  const [status, setStatus] = useState<HyperPackStatus>('idle');
  const [progress, setProgress] = useState<HyperPackProgress | null>(null);
  const [result, setResult] = useState<HyperPackResult | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [listEntries, setListEntries] = useState<ListEntry[] | null>(null);
  const [extractedFiles, setExtractedFiles] = useState<ExtractedFile[] | null>(null);

  const workerRef = useRef<Worker | null>(null);
  const startTimeRef = useRef<number>(0);
  const initResolveRef = useRef<(() => void) | null>(null);

  const setupWorkerHandlers = useCallback((worker: Worker) => {
    worker.onmessage = (e: MessageEvent<WorkerResponse>) => {
      const msg = e.data;

      if (msg.type === 'ready') {
        initResolveRef.current?.();
        return;
      }

      if (msg.type === 'progress') {
        const elapsedMs = Date.now() - startTimeRef.current;
        const percent = msg.percent || 0;
        const speed = elapsedMs > 0 && percent > 0 ? (percent / elapsedMs) * 1000 : 0;
        const eta = speed > 0 ? (100 - percent) / speed : 0;

        setProgress((prev) => ({
          percent,
          currentBlock: prev?.currentBlock ?? 0,
          totalBlocks: prev?.totalBlocks ?? 1,
          strategy: msg.detail || prev?.strategy || '',
          speed,
          eta,
          detail: msg.detail,
        }));
      } else if (msg.type === 'done') {
        setStatus('complete');
        setProgress(null);

        const blocks: Array<{ strategy: string; inputSize: number; outputSize: number }> = [];
        if (msg.strategies) {
          for (const [strategy, count] of Object.entries(msg.strategies)) {
            for (let i = 0; i < (count as number); i++) {
              blocks.push({ strategy, inputSize: 0, outputSize: 0 });
            }
          }
        }

        // Store extracted files if this was a multi-file archive
        if (msg.extractedFiles && msg.extractedFiles.length > 0) {
          setExtractedFiles(msg.extractedFiles);
        } else {
          setExtractedFiles(null);
        }

        setResult({
          inputSize: msg.originalSize,
          outputSize: msg.compressedSize,
          ratio: msg.ratio,
          totalMs: msg.elapsed * 1000,
          blocks,
          outputBuffer: msg.data,
          outputFileName: msg.name,
          fileCount: msg.fileCount,
          dedupCount: msg.dedupCount,
          dedupSaved: msg.dedupSaved,
        });
      } else if (msg.type === 'list-result') {
        setListEntries(msg.entries);
      } else if (msg.type === 'error') {
        setStatus('error');
        setError(msg.message);
        setProgress(null);
      }
    };

    worker.onerror = (e) => {
      setStatus('error');
      setError(e.message || 'Worker crashed unexpectedly');
      setProgress(null);
    };
  }, []);

  const ensureWorkerReady = useCallback(async () => {
    if (!workerRef.current) {
      workerRef.current = createWorker();
      setupWorkerHandlers(workerRef.current);
    }
    return new Promise<void>((resolve) => {
      initResolveRef.current = resolve;
      workerRef.current!.postMessage({ type: 'init' });
    });
  }, [setupWorkerHandlers]);

  useEffect(() => {
    workerRef.current = createWorker();
    setupWorkerHandlers(workerRef.current);

    return () => {
      workerRef.current?.terminate();
    };
  }, [setupWorkerHandlers]);

  const compress = useCallback(async (files: FileEntry[], params: CompressParams) => {
    setStatus('processing');
    setError(null);
    setProgress({ percent: 0, currentBlock: 0, totalBlocks: 1, strategy: 'Initializing...', speed: 0, eta: 0 });
    setResult(null);
    setExtractedFiles(null);
    startTimeRef.current = Date.now();

    if (native.isNative()) {
      try {
        const isArchive = params.archiveMode || files.length > 1;
        const inputPath = files[0]?.path ?? '';
        const outputPath = inputPath
          ? native.defaultCompressOutput(inputPath)
          : '';

        const stratArgs = computeStrategyArgs(params);
        let res: native.NativeCompressResult;
        if (isArchive) {
          const paths = files.map((f) => f.path ?? f.name).filter(Boolean);
          res = await native.archiveCompress(paths, outputPath, params.blockSizeMB, params.nthreads, stratArgs.forceStrategy, stratArgs.allowedMask);
        } else {
          res = await native.compress(inputPath, outputPath, params.blockSizeMB, params.nthreads, stratArgs.forceStrategy, stratArgs.allowedMask);
        }

        setStatus('complete');
        setProgress(null);
        setResult({
          inputSize: res.inputBytes,
          outputSize: res.outputBytes,
          ratio: res.ratio,
          totalMs: res.elapsedMs,
          blocks: [],
          outputBuffer: new ArrayBuffer(0),
          outputFileName: outputPath.split(/[\\/]/).pop() ?? 'output.hpk',
          outputPath: res.outputPath,
        });
      } catch (err) {
        setStatus('error');
        setError(String(err));
        setProgress(null);
      }
      return;
    }

    await ensureWorkerReady();

    let outputName: string;
    if (params.archiveMode || files.length > 1) {
      outputName = 'archive.hpk';
    } else if (files.length === 1) {
      outputName = files[0].name + '.hpk';
    } else {
      outputName = 'output.hpk';
    }

    workerRef.current?.postMessage({ type: 'compress', files, params, outputName });
  }, [ensureWorkerReady]);

  const decompress = useCallback(async (file: File) => {
    setStatus('processing');
    setError(null);
    setProgress({ percent: 0, currentBlock: 0, totalBlocks: 1, strategy: 'Initializing...', speed: 0, eta: 0 });
    setResult(null);
    setExtractedFiles(null);
    startTimeRef.current = Date.now();

    if (native.isNative()) {
      try {
        const inputPath = (file as File & { nativePath?: string }).nativePath ?? '';
        const fmt = await native.detectFormat(inputPath);
        let res: native.NativeDecompressResult;

        if (fmt === 6) {
          const outputDir = native.defaultDecompressDir(inputPath);
          res = await native.archiveDecompress(inputPath, outputDir);
        } else {
          const outputPath = native.defaultDecompressOutput(inputPath);
          res = await native.decompress(inputPath, outputPath);
        }

        setStatus('complete');
        setProgress(null);
        setResult({
          inputSize: res.inputBytes,
          outputSize: res.outputBytes,
          ratio: res.inputBytes / Math.max(res.outputBytes, 1),
          totalMs: res.elapsedMs,
          blocks: [],
          outputBuffer: new ArrayBuffer(0),
          outputFileName: res.outputPath.split(/[\\/]/).pop() ?? 'output',
          outputPath: res.outputPath,
        });
      } catch (err) {
        setStatus('error');
        setError(String(err));
        setProgress(null);
      }
      return;
    }

    await ensureWorkerReady();
    const fileBuffer = await file.arrayBuffer();
    workerRef.current?.postMessage({ type: 'decompress', file: fileBuffer, name: file.name });
  }, [ensureWorkerReady]);

  const listArchive = useCallback(async (file: File) => {
    setListEntries(null);
    await ensureWorkerReady();
    const fileBuffer = await file.arrayBuffer();
    workerRef.current?.postMessage({ type: 'list', file: fileBuffer });
  }, [ensureWorkerReady]);

  const cancel = useCallback(() => {
    if (workerRef.current) {
      workerRef.current.terminate();
      workerRef.current = createWorker();
      setupWorkerHandlers(workerRef.current);

      setStatus('idle');
      setProgress(null);
    }
  }, [setupWorkerHandlers]);

  const reset = useCallback(() => {
    setStatus('idle');
    setProgress(null);
    setResult(null);
    setError(null);
    setListEntries(null);
    setExtractedFiles(null);
  }, []);

  /* ── Download: single file (compress result or single-file decompression) ── */
  const download = useCallback(() => {
    if (result && result.outputBuffer && result.outputBuffer.byteLength > 0) {
      const blob = new Blob([result.outputBuffer], { type: 'application/octet-stream' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = result.outputFileName;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      setTimeout(() => URL.revokeObjectURL(url), 60_000);
    }
  }, [result]);

  /* ── Download one extracted file by index ── */
  const downloadExtractedFile = useCallback((index: number) => {
    if (!extractedFiles || index < 0 || index >= extractedFiles.length) return;
    const file = extractedFiles[index];
    const blob = new Blob([file.data], { type: 'application/octet-stream' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    // Use just the filename part (strip directory path)
    a.download = file.name.split('/').pop() || file.name;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    setTimeout(() => URL.revokeObjectURL(url), 60_000);
  }, [extractedFiles]);

  /* ── Extract all to a folder (File System Access API) ── */
  const extractAllToFolder = useCallback(async (): Promise<boolean> => {
    if (!extractedFiles || extractedFiles.length === 0) return false;

    // Check if File System Access API is available
    if ('showDirectoryPicker' in window) {
      try {
        const dirHandle = await (window as any).showDirectoryPicker({ mode: 'readwrite' });

        for (const file of extractedFiles) {
          // Create subdirectories as needed
          const parts = file.name.split('/');
          let currentDir = dirHandle;

          for (let i = 0; i < parts.length - 1; i++) {
            currentDir = await currentDir.getDirectoryHandle(parts[i], { create: true });
          }

          const fileName = parts[parts.length - 1];
          const fileHandle = await currentDir.getFileHandle(fileName, { create: true });
          const writable = await fileHandle.createWritable();
          await writable.write(file.data);
          await writable.close();
        }

        return true;
      } catch (err: any) {
        if (err.name === 'AbortError') return false; // User cancelled
        throw err;
      }
    }

    return false; // API not available
  }, [extractedFiles]);

  /* ── Download all as ZIP (fallback for mobile / unsupported browsers) ── */
  const downloadAllAsZip = useCallback(() => {
    if (!extractedFiles || extractedFiles.length === 0) return;

    const zipBuffer = buildZip(extractedFiles);
    const blob = new Blob([zipBuffer], { type: 'application/zip' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    const baseName = result?.outputFileName || 'archive';
    a.download = baseName + '.zip';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    setTimeout(() => URL.revokeObjectURL(url), 60_000);
  }, [extractedFiles, result]);

  return {
    status, progress, result, error, listEntries, extractedFiles,
    compress, decompress, listArchive, cancel, reset, download,
    downloadExtractedFile, extractAllToFolder, downloadAllAsZip
  };
}
