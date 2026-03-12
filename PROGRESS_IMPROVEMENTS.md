# HyperPack — Améliorations du feedback en temps réel

## Problème identifié

Pendant la compression d'un gros fichier (ex: 170 MB), l'utilisateur voit pendant tout le traitement :

```
Speed: —
ETA:   ~
Strategy: Initializing...
```

Le résultat détaillé n'apparaît qu'à la toute fin. **Pour un traitement de 3 minutes, l'UI semble figée.**

## Cause racine

### Version WASM (Web)

Le code C écrit des dizaines de lignes de log sur `stderr` pendant la compression :

```
[HP5] Compressing file.dat (170.07 MB, 2 blocks of 128 MB)
    [Entropy 5.432 but repeating pattern detected, trying compression]
    [BWT+O0] → teste O0
    [BWT+O1] 134217728 -> 38234567 (3.51x) *** NEW BEST ***
    [LZP+BWT+O0] 134217728 -> 36789012 (3.65x) *** NEW BEST ***
    [LZMA] 134217728 -> 35123456 (3.82x) *** NEW BEST ***
    [Audio] not detected, skipping
    [Base64] not enough b64, skipping
  Block 1/2: 134217728 -> 35123456 (3.82x) [LZMA]
  Block 2/2: 44065792 -> 17654321 (2.50x) [LZP+BWT+O1]
[HP5] Done: 178283520 -> 55345678 bytes (3.22x) in 180.0s
```

Le `worker.js` capturait ces lignes via `printErr` mais le `parseProgress()` **ne remontait au frontend que les lignes `Block X/Y`** finales. Toutes les lignes de test de stratégie étaient ignorées.

### Version native (Tauri Desktop)

**Zéro feedback** : la commande Tauri `hp_compress` appelle le C via `spawn_blocking`, attend le résultat, et renvoie. Aucun event intermédiaire.

## Solution implémentée (6 fichiers, 0 modification du code C)

### 1. `worker.js` — Parser enrichi (`parseProgress()`)

**Avant :** ne capturait que `Block X/Y:` et `Done:` → 2-3 events par fichier.

**Après :** parse ~15 types de messages stderr différents → dizaines d'events par block :

| Pattern | Phase | Info remontée |
|---------|-------|---------------|
| `[HP5] Compressing ... (X MB, N blocks)` | `analyzing` | Taille totale, nombre de blocs |
| `[HPK6] Scanned N entries` | `scanning` | Scan en cours |
| `[HPK6] N files, M dirs, X MB, B blocks` | `analyzing` | Stats archive |
| `[HP5] PNG detected` | `analyzing` | Pré-transformation PNG |
| `[Entropy X.XXX ...]` | `analyzing` | Analyse entropie |
| `[LZMA] N -> M (Xx) *** NEW BEST ***` | `testing` | Résultat stratégie + best ratio |
| `[Sub PPM] N -> M` | `testing` | Sous-flux en test |
| `[Audio] not detected, skipping` | `testing` | Stratégie ignorée |
| `[LZMA] skipped by heuristic` | `testing` | Heuristique LZMA |
| `Block 1/2: N -> M (Xx) [Strat]` | `block-done` | Bloc terminé |
| `Block 5 [file.txt:1/3]: ...` | `block-done` | Bloc HPK6 + fichier |
| `[HP5] Done:` / `[HPK6] Done:` | `done` | Terminé |

Chaque event contient maintenant :
- `phase` : étape en cours
- `currentStrategy` : stratégie testée
- `bestStrategy` + `bestRatio` : meilleure stratégie trouvée
- `totalBytes` / `bytesProcessed` : pour calcul speed/ETA réel
- `testedStrategies[]` : historique des stratégies testées dans le bloc courant
- `currentFile` : fichier en cours (HPK6)

### 2. `bridge.ts` — Type `WorkerResponse` enrichi

Ajout de champs optionnels au type `progress` pour transporter les nouvelles données.

### 3. `useHyperPack.ts` — Gestion de progression enrichie

- **Speed** calculé à partir des `bytesProcessed` réels (bytes/sec) au lieu du % approximatif
- **ETA** calculé à partir des bytes restants / vitesse réelle
- **Listener Tauri** : dans la version native, écoute l'event `hp-progress` pour recevoir les mêmes données
- Nettoyage automatique du listener dans `finally`

### 4. `ProgressBar.tsx` — Interface enrichie

L'ancien composant :
```
Compressing... Block 0/1
Strategy: Initializing...
[████████████                    ] 38%
Speed: —          ETA: ~
```

Le nouveau composant :
```
Compressing…  Block 1/2
📊 Analyzing data…
                                    38%
                           Ratio: 3.82x

[████████████████                ] 38%

⚡ Testing: LZMA
   Best: LZP+BWT+O1 (3.65x)

 BWT+O0 3.51x  BWT+O1 3.51x  LZP+BWT+O0 3.65x  LZMA ...

Speed: 0.9 MB/s   67.2 MB / 170.1 MB   ETA: 1m 54s
```

Éléments ajoutés :
- **Icône + label de phase** (⏳ Initializing, 🔍 Scanning, 📊 Analyzing, ⚡ Testing, ✅ Block complete)
- **Stratégie en cours** avec animation pulse pendant le test
- **Meilleure stratégie** affichée à côté avec son ratio
- **Tags de stratégies testées** : liste compacte scrollable avec highlight vert pour la meilleure
- **Nom du fichier** courant pour les archives HPK6
- **Bytes traités / total** en plus de speed et ETA
- **Speed réel** en MB/s, KB/s ou B/s selon la valeur

### 5. `lib.rs` (Tauri) — Capture stderr + events Tauri

Nouveau système `run_with_progress()` qui :
1. Crée un pipe Unix (ou `_pipe` Windows)
2. Redirige stderr du C vers le pipe
3. Lance un thread lecteur qui parse les lignes en temps réel
4. Émet des events Tauri `hp-progress` avec le même format que le worker WASM
5. Restaure stderr original à la fin

Dépendances ajoutées : `regex-lite` (léger, sans alloc) et `libc` (Unix pipe).

### 6. `Cargo.toml` — Nouvelles dépendances

```toml
regex-lite = "0.1"
[target.'cfg(unix)'.dependencies]
libc = "0.2"
```

## Impact sur les performances

- **Zéro** modification du code C → aucun impact sur les perfs de compression
- Les `fprintf(stderr, ...)` existants sont déjà exécutés → on les parse simplement au lieu de les ignorer
- Côté frontend : les `setProgress()` sont des micro-updates React, négligeables

## Ce qui n'est PAS cassé

- ✅ Aucune modification de `hyperpack.c` / `hyperpack_lib.c` / `hyperpack_wasm.c`
- ✅ Format de fichier HPK5/HPK6 inchangé
- ✅ API WASM (`hp_compress`, `hp_decompress`, etc.) inchangée
- ✅ Commandes Tauri : mêmes signatures, mêmes résultats
- ✅ `ResultPanel` : les résultats finaux restent identiques
- ✅ Rétrocompatibilité : les nouveaux champs de progress sont tous optionnels
