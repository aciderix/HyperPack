/**
 * HyperPack WASM Web Worker
 *
 * Loads the Emscripten-compiled WASM module and bridges
 * postMessage requests to the real compression engine.
 *
 * Progress is reported by parsing the C engine's stderr output.
 */

let Module = null;
let currentJobId = null;
let startTime = 0;
let totalBlocks = 1;
let completedBlocks = [];
let currentBlockNum = 0;

/**
 * Parse stderr lines from the C engine for progress reporting.
 */
function handleStderr(text) {
  if (!currentJobId) return;

  // Parse header: "[HP5] Compressing ... (X MB, N blocks of M MB)"
  const headerMatch = text.match(/\[HP5\] Compressing .+ \([\d.]+ MB, (\d+) blocks? of/);
  if (headerMatch) {
    totalBlocks = parseInt(headerMatch[1]);
    return;
  }

  // Parse decompression header
  const decHeaderMatch = text.match(/\[HP5\] Decompressing \([\d.]+ MB, (\d+) blocks?\)/);
  if (decHeaderMatch) {
    totalBlocks = parseInt(decHeaderMatch[1]);
    return;
  }

  // Parse block completion: "  Block X/Y: IN -> OUT (Rx) [STRAT]"
  const blockMatch = text.match(/Block (\d+)\/(\d+): (\d+) -> (\d+) \(([\d.]+)x\) \[(.+)\]/);
  if (blockMatch) {
    const [, current, total, inSize, outSize, ratio, strategy] = blockMatch;
    currentBlockNum = parseInt(current);
    totalBlocks = parseInt(total);
    const bytesIn = parseInt(inSize);
    const bytesOut = parseInt(outSize);

    completedBlocks.push({ strategy, inputSize: bytesIn, outputSize: bytesOut });

    const percent = Math.min(99, Math.floor((currentBlockNum / totalBlocks) * 100));
    const totalBytesProcessed = completedBlocks.reduce((sum, b) => sum + b.inputSize, 0);

    self.postMessage({
      type: 'progress',
      id: currentJobId,
      percent,
      currentBlock: currentBlockNum,
      totalBlocks,
      strategy,
      bytesProcessed: totalBytesProcessed,
      elapsedMs: Date.now() - startTime,
      currentRatio: parseFloat(ratio),
    });
    return;
  }

  // Parse decompression block: "  Block X/Y: IN -> OUT [STRAT] CRC OK"
  const decBlockMatch = text.match(/Block (\d+)\/(\d+): (\d+) -> (\d+) \[(.+)\] CRC OK/);
  if (decBlockMatch) {
    const [, current, total, inSize, outSize, strategy] = decBlockMatch;
    currentBlockNum = parseInt(current);
    totalBlocks = parseInt(total);
    const bytesIn = parseInt(inSize);
    const bytesOut = parseInt(outSize);

    completedBlocks.push({ strategy, inputSize: bytesIn, outputSize: bytesOut });

    const percent = Math.min(99, Math.floor((currentBlockNum / totalBlocks) * 100));
    const totalBytesProcessed = completedBlocks.reduce((sum, b) => sum + b.inputSize, 0);

    self.postMessage({
      type: 'progress',
      id: currentJobId,
      percent,
      currentBlock: currentBlockNum,
      totalBlocks,
      strategy,
      bytesProcessed: totalBytesProcessed,
      elapsedMs: Date.now() - startTime,
    });
    return;
  }

  // Parse DUP block: "  Block X/Y: DUP of Z"
  const dupMatch = text.match(/Block (\d+)\/(\d+): DUP of (\d+)/);
  if (dupMatch) {
    const [, current, total] = dupMatch;
    currentBlockNum = parseInt(current);
    totalBlocks = parseInt(total);

    completedBlocks.push({ strategy: 'DUP', inputSize: 0, outputSize: 0 });

    const percent = Math.min(99, Math.floor((currentBlockNum / totalBlocks) * 100));

    self.postMessage({
      type: 'progress',
      id: currentJobId,
      percent,
      currentBlock: currentBlockNum,
      totalBlocks,
      strategy: 'DUP',
      bytesProcessed: 0,
      elapsedMs: Date.now() - startTime,
    });
    return;
  }

  // Parse strategy testing progress (within a block)
  // These show which strategy is being tested
  const stratTestMatch = text.match(/\[(\w[\w+]*)\]/);
  if (stratTestMatch && currentBlockNum < totalBlocks) {
    const strategy = stratTestMatch[1];
    // Send a progress update showing current strategy being tested
    self.postMessage({
      type: 'progress',
      id: currentJobId,
      percent: Math.min(99, Math.floor((Math.max(0, currentBlockNum) / totalBlocks) * 100)),
      currentBlock: currentBlockNum + 1,
      totalBlocks,
      strategy: 'Testing ' + strategy + '...',
      bytesProcessed: completedBlocks.reduce((sum, b) => sum + b.inputSize, 0),
      elapsedMs: Date.now() - startTime,
    });
  }
}

/**
 * Initialize the WASM module.
 */
async function initModule() {
  try {
    importScripts('hyperpack.js');
  } catch (e) {
    throw new Error(
      'WASM module not found. Run build-wasm.sh to compile the HyperPack engine. ' +
      '(Looking for hyperpack.js + hyperpack.wasm in public/)'
    );
  }

  Module = await createHyperPack({
    print: () => {},       // ignore stdout
    printErr: (text) => {  // capture stderr for progress
      handleStderr(text);
    },
  });
}

/**
 * Reset state for a new job.
 */
function resetJobState(id) {
  currentJobId = id;
  startTime = Date.now();
  totalBlocks = 1;
  completedBlocks = [];
  currentBlockNum = 0;
}

/**
 * Handle incoming messages from the main thread.
 */
self.onmessage = async (e) => {
  const msg = e.data;

  // Cancel: main thread should terminate this worker for immediate cancel
  if (msg.type === 'cancel') {
    currentJobId = null;
    return;
  }

  try {
    if (!Module) {
      await initModule();
    }

    if (msg.type === 'compress') {
      resetJobState(msg.id);

      // Send initial progress
      self.postMessage({
        type: 'progress',
        id: msg.id,
        percent: 0,
        currentBlock: 0,
        totalBlocks: 1,
        strategy: 'Loading file...',
        bytesProcessed: 0,
        elapsedMs: 0,
      });

      // Write input file to MEMFS
      const inputData = new Uint8Array(msg.fileBuffer);
      Module.FS.writeFile('/input', inputData);

      // Run compression
      const blockMB = msg.params.blockSizeMB || 8;
      const result = Module._hp_compress(blockMB);

      if (currentJobId !== msg.id) return; // cancelled

      if (result === 0) {
        // Read output
        const outputData = Module.FS.readFile('/output.hpk');
        const outputBuffer = outputData.buffer.slice(
          outputData.byteOffset,
          outputData.byteOffset + outputData.byteLength
        );

        const inputSize = inputData.length;
        const outputSize = outputData.length;

        self.postMessage({
          type: 'complete',
          id: msg.id,
          outputBuffer,
          outputFileName: msg.fileName + '.hpk',
          stats: {
            inputSize,
            outputSize,
            ratio: outputSize > 0 ? inputSize / outputSize : 1,
            totalMs: Date.now() - startTime,
            blocks: completedBlocks.filter(b => b.strategy !== 'DUP'),
          },
        }, [outputBuffer]);
      } else {
        self.postMessage({
          type: 'error',
          id: msg.id,
          message: 'Compression failed (engine returned code ' + result + ')',
        });
      }

      // Cleanup MEMFS
      try { Module.FS.unlink('/input'); } catch (_) {}
      try { Module.FS.unlink('/output.hpk'); } catch (_) {}

    } else if (msg.type === 'decompress') {
      resetJobState(msg.id);

      self.postMessage({
        type: 'progress',
        id: msg.id,
        percent: 0,
        currentBlock: 0,
        totalBlocks: 1,
        strategy: 'Reading header...',
        bytesProcessed: 0,
        elapsedMs: 0,
      });

      // Write input file to MEMFS
      const inputData = new Uint8Array(msg.fileBuffer);
      Module.FS.writeFile('/input.hpk', inputData);

      // Run decompression
      const result = Module._hp_decompress();

      if (currentJobId !== msg.id) return; // cancelled

      if (result === 0) {
        const outputData = Module.FS.readFile('/output');
        const outputBuffer = outputData.buffer.slice(
          outputData.byteOffset,
          outputData.byteOffset + outputData.byteLength
        );

        const inputSize = inputData.length;
        const outputSize = outputData.length;

        // Remove .hpk extension for output filename
        let outputFileName = msg.fileName;
        if (outputFileName.endsWith('.hpk')) {
          outputFileName = outputFileName.slice(0, -4);
        } else {
          outputFileName = 'decompressed_' + outputFileName;
        }

        self.postMessage({
          type: 'complete',
          id: msg.id,
          outputBuffer,
          outputFileName,
          stats: {
            inputSize,
            outputSize,
            ratio: inputSize > 0 ? outputSize / inputSize : 1,
            totalMs: Date.now() - startTime,
            blocks: completedBlocks,
          },
        }, [outputBuffer]);
      } else {
        self.postMessage({
          type: 'error',
          id: msg.id,
          message: 'Decompression failed — file may be corrupted or not a valid .hpk file (code ' + result + ')',
        });
      }

      // Cleanup MEMFS
      try { Module.FS.unlink('/input.hpk'); } catch (_) {}
      try { Module.FS.unlink('/output'); } catch (_) {}
    }
  } catch (err) {
    self.postMessage({
      type: 'error',
      id: msg.id,
      message: err.message || 'Unknown worker error',
    });
  }
};
