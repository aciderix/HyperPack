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
function compressSingle(file, params, outName) {
  const inPath = '/input';
  const outPath = '/output.hpk';
  cleanupMemfs([inPath, outPath]);

  const data = new Uint8Array(file.data);
  Module.FS.writeFile(inPath, data);

  const start = performance.now();
  const ret = Module._hp_compress(params.blockSizeMB || 8);

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

function parseProgress(text) {
  const stratMatch = text.match(/\[([A-Za-z0-9+_]+)\]$/);
  if (stratMatch) {
    const name = stratMatch[1];
    lastStrategies[name] = (lastStrategies[name] || 0) + 1;
  }

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

  if (text.includes('[DEDUP]')) {
    lastDedupCount++;
  }
  if (text.match(/Dedup saved: (\d+)/)) {
    lastDedupSaved = parseInt(text.match(/Dedup saved: (\d+)/)[1]);
  }

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

  const doneMatch = text.match(/\[HPK6\] Done:.*?\((\d+\.?\d*)x\)/);
  if (doneMatch) {
    self.postMessage({ type: 'progress', percent: 100, detail: text.trim() });
  }

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
