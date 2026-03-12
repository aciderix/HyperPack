/* HyperPack Web Worker bridge types */

export type StrategyMode = 'auto' | 'force' | 'include' | 'exclude';

export interface CompressParams {
  blockSizeMB: number;
  archiveMode: boolean;
  nthreads: number;
  /** Strategy selection mode: auto, force, include, exclude */
  strategyMode: StrategyMode;
  /** When mode = 'force': the strategy index (0-30) */
  forceStrategy: number;
  /** When mode = 'include' or 'exclude': set of strategy indices */
  strategySet: number[];
}

export interface FileEntry {
  name: string;
  data: ArrayBuffer;
  size: number;
  path?: string;
  isDirectory?: boolean;
}

export interface ExtractedFile {
  name: string;
  size: number;
  data: ArrayBuffer;
}

export type WorkerRequest =
  | { type: 'init' }
  | { type: 'compress'; files: FileEntry[]; params: CompressParams; outputName: string }
  | { type: 'decompress'; file: ArrayBuffer; name: string }
  | { type: 'list'; file: ArrayBuffer }
  | { type: 'cancel' };

export type WorkerResponse =
  | { type: 'ready' }
  | { type: 'progress'; percent: number; speed?: string; eta?: string; detail?: string;
      phase?: string; currentStrategy?: string; bestStrategy?: string; bestRatio?: number;
      totalBlocks?: number; currentBlock?: number; totalBytes?: number; bytesProcessed?: number;
      blockInputSize?: number; blockOutputSize?: number;
      testedStrategies?: Array<{ name: string; ratio: number; size: number }>;
      currentFile?: string }
  | { type: 'done'; data: ArrayBuffer; name: string; originalSize: number; compressedSize: number;
      ratio: number; elapsed: number; strategies?: Record<string, number>;
      fileCount?: number; dedupCount?: number; dedupSaved?: number;
      extractedFiles?: ExtractedFile[] }
  | { type: 'list-result'; entries: Array<{type: string; path: string; size: number; crc: string; blocks: number; isDedup: boolean}> }
  | { type: 'error'; message: string };

/**
 * Compute the force_strategy and allowed_mask values from CompressParams
 * for passing to the C/WASM API.
 */
export function computeStrategyArgs(params: CompressParams): { forceStrategy: number; allowedMask: number } {
  switch (params.strategyMode) {
    case 'force':
      return { forceStrategy: params.forceStrategy, allowedMask: 0xFFFFFFFF };
    case 'include': {
      let mask = 0;
      for (const s of params.strategySet) mask |= (1 << s);
      return { forceStrategy: -1, allowedMask: mask || 0xFFFFFFFF };
    }
    case 'exclude': {
      let mask = 0xFFFFFFFF;
      for (const s of params.strategySet) mask &= ~(1 << s);
      return { forceStrategy: -1, allowedMask: mask };
    }
    default: // 'auto'
      return { forceStrategy: -1, allowedMask: 0xFFFFFFFF };
  }
}
