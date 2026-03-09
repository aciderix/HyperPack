export type CompressParams = {
  blockSizeMB: number;  // 1-64 MB (default 8)
};

export type WorkerRequest =
  | {
      type: 'compress';
      id: string;
      fileBuffer: ArrayBuffer;
      fileName: string;
      params: CompressParams;
    }
  | {
      type: 'decompress';
      id: string;
      fileBuffer: ArrayBuffer;
      fileName: string;
    }
  | {
      type: 'cancel';
      id: string;
    };

export type WorkerResponse =
  | {
      type: 'progress';
      id: string;
      percent: number;
      currentBlock: number;
      totalBlocks: number;
      strategy: string;
      bytesProcessed: number;
      elapsedMs: number;
      currentRatio?: number;
    }
  | {
      type: 'complete';
      id: string;
      outputBuffer: ArrayBuffer;
      outputFileName: string;
      stats: {
        inputSize: number;
        outputSize: number;
        ratio: number;
        totalMs: number;
        blocks: Array<{
          strategy: string;
          inputSize: number;
          outputSize: number;
        }>;
      };
    }
  | {
      type: 'error';
      id: string;
      message: string;
    };
