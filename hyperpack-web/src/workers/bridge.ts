/* HyperPack Web Worker bridge types */

export interface CompressParams {
  blockSizeMB: number;
  archiveMode: boolean; // true = HPK6 multi-file, false = HPK5 single file
}

export interface FileEntry {
  name: string;        // relative path (e.g. "mydir/subdir/file.txt")
  data: ArrayBuffer;   // file content
  size: number;        // original size
}

export type WorkerRequest =
  | { type: 'init' }
  | { type: 'compress'; files: FileEntry[]; params: CompressParams; outputName: string }
  | { type: 'decompress'; file: ArrayBuffer; name: string }
  | { type: 'list'; file: ArrayBuffer }
  | { type: 'cancel' };

export type WorkerResponse =
  | { type: 'ready' }
  | { type: 'progress'; percent: number; speed?: string; eta?: string; detail?: string }
  | { type: 'done'; data: ArrayBuffer; name: string; originalSize: number; compressedSize: number;
      ratio: number; elapsed: number; strategies?: Record<string, number>;
      fileCount?: number; dedupCount?: number; dedupSaved?: number }
  | { type: 'list-result'; entries: Array<{type: string; path: string; size: number; crc: string; blocks: number; isDedup: boolean}> }
  | { type: 'error'; message: string };
