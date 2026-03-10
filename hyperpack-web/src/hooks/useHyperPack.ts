import { useState, useEffect, useRef, useCallback } from 'react';
import { CompressParams, FileEntry, WorkerResponse } from '../workers/bridge';
import * as native from '../lib/native';

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
  outputPath?: string;   // native mode: absolute path to output file
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

export function useHyperPack() {
  const [status, setStatus] = useState<HyperPackStatus>('idle');
  const [progress, setProgress] = useState<HyperPackProgress | null>(null);
  const [result, setResult] = useState<HyperPackResult | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [listEntries, setListEntries] = useState<ListEntry[] | null>(null);
  
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
        // Estimate speed/ETA from percent and elapsed time
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

        // Build blocks array from strategies map
        const blocks: Array<{ strategy: string; inputSize: number; outputSize: number }> = [];
        if (msg.strategies) {
          for (const [strategy, count] of Object.entries(msg.strategies)) {
            for (let i = 0; i < (count as number); i++) {
              blocks.push({ strategy, inputSize: 0, outputSize: 0 });
            }
          }
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
    // Send init and wait for ready
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
    startTimeRef.current = Date.now();

    // ── Native (Tauri) path ──────────────────────────────────────────────
    if (native.isNative()) {
      try {
        const isArchive = params.archiveMode || files.length > 1;
        const inputPath = files[0]?.path ?? '';
        const outputPath = inputPath
          ? native.defaultCompressOutput(inputPath)
          : '';

        let res: native.NativeCompressResult;
        if (isArchive) {
          const paths = files.map((f) => f.path ?? f.name).filter(Boolean);
          res = await native.archiveCompress(paths, outputPath, params.blockSizeMB, params.nthreads);
        } else {
          res = await native.compress(inputPath, outputPath, params.blockSizeMB, params.nthreads);
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

    // ── WASM (browser) path ──────────────────────────────────────────────
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
    startTimeRef.current = Date.now();

    // ── Native (Tauri) path ──────────────────────────────────────────────
    if (native.isNative()) {
      try {
        // file.path is set by App.tsx in native mode
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

    // ── WASM (browser) path ──────────────────────────────────────────────
    await ensureWorkerReady();
    const fileBuffer = await file.arrayBuffer();
    workerRef.current?.postMessage({ type: 'decompress', file: fileBuffer, name: file.name });
  }, [ensureWorkerReady]);

  const listArchive = useCallback(async (file: File) => {
    setListEntries(null);

    await ensureWorkerReady();

    const fileBuffer = await file.arrayBuffer();
    workerRef.current?.postMessage({
      type: 'list',
      file: fileBuffer,
    });
  }, [ensureWorkerReady]);

  const cancel = useCallback(() => {
    if (workerRef.current) {
      // Terminate the blocked worker and create a fresh one
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
  }, []);

  const download = useCallback(() => {
    if (result && result.outputBuffer) {
      const blob = new Blob([result.outputBuffer]);
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = result.outputFileName;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      URL.revokeObjectURL(url);
    }
  }, [result]);

  return { status, progress, result, error, listEntries, compress, decompress, listArchive, cancel, reset, download };
}
