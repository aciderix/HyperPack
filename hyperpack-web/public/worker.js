/* HyperPack Web Worker — WASM bridge (HPK5 + HPK6) */
let Module = null;

self.onmessage = async function (e) {
  const msg = e.data;

  if (msg.type === 'init') {
    try {
      importScripts('hyperpack.js');
      Module = await createHyperPack({
        print: (text) => { /* stdout — ignore */ },
        printErr: (text) => { parseProgress(text); }
      });
      self.postMessage({ type: 'ready' });
    } catch (err) {
      self.postMessage({ type: 'error', message: 'Failed to load WASM: ' + err.message });
    }
    return;
  }

  if (!Module) {
    self.postMessage({ type: 'error', message: 'WASM not initialized' });
    return;
  }

  if (msg.type === 'compress') {
    try {
      const { files, params, outputName } = msg;
      const outName = outputName || 'output.hpk';

      if (params.archiveMode && files.length > 0) {
        compressArchive(files, params, outName);
      } else if (files.length === 1) {
        compressSingle(files[0], params, outName);
      } else {
        self.postMessage({ type: 'error', message: 'No files provided' });
      }
    } catch (err) {
      self.postMessage({ type: 'error', message: 'Compress error: ' + err.message });
    }
    return;
  }

  if (msg.type === 'decompress') {
    try {
      decompressFile(msg.file, msg.name);
    } catch (err) {
      self.postMessage({ type: 'error', message: 'Decompress error: ' + err.message });
    }
    return;
  }

  if (msg.type === 'list') {
    try {
      listArchive(msg.file);
    } catch (err) {
      self.postMessage({ type: 'error', message: 'List error: ' + err.message });
    }
    return;
  }
};

/* ===== HPK5 single file compress ===== */
function computeStrategyArgs(params) {
  var mode = params.strategyMode || 'auto';
  if (mode === 'force') {
    return { forceStrategy: params.forceStrategy || 0, allowedMask: 0xFFFFFFFF };
  } else if (mode === 'include') {
    var mask = 0;
    var set = params.strategySet || [];
    for (var i = 0; i < set.length; i++) mask |= (1 << set[i]);
    return { forceStrategy: -1, allowedMask: mask || 0xFFFFFFFF };
  } else if (mode === 'exclude') {
    var mask = 0xFFFFFFFF;
    var set = params.strategySet || [];
    for (var i = 0; i < set.length; i++) mask &= ~(1 << set[i]);
    return { forceStrategy: -1, allowedMask: mask };
  }
  return { forceStrategy: -1, allowedMask: 0xFFFFFFFF };
}

function compressSingle(file, params, outName) {
  const inPath = '/input';
  const outPath = '/output.hpk';
  cleanupMemfs([inPath, outPath]);

  const data = new Uint8Array(file.data);
  Module.FS.writeFile(inPath, data);

  /* Reset progress state and emit initial progress */
  resetProgressState();
  lastStrategies = {};
  lastDedupCount = 0;
  lastDedupSaved = 0;
  progressState.totalBytes = data.length;
  progressState.phase = 'init';
  emitProgress(0);

  var stratArgs = computeStrategyArgs(params);
  const start = performance.now();
  const ret = Module._hp_compress(params.blockSizeMB || 8, stratArgs.forceStrategy, stratArgs.allowedMask);

  if (ret !== 0) {
    self.postMessage({ type: 'error', message: 'Compression failed (code ' + ret + ')' });
    return;
  }

  const compressed = Module.FS.readFile(outPath);
  const elapsed = (performance.now() - start) / 1000;

  var outBuf = new Uint8Array(compressed).buffer;

  self.postMessage({
    type: 'done',
    data: outBuf,
    name: outName,
    originalSize: data.length,
    compressedSize: compressed.length,
    ratio: data.length / compressed.length,
    elapsed: elapsed,
    strategies: lastStrategies,
    fileCount: 1,
    dedupCount: 0,
    dedupSaved: 0
  }, [outBuf]);

  cleanupMemfs([inPath, outPath]);
}

