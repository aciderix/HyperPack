# HyperPack v11 — Benchmark Results

## Global Summary (3 Corpora — 38 files)

| Rank | Compressor | Avg Ratio | Wins |
|------|-----------|-----------|------|
| 🥇 | **HyperPack v11** | **4.413x** | **12** |
| 🥈 | xz -9 | 4.119x | 9 |
| 🥉 | bzip2 -9 | 3.967x | 15 |
| 4 | gzip -9 | 3.063x | 1 |

## Canterbury Corpus (11 files)

| File | Size | HP v11 | xz | bzip2 | gzip | Winner |
|------|------|--------|-----|-------|------|--------|
| alice29.txt | 149 KB | 2.521x | 2.578x | 2.617x | 2.164x | bzip2 |
| asyoulik.txt | 125 KB | 2.318x | 2.363x | 2.437x | 2.013x | bzip2 |
| cp.html | 24 KB | 3.258x | 3.402x | 2.851x | 2.765x | xz |
| fields.c | 11 KB | 2.929x | 3.177x | 2.580x | 2.375x | xz |
| grammar.lsp | 3.7 KB | 2.459x | 2.633x | 2.286x | 2.008x | xz |
| kennedy.xls | 1006 KB | 5.736x | 4.775x | 3.689x | 3.131x | **HP** |
| lcet10.txt | 417 KB | 3.118x | 3.070x | 3.054x | 2.487x | **HP** |
| plrabn12.txt | 471 KB | 2.698x | 2.705x | 2.771x | 2.267x | bzip2 |
| ptt5 | 502 KB | 24.000x | 14.797x | 8.046x | 8.048x | **HP** |
| sum | 37 KB | 3.136x | 3.364x | 2.365x | 2.405x | xz |
| xargs.1 | 4.2 KB | 2.471x | 2.471x | 2.210x | 1.954x | tie |

**HP v11: 6.226x avg** | xz: 5.709x | bzip2: 4.133x | gzip: 3.419x

## Calgary Corpus (18 files)

| File | Size | HP v11 | xz | bzip2 | gzip | Winner |
|------|------|--------|-----|-------|------|--------|
| bib | 111 KB | 2.929x | 2.985x | 2.933x | 2.348x | xz |
| book1 | 751 KB | 2.592x | 2.598x | 2.640x | 2.186x | bzip2 |
| book2 | 597 KB | 2.856x | 2.864x | 2.867x | 2.363x | bzip2 |
| geo | 100 KB | 2.120x | 2.286x | 1.795x | 1.641x | xz |
| news | 369 KB | 2.669x | 2.688x | 2.572x | 2.171x | xz |
| obj1 | 21 KB | 2.549x | 2.693x | 2.025x | 2.024x | xz |
| obj2 | 246 KB | 3.574x | 3.262x | 2.627x | 2.508x | **HP** |
| paper1 | 53 KB | 2.649x | 2.746x | 2.458x | 2.124x | xz |
| paper2 | 82 KB | 2.602x | 2.639x | 2.543x | 2.158x | xz |
| paper3 | 46 KB | 2.330x | 2.408x | 2.282x | 1.987x | xz |
| paper4 | 13 KB | 2.275x | 2.487x | 2.163x | 1.860x | xz |
| paper5 | 11 KB | 2.245x | 2.475x | 2.075x | 1.742x | xz |
| paper6 | 38 KB | 2.642x | 2.801x | 2.386x | 2.119x | xz |
| pic | 502 KB | 24.000x | 14.797x | 8.046x | 8.048x | **HP** |
| progc | 39 KB | 2.766x | 2.889x | 2.467x | 2.155x | xz |
| progl | 71 KB | 3.476x | 3.451x | 3.088x | 2.561x | **HP** |
| progp | 49 KB | 3.282x | 3.379x | 2.914x | 2.504x | xz |
| trans | 93 KB | 3.918x | 3.637x | 3.437x | 2.776x | **HP** |

**HP v11: 3.804x avg** | xz: 3.767x | bzip2: 2.923x | gzip: 2.468x

## Silesia Corpus (partial — files < 10MB)

| File | Size | HP v11 | HP v10.2 | xz | Winner |
|------|------|--------|----------|-----|--------|
| dickens | 9.7 MB | 4.040x | 4.040x | 3.840x | **HP** |
| mr | 9.5 MB | 4.210x | 4.210x | 3.960x | **HP** |
| nci | 32 MB | 24.530x | 24.530x | 18.440x | **HP** |
| ooffice | 5.9 MB | 2.420x | 2.360x | 2.370x | **HP v11** |
| osdb | 9.6 MB | 3.940x | 3.940x | 3.930x | **HP** |
| reymont | 6.3 MB | 5.870x | 5.870x | 5.420x | **HP** |
| sao | 6.9 MB | 1.556x | 1.556x | 1.590x | xz |
| x-ray | 8.1 MB | 2.130x | 2.130x | 2.060x | **HP** |
| xml | 5.1 MB | 12.120x | 12.120x | 11.020x | **HP** |

## Evolution: v9 → v10.2 → v11

| Version | Avg Ratio | Total Time | Key Changes |
|---------|-----------|------------|-------------|
| v9 | 4.218x | 55.3s | Baseline |
| v10.2 | 4.341x (+2.9%) | 65.0s | LZMA 64MB, MTF-2, smart heuristic |
| **v11** | **4.413x (+4.6%)** | **57.1s (+3%)** | LZMA forced, BCJ, parallel, adaptive, text detect |

## Notable Improvements v9 → v11

| File | v9 Ratio | v11 Ratio | Improvement |
|------|----------|-----------|-------------|
| sum | 2.429x | 3.136x | **+23.5%** |
| ptt5/pic | 19.719x | 24.000x | **+21.7%** |
| ooffice | 2.030x | 2.420x | **+19.2%** |
| obj2 | 3.113x | 3.574x | **+14.8%** |
| sao | 1.431x | 1.556x | **+8.7%** |
