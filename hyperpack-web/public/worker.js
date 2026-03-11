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
        /* HPK6 archive mode */
        compressArchive(files, params, outName);
      } else if (files.length === 1) {
        /* HPK5 single file mode */
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
function compressSingle(file, params, outName) {
  const inPath = '/input';       /* C hardcodes this path */
  const outPath = '/output.hpk'; /* C hardcodes this path */
  cleanupMemfs([inPath, outPath]);

  const data = new Uint8Array(file.data);
  Module.FS.writeFile(inPath, data);

  const start = performance.now();
  const ret = Module._hp_compress(params.blockSizeMB || 8); /* C takes only block_mb */

  if (ret !== 0) {
    self.postMessage({ type: 'error', message: 'Compression failed (code ' + ret + ')' });
    return;
  }

  const compressed = Module.FS.readFile(outPath);
  const elapsed = (performance.now() - start) / 1000;

  /* Copy into a correctly-sized ArrayBuffer — FS.readFile may return a view
     of a larger internal MEMFS/heap buffer, so compressed.buffer can be
     oversized and would corrupt the downloaded file. */
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

  // Remove old input dir and recreate it fresh
  try { Module.FS.rmdir(dirName); } catch (e) {}
  removeRecursive(dirName);
  try { Module.FS.mkdir(dirName); } catch (e) {}

  // Write all files to MEMFS directory structure
  let totalSize = 0;
  const dirs = new Set();
  dirs.add(dirName);

  for (const file of files) {
    const fullPath = dirName + '/' + file.name;
    // Ensure parent directories exist
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

  const start = performance.now();
  const dirPtr = Module.stringToNewUTF8(dirName);
  const outPtr = Module.stringToNewUTF8(outPath);
  const ret = Module._hp_archive_compress(dirPtr, outPtr, params.blockSizeMB || 8);
  Module._free(dirPtr);
  Module._free(outPtr);

  if (ret !== 0) {
    self.postMessage({ type: 'error', message: 'Archive compression failed (code ' + ret + ')' });
    return;
  }

  const compressed = Module.FS.readFile(outPath);
  const elapsed = (performance.now() - start) / 1000;

  /* Copy into a correctly-sized ArrayBuffer (see compressSingle comment) */
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
  const outPath = '/output';    /* C hardcodes this path for HPK5 output */
  const outDir = '/output_dir';
  cleanupMemfs([inPath, outPath]);
  removeRecursive(outDir);

  const data = new Uint8Array(fileBuffer);
  Module.FS.writeFile(inPath, data);

  // Detect format
  const fmtPtr = Module.stringToNewUTF8(inPath);
  const fmt = Module._hp_detect_format(fmtPtr);
  Module._free(fmtPtr);

  const start = performance.now();

  if (fmt === 6) {
    // HPK6 archive
    try { Module.FS.mkdir(outDir); } catch (e) {}
    const inPtr2 = Module.stringToNewUTF8(inPath);
    const outDirPtr = Module.stringToNewUTF8(outDir);
    const nullPtr = 0; // NULL for extract_pattern = extract all
    const ret = Module._hp_archive_decompress(inPtr2, outDirPtr, nullPtr);
    Module._free(inPtr2);
    Module._free(outDirPtr);

    if (ret !== 0) {
      self.postMessage({ type: 'error', message: 'Archive decompression failed' });
      return;
    }

    // Collect all extracted files
    const extractedFiles = collectFiles(outDir, '');
    const totalSize = extractedFiles.reduce((sum, f) => sum + f.size, 0);
    const elapsed = (performance.now() - start) / 1000;

    if (extractedFiles.length === 1) {
      // Single file: return it directly
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
      // Multiple files: send all files as transferable array
      var allFiles = [];
      var transferables = [];
      for (var i = 0; i < extractedFiles.length; i++) {
        var fileBuf = new Uint8Array(extractedFiles[i].data).buffer;
        allFiles.push({ name: extractedFiles[i].name, data: fileBuf, size: extractedFiles[i].size });
        transferables.push(fileBuf);
      }
      var baseName = fileName.replace(/\.hpk(\.txt)?$/i, '');
      self.postMessage({
        type: 'done-multi',
        name: baseName,
        originalSize: data.length,
        decompressedSize: totalSize,
        elapsed: elapsed,
        fileCount: extractedFiles.length,
        files: allFiles
      }, transferables);
    }

    removeRecursive(outDir);
  } else {
    // HPK5 single file
    const ret = Module._hp_decompress(); /* C takes no args, uses hardcoded paths */

    if (ret !== 0) {
      self.postMessage({ type: 'error', message: 'Decompression failed' });
      return;
    }

    const output = Module.FS.readFile(outPath);
    const elapsed = (performance.now() - start) / 1000;

    /* Copy into a correctly-sized ArrayBuffer */
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
      const entries = Module.FS.readdir(path).filter(e => e !== '.' && e !== '..');
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
    const entries = Module.FS.readdir(basePath).filter(e => e !== '.' && e !== '..');
    for (const entry of entries) {
      const fullPath = basePath + '/' + entry;
      const relPath = rel ? rel + '/' + entry : entry;
      const stat = Module.FS.stat(fullPath);
      if (Module.FS.isDir(stat.mode)) {
        files.push(...collectFiles(fullPath, relPath));
      } else {
        const data = Module.FS.readFile(fullPath);
        files.push({ name: relPath, data: data, size: data.length });
      }
    }
  } catch (e) {}
  return files;
}

/* ===== Progress parsing from stderr ===== */
let lastStrategies = {};
let lastDedupCount = 0;
let lastDedupSaved = 0;
let listEntries = [];

function parseProgress(text) {
  // Strategy tracking
  const stratMatch = text.match(/\[([A-Za-z0-9+_]+)\]$/);
  if (stratMatch) {
    const name = stratMatch[1];
    lastStrategies[name] = (lastStrategies[name] || 0) + 1;
  }

  // Block progress
  const blockMatch = text.match(/Block (\d+).*?(\d+)\/(\d+)/);
  if (blockMatch) {
    const current = parseInt(blockMatch[1]);
    const total = parseInt(blockMatch[3]);
    if (total > 0) {
      self.postMessage({
        type: 'progress',
        percent: Math.round((current / total) * 100),
        detail: text.trim()
      });
    }
  }

  // Dedup tracking
  if (text.includes('[DEDUP]')) {
    lastDedupCount++;
  }
  if (text.match(/Dedup saved: (\d+)/)) {
    lastDedupSaved = parseInt(text.match(/Dedup saved: (\d+)/)[1]);
  }

  // List parsing
  const listMatch = text.match(/^(FILE|DIR|DEDUP)\s+(\S+)\s+(\d+)\s+([0-9A-Fa-f]+)\s+(.+)$/);
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

  // Done message for HPK6
  const doneMatch = text.match(/\[HPK6\] Done:.*?\((\d+\.?\d*)x\)/);
  if (doneMatch) {
    self.postMessage({ type: 'progress', percent: 100, detail: text.trim() });
  }

  // HPK5 done
  const done5Match = text.match(/\[HP5\] Done:.*?\((\d+\.?\d*)x\)/);
  if (done5Match) {
    self.postMessage({ type: 'progress', percent: 100, detail: text.trim() });
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