/* ===== HPK6 archive compress ===== */
function compressArchive(files, params, outName) {
  const dirName = '/archive_input';
  const outPath = '/output.hpk';
  cleanupMemfs([outPath]);

  try { Module.FS.rmdir(dirName); } catch (e) {}
  removeRecursive(dirName);
  try { Module.FS.mkdir(dirName); } catch (e) {}

  /* Clean file names (Android SAF URIs, URL encoding, common prefix) */
  var cleanedFiles = cleanFileNames(files);

  let totalSize = 0;
  const dirs = new Set();
  dirs.add(dirName);

  for (const file of cleanedFiles) {
    const fullPath = dirName + '/' + file.name;
    const parts = fullPath.split('/');
    for (let i = 2; i < parts.length; i++) {
      const dir = parts.slice(0, i).join('/');
      if (!dirs.has(dir)) {
        try { Module.FS.mkdir(dir); } catch (e) {}
        dirs.add(dir);
      }
    }
    const data = new Uint8Array(file.data);
    Module.FS.writeFile(fullPath, data);
    totalSize += data.length;
  }

  /* Reset progress state and emit initial progress */
  resetProgressState();
  lastStrategies = {};
  lastDedupCount = 0;
  lastDedupSaved = 0;
  progressState.totalBytes = totalSize;
  progressState.phase = 'init';
  emitProgress(0);

  var stratArgs = computeStrategyArgs(params);
  const start = performance.now();
  const dirPtr = Module.stringToNewUTF8(dirName);
  const outPtr = Module.stringToNewUTF8(outPath);
  const ret = Module._hp_archive_compress(dirPtr, outPtr, params.blockSizeMB || 8, stratArgs.forceStrategy, stratArgs.allowedMask);
  Module._free(dirPtr);
  Module._free(outPtr);

  if (ret !== 0) {
    self.postMessage({ type: 'error', message: 'Archive compression failed (code ' + ret + ')' });
    return;
  }

  const compressed = Module.FS.readFile(outPath);
  const elapsed = (performance.now() - start) / 1000;

  var outBuf = new Uint8Array(compressed).buffer;

  self.postMessage({
    type: 'done',
    data: outBuf,
    name: outName,
    originalSize: totalSize,
    compressedSize: compressed.length,
    ratio: totalSize / compressed.length,
    elapsed: elapsed,
    strategies: lastStrategies,
    fileCount: files.length,
    dedupCount: lastDedupCount,
    dedupSaved: lastDedupSaved
  }, [outBuf]);

  removeRecursive(dirName);
  cleanupMemfs([outPath]);
}

/* ===== Decompress (auto-detect HPK5/HPK6) ===== */
function decompressFile(fileBuffer, fileName) {
  const inPath = '/input.hpk';
  const outPath = '/output';
  const outDir = '/output_dir';
  cleanupMemfs([inPath, outPath]);
  removeRecursive(outDir);

  const data = new Uint8Array(fileBuffer);
  Module.FS.writeFile(inPath, data);

  const fmtPtr = Module.stringToNewUTF8(inPath);
  const fmt = Module._hp_detect_format(fmtPtr);
  Module._free(fmtPtr);

  const start = performance.now();

  if (fmt === 6) {
    /* HPK6 archive */
    try { Module.FS.mkdir(outDir); } catch (e) {}
    const inPtr2 = Module.stringToNewUTF8(inPath);
    const outDirPtr = Module.stringToNewUTF8(outDir);
    const nullPtr = 0;
    const ret = Module._hp_archive_decompress(inPtr2, outDirPtr, nullPtr);
    Module._free(inPtr2);
    Module._free(outDirPtr);

    if (ret !== 0) {
      self.postMessage({ type: 'error', message: 'Archive decompression failed' });
      return;
    }

    const extractedFiles = cleanFileNames(collectFiles(outDir, ''));
    const totalSize = extractedFiles.reduce(function(sum, f) { return sum + f.size; }, 0);
    const elapsed = (performance.now() - start) / 1000;

    if (extractedFiles.length === 1) {
      /* Single file in archive — return directly */
      var outBuf6s = new Uint8Array(extractedFiles[0].data).buffer;
      self.postMessage({
        type: 'done',
        data: outBuf6s,
        name: extractedFiles[0].name,
        originalSize: data.length,
        compressedSize: totalSize,
        ratio: 1,
        elapsed: elapsed,
        fileCount: 1
      }, [outBuf6s]);
    } else {
      /* ── FIX: send ALL extracted files back to the main thread ── */
      var filesOut = [];
      var transferables = [];
      for (var i = 0; i < extractedFiles.length; i++) {
        /* Copy into a clean ArrayBuffer — FS.readFile returns a view of the
           WASM heap which cannot be transferred and may get invalidated. */
        var cleanBuf = new Uint8Array(extractedFiles[i].data).buffer;
        filesOut.push({
          name: extractedFiles[i].name,
          size: extractedFiles[i].size,
          data: cleanBuf
        });
        transferables.push(cleanBuf);
      }

      self.postMessage({
        type: 'done',
        data: new ArrayBuffer(0),
        name: fileName.replace(/\.hpk$/i, ''),
        originalSize: data.length,
        compressedSize: totalSize,
        ratio: 1,
        elapsed: elapsed,
        fileCount: extractedFiles.length,
        extractedFiles: filesOut
      }, transferables);
    }

    removeRecursive(outDir);
  } else {
    /* HPK5 single file */
    const ret = Module._hp_decompress();

    if (ret !== 0) {
      self.postMessage({ type: 'error', message: 'Decompression failed' });
      return;
    }

    const output = Module.FS.readFile(outPath);
    const elapsed = (performance.now() - start) / 1000;

    var outBuf5 = new Uint8Array(output).buffer;

    const outName = fileName.replace(/\.hpk$/i, '');
    self.postMessage({
      type: 'done',
      data: outBuf5,
      name: outName,
      originalSize: data.length,
      compressedSize: output.length,
      ratio: 1,
      elapsed: elapsed,
      fileCount: 1
    }, [outBuf5]);

    cleanupMemfs([outPath]);
  }

  cleanupMemfs([inPath]);
}

