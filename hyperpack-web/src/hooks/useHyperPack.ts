import { useState, useEffect, useRef, useCallback } from 'react';
import { CompressParams, WorkerRequest, WorkerResponse } from '../workers/bridge';

export type HyperPackStatus = 'idle' | 'processing' | 'complete' | 'error';

export type HyperPackProgress = {
  percent: number;
  currentBlock: number;
  totalBlocks: number;
  strategy: string;
  speed: number;
  eta: number;
  currentRatio?: number;
};

export type HyperPackResult = {
  inputSize: number;
  outputSize: number;
  ratio: number;
  totalMs: number;
  blocks: Array<{ strategy: string; inputSize: number; outputSize: number }>;
  outputBuffer: ArrayBuffer;
  outputFileName: string;
};

function createWorker(): Worker {
  return new Worker('/worker.js');
}

export function useHyperPack() {
  const [status, setStatus] = useState<HyperPackStatus>('idle');
  const [progress, setProgress] = useState<HyperPackProgress | null>(null);
  const [result, setResult] = useState<HyperPackResult | null>(null);
  const [error, setError] = useState<string | null>(null);
  
  const workerRef = useRef<Worker | null>(null);
  const currentJobIdRef = useRef<string | null>(null);

  const setupWorkerHandlers = useCallback((worker: Worker) => {
    worker.onmessage = (e: MessageEvent<WorkerResponse>) => {
      const msg = e.data;
      if (msg.id !== currentJobIdRef.current) return;

      if (msg.type === 'progress') {
        const speed = msg.elapsedMs > 0 ? (msg.bytesProcessed / msg.elapsedMs) * 1000 : 0;
        const estimatedTotal = msg.percent > 0 ? (msg.bytesProcessed / msg.percent) * 100 : 0;
        const remainingBytes = estimatedTotal - msg.bytesProcessed;
        const eta = speed > 0 ? remainingBytes / speed : 0;

        setProgress({
          percent: msg.percent,
          currentBlock: msg.currentBlock,
          totalBlocks: msg.totalBlocks,
          strategy: msg.strategy,
          speed,
          eta,
          currentRatio: msg.currentRatio,
        });
      } else if (msg.type === 'complete') {
        setStatus('complete');
        setProgress(null);
        setResult({
          inputSize: msg.stats.inputSize,
          outputSize: msg.stats.outputSize,
          ratio: msg.stats.ratio,
          totalMs: msg.stats.totalMs,
          blocks: msg.stats.blocks,
          outputBuffer: msg.outputBuffer,
          outputFileName: msg.outputFileName,
        });
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

  useEffect(() => {
    workerRef.current = createWorker();
    setupWorkerHandlers(workerRef.current);

    return () => {
      workerRef.current?.terminate();
    };
  }, [setupWorkerHandlers]);

  const compress = useCallback(async (file: File, params: CompressParams) => {
    setStatus('processing');
    setError(null);
    setProgress({ percent: 0, currentBlock: 0, totalBlocks: 1, strategy: 'Initializing WASM...', speed: 0, eta: 0 });
    setResult(null);
    
    const id = Math.random().toString(36).substring(7);
    currentJobIdRef.current = id;
    
    const fileBuffer = await file.arrayBuffer();
    
    const req: WorkerRequest = {
      type: 'compress',
      id,
      fileBuffer,
      fileName: file.name,
      params,
    };
    
    workerRef.current?.postMessage(req, [fileBuffer]);
  }, []);

  const decompress = useCallback(async (file: File) => {
    setStatus('processing');
    setError(null);
    setProgress({ percent: 0, currentBlock: 0, totalBlocks: 1, strategy: 'Initializing WASM...', speed: 0, eta: 0 });
    setResult(null);
    
    const id = Math.random().toString(36).substring(7);
    currentJobIdRef.current = id;
    
    const fileBuffer = await file.arrayBuffer();
    
    const req: WorkerRequest = {
      type: 'decompress',
      id,
      fileBuffer,
      fileName: file.name,
    };
    
    workerRef.current?.postMessage(req, [fileBuffer]);
  }, []);

  const cancel = useCallback(() => {
    if (currentJobIdRef.current && workerRef.current) {
      // Terminate the blocked worker and create a fresh one
      workerRef.current.terminate();
      workerRef.current = createWorker();
      setupWorkerHandlers(workerRef.current);
      
      currentJobIdRef.current = null;
      setStatus('idle');
      setProgress(null);
    }
  }, [setupWorkerHandlers]);

  const reset = useCallback(() => {
    setStatus('idle');
    setProgress(null);
    setResult(null);
    setError(null);
    currentJobIdRef.current = null;
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

  return { status, progress, result, error, compress, decompress, cancel, reset, download };
}