/* ===== List archive contents ===== */
function listArchive(fileBuffer) {
  const inPath = '/input.hpk';
  cleanupMemfs([inPath]);

  const data = new Uint8Array(fileBuffer);
  Module.FS.writeFile(inPath, data);

  listEntries = [];
  const inPtr = Module.stringToNewUTF8(inPath);
  const ret = Module._hp_archive_list(inPtr);
  Module._free(inPtr);

  if (ret !== 0) {
    self.postMessage({ type: 'error', message: 'List failed' });
    return;
  }

  self.postMessage({ type: 'list-result', entries: listEntries });
  cleanupMemfs([inPath]);
}

/* ===== MEMFS helpers ===== */
function cleanupMemfs(paths) {
  for (const p of paths) {
    try { Module.FS.unlink(p); } catch (e) {}
  }
}

function removeRecursive(path) {
  try {
    const stat = Module.FS.stat(path);
    if (Module.FS.isDir(stat.mode)) {
      const entries = Module.FS.readdir(path).filter(function(e) { return e !== '.' && e !== '..'; });
      for (const entry of entries) {
        removeRecursive(path + '/' + entry);
      }
      Module.FS.rmdir(path);
    } else {
      Module.FS.unlink(path);
    }
  } catch (e) {}
}

function collectFiles(basePath, rel) {
  const files = [];
  try {
    const entries = Module.FS.readdir(basePath).filter(function(e) { return e !== '.' && e !== '..'; });
    for (const entry of entries) {
      const fullPath = basePath + '/' + entry;
      const relPath = rel ? rel + '/' + entry : entry;
      const stat = Module.FS.stat(fullPath);
      if (Module.FS.isDir(stat.mode)) {
        files.push.apply(files, collectFiles(fullPath, relPath));
      } else {
        const data = Module.FS.readFile(fullPath);
        files.push({ name: relPath, data: data, size: data.length });
      }
    }
  } catch (e) {}
  return files;
}

/* ===== File name cleaning (Android SAF URIs, URL encoding) ===== */
function cleanFileName(name) {
  /* 1. URL-decode */
  try { name = decodeURIComponent(name); } catch (e) {}
  /* 2. Strip internal MEMFS prefix */
  name = name.replace(/^archive_input\//, '');
  /* 3. Android Storage Access Framework:
         tree/<treeId>/document/<storageVolume>:<path>
     After URL-decode treeId/docId may contain real slashes,
     so we look for the "/document/" marker. */
  var docIdx = name.indexOf('/document/');
  if (docIdx !== -1 && name.indexOf('tree/') === 0) {
    var docPath = name.substring(docIdx + 10); /* skip '/document/' */
    var colonIdx = docPath.indexOf(':');
    var slashIdx = docPath.indexOf('/');
    if (colonIdx !== -1 && (slashIdx === -1 || colonIdx < slashIdx)) {
      docPath = docPath.substring(colonIdx + 1);
    }
    name = docPath;
  }
  /* 4. Trim leading slashes */
  name = name.replace(/^\/+/, '');
  return name;
}

/**
 * Clean an array of {name, data, size} file objects:
 *  - decode / sanitise each name
 *  - strip the longest common *directory* prefix so the tree
 *    starts at the interesting level (e.g. drop "Download/D184MB/")
 */
function cleanFileNames(files) {
  var cleaned = [];
  for (var i = 0; i < files.length; i++) {
    cleaned.push({
      name: cleanFileName(files[i].name),
      data: files[i].data,
      size: files[i].size !== undefined
        ? files[i].size
        : (files[i].data ? (files[i].data.length || files[i].data.byteLength || 0) : 0)
    });
  }
  if (cleaned.length <= 1) return cleaned;

  /* Find common directory prefix (never strip the filename itself) */
  var allParts = cleaned.map(function (f) { return f.name.split('/'); });
  var minLen = Math.min.apply(null, allParts.map(function (p) { return p.length; }));
  var prefixLen = 0;
  for (var i = 0; i < minLen - 1; i++) {
    var part = allParts[0][i];
    var allMatch = true;
    for (var j = 1; j < allParts.length; j++) {
      if (allParts[j][i] !== part) { allMatch = false; break; }
    }
    if (allMatch) prefixLen++;
    else break;
  }
  if (prefixLen === 0) return cleaned;

  return cleaned.map(function (f) {
    var parts = f.name.split('/');
    return { name: parts.slice(prefixLen).join('/'), data: f.data, size: f.size };
  });
}

/* ===== Progress parsing from stderr ===== */
let lastStrategies = {};
let lastDedupCount = 0;
let lastDedupSaved = 0;
let listEntries = [];

/* ── Live progress state ── */
var progressState = {
  totalBlocks: 1,
  currentBlock: 0,
  totalBytes: 0,
  bytesProcessed: 0,
  phase: 'init',               /* init | scanning | analyzing | testing | block-done | done */
  currentStrategy: '',         /* strategy currently being tested */
  bestStrategy: '',            /* best strategy found so far for current block */
  bestRatio: 0,                /* best compression ratio for current block */
  blockInputSize: 0,           /* current block input size */
  blockOutputSize: 0,          /* current block best output size */
  testedStrategies: [],        /* list of {name, ratio} tested in current block */
  currentFile: ''              /* current file name (HPK6) */
};

function resetProgressState() {
  progressState.currentBlock = 0;
  progressState.bytesProcessed = 0;
  progressState.phase = 'init';
  progressState.currentStrategy = '';
  progressState.bestStrategy = '';
  progressState.bestRatio = 0;
  progressState.blockInputSize = 0;
  progressState.blockOutputSize = 0;
  progressState.testedStrategies = [];
  progressState.currentFile = '';
}

function emitProgress(percent) {
  var pct = typeof percent === 'number' ? percent : null;
  /* Auto-compute percent from blocks if not given */
  if (pct === null && progressState.totalBlocks > 0) {
    pct = Math.round((progressState.currentBlock / progressState.totalBlocks) * 100);
  }
  if (pct === null) pct = 0;
  self.postMessage({
    type: 'progress',
    percent: pct,
    detail: progressState.currentStrategy || progressState.phase,
    phase: progressState.phase,
    currentStrategy: progressState.currentStrategy,
    bestStrategy: progressState.bestStrategy,
    bestRatio: progressState.bestRatio,
    totalBlocks: progressState.totalBlocks,
    currentBlock: progressState.currentBlock,
    totalBytes: progressState.totalBytes,
    bytesProcessed: progressState.bytesProcessed,
    blockInputSize: progressState.blockInputSize,
    blockOutputSize: progressState.blockOutputSize,
    testedStrategies: progressState.testedStrategies,
    currentFile: progressState.currentFile
  });
}

function parseProgress(text) {
  /* ── Strategy tracking for final result ── */
  const stratMatch = text.match(/\[([A-Za-z0-9+_]+)\]$/);
  if (stratMatch) {
    const name = stratMatch[1];
    lastStrategies[name] = (lastStrategies[name] || 0) + 1;
  }

  /* ── HPK5 init: "[HP5] Compressing file (170.07 MB, 2 blocks of 128 MB)" ── */
  var hp5Init = text.match(/\[HP5\] Compressing .* \(([\d.]+) MB, (\d+) blocks? of (\d+) MB\)/);
  if (hp5Init) {
    progressState.totalBytes = Math.round(parseFloat(hp5Init[1]) * 1048576);
    progressState.totalBlocks = parseInt(hp5Init[2]);
    progressState.phase = 'analyzing';
    emitProgress(0);
    return;
  }

  /* ── HPK6 scan: "[HPK6] Scanned N entries" ── */
  if (text.match(/\[HPK6\] Scanned (\d+) entries/)) {
    progressState.phase = 'scanning';
    emitProgress(0);
    return;
  }

  /* ── HPK6 init: "[HPK6] N files, M dirs, X.XX MB total, B blocks" ── */
  var hpk6Init = text.match(/\[HPK6\] (\d+) files, (\d+) dirs, ([\d.]+) MB total, (\d+) blocks/);
  if (hpk6Init) {
    progressState.totalBytes = Math.round(parseFloat(hpk6Init[3]) * 1048576);
    progressState.totalBlocks = parseInt(hpk6Init[4]);
    progressState.phase = 'analyzing';
    emitProgress(0);
    return;
  }

  /* ── PNG detection ── */
  if (text.match(/\[HP5\] PNG detected/)) {
    progressState.phase = 'analyzing';
    progressState.currentStrategy = 'PNG pre-transform';
    emitProgress(0);
    return;
  }

  /* ── Entropy analysis: "[Entropy 5.432 ...]" ── */
  if (text.match(/\[Entropy/)) {
    progressState.phase = 'analyzing';
    progressState.currentStrategy = 'Entropy analysis';
    emitProgress();
    return;
  }

  /* ── Strategy testing messages (intra-block): "[BWT+O0]", "[LZMA]", "[Audio]" etc. ── */
  /* These are emitted DURING block compression, showing which strategy is being tried */

  /* Strategy result: "[LZMA] 8388608 -> 2345678 (3.58x) *** NEW BEST ***" */
  var stratResult = text.match(/\[([A-Za-z0-9+_]+)\]\s+(\d+)\s*->\s*(\d+)\s*\(([\d.]+)x\)(.*)/);
  if (stratResult) {
    var sName = stratResult[1];
    var sIn = parseInt(stratResult[2]);
    var sOut = parseInt(stratResult[3]);
    var sRatio = parseFloat(stratResult[4]);
    var isNewBest = text.indexOf('NEW BEST') !== -1;

    progressState.phase = 'testing';
    progressState.currentStrategy = sName;
    progressState.blockInputSize = sIn;

    /* Track tested strategy */
    progressState.testedStrategies.push({ name: sName, ratio: sRatio, size: sOut });

    if (isNewBest || sRatio > progressState.bestRatio) {
      progressState.bestRatio = sRatio;
      progressState.bestStrategy = sName;
      progressState.blockOutputSize = sOut;
    }
    emitProgress();
    return;
  }

  /* Sub-stream strategy results: "[Sub PPM] N -> M (Xx)" */
  var subResult = text.match(/\[Sub ([A-Za-z0-9+_]+)\]\s+(\d+)\s*->\s*(\d+)\s*\(([\d.]+)x/);
  if (subResult) {
    progressState.phase = 'testing';
    progressState.currentStrategy = subResult[1];
    emitProgress();
    return;
  }

  /* Strategy skipped: "[Audio] not detected, skipping" */
  var skipMatch = text.match(/\[([A-Za-z0-9+_]+)\]\s+(not detected|skipped)/);
  if (skipMatch) {
    progressState.phase = 'testing';
    progressState.currentStrategy = skipMatch[1] + ' (skip)';
    emitProgress();
    return;
  }

  /* Strategy being forced: "[forced BWT+O1] ..." */
  var forcedMatch = text.match(/\[forced ([A-Za-z0-9+_]+)\]/);
  if (forcedMatch) {
    progressState.phase = 'testing';
    progressState.currentStrategy = forcedMatch[1];
    emitProgress();
    return;
  }

  /* LZMA heuristic: "[LZMA] skipped by heuristic..." or "[LZMA] forced for small block..." */
  var lzmaHeur = text.match(/\[LZMA\]\s+(skipped|forced)/);
  if (lzmaHeur) {
    progressState.phase = 'testing';
    progressState.currentStrategy = 'LZMA (' + lzmaHeur[1] + ')';
    emitProgress();
    return;
  }

  /* LZMA early-exit: "[LZMA] early-exit at X%..." */
  if (text.match(/\[LZMA\] early-exit/)) {
    progressState.phase = 'testing';
    progressState.currentStrategy = 'LZMA (early-exit)';
    emitProgress();
    return;
  }

  /* ── Block completion ── */
  /* HP5: "  Block 1/2: 134217728 -> 41234567 (3.25x) [LZP+BWT+O1]" */
  var blockDone = text.match(/Block (\d+)\/(\d+):\s+(\d+)\s*->\s*(\d+)\s*\(([\d.]+)x\)\s*\[([^\]]+)\]/);
  if (blockDone) {
    var cur = parseInt(blockDone[1]);
    var tot = parseInt(blockDone[2]);
    var bIn = parseInt(blockDone[3]);
    progressState.currentBlock = cur;
    progressState.totalBlocks = tot;
    progressState.bytesProcessed += bIn;
    progressState.phase = 'block-done';
    progressState.currentStrategy = blockDone[6];
    progressState.bestStrategy = blockDone[6];
    progressState.bestRatio = parseFloat(blockDone[5]);
    /* Reset per-block state for next block */
    progressState.testedStrategies = [];
    emitProgress(Math.round((cur / tot) * 100));
    return;
  }

  /* Block DUP: "Block 1/2: DUP of 3" */
  var blockDup = text.match(/Block (\d+)\/(\d+): DUP/);
  if (!blockDup) blockDup = text.match(/Block (\d+) \[.*?:(\d+)\/(\d+)\]: DUP/);
  if (blockDup) {
    var curD = parseInt(blockDup[1]);
    progressState.currentBlock = curD;
    progressState.phase = 'block-done';
    progressState.currentStrategy = 'DUP (dedup)';
    progressState.testedStrategies = [];
    emitProgress();
    return;
  }

  /* HPK6 block with file info: "Block 5 [file.txt:1/3]: 8388608 -> 2345678 (3.57x) [BWT+O1]" */
  var hpk6Block = text.match(/Block (\d+) \[([^\]]+):(\d+)\/(\d+)\]:\s+(\d+)\s*->\s*(\d+)\s*\(([\d.]+)x\)\s*\[([^\]]+)\]/);
  if (hpk6Block) {
    progressState.currentBlock = parseInt(hpk6Block[1]);
    progressState.currentFile = hpk6Block[2];
    progressState.bytesProcessed += parseInt(hpk6Block[5]);
    progressState.phase = 'block-done';
    progressState.currentStrategy = hpk6Block[8];
    progressState.bestStrategy = hpk6Block[8];
    progressState.bestRatio = parseFloat(hpk6Block[7]);
    progressState.testedStrategies = [];
    emitProgress();
    return;
  }

  /* ── Dedup tracking ── */
  if (text.includes('[DEDUP]')) {
    lastDedupCount++;
  }
  if (text.match(/Dedup saved: (\d+)/)) {
    lastDedupSaved = parseInt(text.match(/Dedup saved: (\d+)/)[1]);
  }

  /* ── Archive list entries ── */
  var listMatch = text.match(/^(FILE|DIR|DEDUP)\s+(\S+)\s+(\d+)\s+([0-9A-Fa-f]+)\s+(.+)$/);
  if (listMatch) {
    listEntries.push({
      type: listMatch[1],
      size: parseSize(listMatch[2]),
      blocks: parseInt(listMatch[3]),
      crc: listMatch[4],
      path: listMatch[5],
      isDedup: listMatch[1] === 'DEDUP'
    });
  }

  /* ── Done messages ── */
  var doneMatch = text.match(/\[HPK6\] Done:.*?\((\d+\.?\d*)x\)/);
  if (doneMatch) {
    progressState.phase = 'done';
    emitProgress(100);
    return;
  }

  var done5Match = text.match(/\[HP5\] Done:.*?\((\d+\.?\d*)x\)/);
  if (done5Match) {
    progressState.phase = 'done';
    emitProgress(100);
    return;
  }
}

function parseSize(str) {
  const match = str.match(/([\d.]+)(B|KB|MB|GB)?/);
  if (!match) return 0;
  const val = parseFloat(match[1]);
  switch (match[2]) {
    case 'GB': return val * 1073741824;
    case 'MB': return val * 1048576;
    case 'KB': return val * 1024;
    default: return val;
  }
}
