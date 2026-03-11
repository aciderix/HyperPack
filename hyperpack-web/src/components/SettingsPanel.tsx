import { X, ChevronDown, ChevronUp } from 'lucide-react';
import { useState } from 'react';
import { CompressParams, StrategyMode } from '../workers/bridge';
import * as native from '../lib/native';

/** All 31 strategies with their index, name, and category for display */
const STRATEGIES = [
  { id: 0,  name: 'STORE',         cat: 'Basic',  desc: 'No compression (passthrough)' },
  { id: 1,  name: 'BWT+O0',        cat: 'BWT',    desc: 'Burrows-Wheeler + Order-0 arithmetic' },
  { id: 2,  name: 'BWT+O1',        cat: 'BWT',    desc: 'Burrows-Wheeler + Order-1 arithmetic' },
  { id: 3,  name: 'D+BWT+O0',      cat: 'BWT',    desc: 'Delta + BWT + Order-0' },
  { id: 4,  name: 'D+BWT+O1',      cat: 'BWT',    desc: 'Delta + BWT + Order-1' },
  { id: 5,  name: 'LZP+BWT+O0',    cat: 'LZP',    desc: 'LZ Prediction + BWT + Order-0' },
  { id: 6,  name: 'LZP+BWT+O1',    cat: 'LZP',    desc: 'LZ Prediction + BWT + Order-1' },
  { id: 7,  name: 'D+LZP+BWT+O0',  cat: 'LZP',    desc: 'Delta + LZP + BWT + Order-0' },
  { id: 8,  name: 'D+LZP+BWT+O1',  cat: 'LZP',    desc: 'Delta + LZP + BWT + Order-1' },
  { id: 9,  name: 'CM',            cat: 'CM',     desc: 'Context Mixing' },
  { id: 10, name: 'BWT+CM',        cat: 'CM',     desc: 'BWT + Context Mixing' },
  { id: 11, name: 'BWT+MTF+CM',    cat: 'CM',     desc: 'BWT + Move-to-Front + CM' },
  { id: 12, name: 'BWT+O2',        cat: 'BWT',    desc: 'BWT + Order-2 arithmetic' },
  { id: 13, name: 'D+BWT+O2',      cat: 'BWT',    desc: 'Delta + BWT + Order-2' },
  { id: 14, name: 'LZP+BWT+O2',    cat: 'LZP',    desc: 'LZP + BWT + Order-2' },
  { id: 15, name: 'D+LZP+BWT+O2',  cat: 'LZP',    desc: 'Delta + LZP + BWT + Order-2' },
  { id: 16, name: 'PPM',           cat: 'PPM',    desc: 'Prediction by Partial Matching' },
  { id: 17, name: 'BWT+PPM',       cat: 'PPM',    desc: 'BWT + PPM' },
  { id: 18, name: 'BWT+MTF+PPM',   cat: 'PPM',    desc: 'BWT + Move-to-Front + PPM' },
  { id: 19, name: 'Audio',         cat: 'Special', desc: 'Audio PCM predictor' },
  { id: 20, name: 'Base64',        cat: 'Special', desc: 'Base64 decoder (deprecated)' },
  { id: 21, name: 'Base64v2',      cat: 'Special', desc: 'Base64 decode + recompress' },
  { id: 22, name: 'LZ77+BWT+O0',   cat: 'LZ',     desc: 'LZ77 + BWT + Order-0' },
  { id: 23, name: 'LZ77+BWT+O1',   cat: 'LZ',     desc: 'LZ77 + BWT + Order-1' },
  { id: 24, name: 'LZMA',          cat: 'LZ',     desc: 'LZMA (Lempel-Ziv-Markov chain)' },
  { id: 25, name: 'BCJ+LZMA',      cat: 'LZ',     desc: 'x86 bytecode filter + LZMA' },
  { id: 26, name: 'F32XOR+BWT',    cat: 'Special', desc: 'Float32 XOR-delta + BWT' },
  { id: 27, name: 'BWT+O0PS',      cat: 'BWT',    desc: 'BWT + Order-0 with pre-scanned freq' },
  { id: 28, name: 'BWT+rANS',      cat: 'BWT',    desc: 'BWT + rANS entropy coder' },
  { id: 29, name: 'BWT+CTX2',      cat: 'BWT',    desc: 'BWT + 2-context Order-0' },
  { id: 30, name: 'BWT+O1PS',      cat: 'BWT',    desc: 'BWT + Order-1 with pre-scanned freq' },
] as const;

const CATEGORIES = ['BWT', 'LZP', 'CM', 'PPM', 'LZ', 'Special', 'Basic'] as const;
const CAT_COLORS: Record<string, string> = {
  BWT:     'bg-blue-500/20 text-blue-400 border-blue-500/30',
  LZP:     'bg-purple-500/20 text-purple-400 border-purple-500/30',
  CM:      'bg-orange-500/20 text-orange-400 border-orange-500/30',
  PPM:     'bg-green-500/20 text-green-400 border-green-500/30',
  LZ:      'bg-cyan-500/20 text-cyan-400 border-cyan-500/30',
  Special: 'bg-pink-500/20 text-pink-400 border-pink-500/30',
  Basic:   'bg-gray-500/20 text-gray-400 border-gray-500/30',
};

interface SettingsPanelProps {
  isOpen: boolean;
  onClose: () => void;
  settings: CompressParams;
  onSettingsChange: (settings: CompressParams) => void;
}

export function SettingsPanel({ isOpen, onClose, settings, onSettingsChange }: SettingsPanelProps) {
  const [showStrategies, setShowStrategies] = useState(false);
  if (!isOpen) return null;
  const isNativeMode = native.isNative();

  const strategyMode = settings.strategyMode || 'auto';
  const strategySet = new Set(settings.strategySet || []);

  const toggleStrategy = (id: number) => {
    const next = new Set(strategySet);
    if (next.has(id)) next.delete(id);
    else next.add(id);
    onSettingsChange({ ...settings, strategySet: Array.from(next).sort((a, b) => a - b) });
  };

  const selectCategory = (cat: string) => {
    const catIds = STRATEGIES.filter(s => s.cat === cat).map(s => s.id);
    const allSelected = catIds.every(id => strategySet.has(id));
    const next = new Set(strategySet);
    if (allSelected) {
      catIds.forEach(id => next.delete(id));
    } else {
      catIds.forEach(id => next.add(id));
    }
    onSettingsChange({ ...settings, strategySet: Array.from(next).sort((a, b) => a - b) });
  };

  const setMode = (mode: StrategyMode) => {
    onSettingsChange({ ...settings, strategyMode: mode, strategySet: [] });
  };

  const needsStrategyList = strategyMode === 'include' || strategyMode === 'exclude';

  return (
    <div 
      className="fixed inset-0 z-50 flex justify-end bg-black/50 backdrop-blur-sm animate-fade-in"
      onClick={onClose}
    >
      <div 
        className="w-full max-w-md bg-hp-card h-full shadow-2xl border-l border-hp-border flex flex-col animate-slide-in-right"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex items-center justify-between p-4 border-b border-hp-border">
          <h2 className="text-lg font-bold text-hp-text flex items-center gap-2">
            <span>⚙️</span> Compression Settings
          </h2>
          <button 
            onClick={onClose}
            className="p-2 text-hp-muted hover:text-hp-text hover:bg-hp-hover rounded-lg transition-colors"
          >
            <X className="w-5 h-5" />
          </button>
        </div>
        
        <div className="flex-1 overflow-y-auto p-6 space-y-6">
          {/* Strategy Mode Selector */}
          <div className="space-y-3">
            <label className="block text-sm font-medium text-hp-text">Strategy Selection</label>
            <div className="grid grid-cols-2 gap-2">
              {([
                { mode: 'auto' as StrategyMode, icon: '🧠', label: 'Auto', desc: 'Best per block' },
                { mode: 'force' as StrategyMode, icon: '🎯', label: 'Force', desc: 'Single strategy' },
                { mode: 'include' as StrategyMode, icon: '✅', label: 'Include', desc: 'Only selected' },
                { mode: 'exclude' as StrategyMode, icon: '🚫', label: 'Exclude', desc: 'Skip selected' },
              ]).map(({ mode, icon, label, desc }) => (
                <button
                  key={mode}
                  onClick={() => setMode(mode)}
                  className={`p-3 rounded-xl border text-left transition-all duration-200 ${
                    strategyMode === mode
                      ? 'border-hp-accent bg-hp-accent/10 shadow-sm shadow-hp-accent/10'
                      : 'border-hp-border bg-hp-bg hover:border-hp-accent/50'
                  }`}
                >
                  <div className="flex items-center gap-2">
                    <span className="text-base">{icon}</span>
                    <span className="text-sm font-medium text-hp-text">{label}</span>
                  </div>
                  <p className="text-[10px] text-hp-muted mt-1">{desc}</p>
                </button>
              ))}
            </div>
          </div>

          {/* Force strategy dropdown */}
          {strategyMode === 'force' && (
            <div className="space-y-2">
              <label className="block text-sm font-medium text-hp-text">Force Strategy</label>
              <select
                value={settings.forceStrategy}
                onChange={(e) => onSettingsChange({ ...settings, forceStrategy: Number(e.target.value) })}
                className="w-full bg-hp-bg border border-hp-border text-hp-text rounded-lg px-3 py-2 focus:outline-none focus:border-hp-accent text-sm"
              >
                {STRATEGIES.map((s) => (
                  <option key={s.id} value={s.id}>
                    {s.id}. {s.name} — {s.desc}
                    {s.id === 20 ? ' ⚠️' : ''}
                  </option>
                ))}
              </select>
              <p className="text-xs text-hp-muted">
                All blocks will use this strategy. Useful for benchmarking or specific data types.
              </p>
            </div>
          )}

          {/* Include/Exclude strategy multi-select */}
          {needsStrategyList && (
            <div className="space-y-3">
              <div className="flex items-center justify-between">
                <label className="text-sm font-medium text-hp-text">
                  {strategyMode === 'include' ? 'Strategies to try' : 'Strategies to skip'}
                </label>
                <span className="text-xs text-hp-muted">
                  {strategySet.size} of {STRATEGIES.length} selected
                </span>
              </div>

              {/* Category quick-select */}
              <div className="flex flex-wrap gap-1.5">
                {CATEGORIES.map(cat => {
                  const catIds = STRATEGIES.filter(s => s.cat === cat).map(s => s.id);
                  const allSelected = catIds.every(id => strategySet.has(id));
                  return (
                    <button
                      key={cat}
                      onClick={() => selectCategory(cat)}
                      className={`px-2 py-1 rounded-md text-[10px] font-medium border transition-all ${
                        allSelected
                          ? CAT_COLORS[cat]
                          : 'bg-hp-bg border-hp-border text-hp-muted hover:text-hp-text'
                      }`}
                    >
                      {cat}
                    </button>
                  );
                })}
              </div>

              {/* Strategy grid */}
              <div className="bg-hp-bg rounded-xl border border-hp-border max-h-64 overflow-y-auto">
                <div className="grid grid-cols-1 divide-y divide-hp-border">
                  {STRATEGIES.map((s) => (
                    <label
                      key={s.id}
                      className={`flex items-center gap-3 px-3 py-2 hover:bg-hp-hover cursor-pointer transition-colors ${
                        s.id === 20 ? 'opacity-50' : ''
                      }`}
                    >
                      <input
                        type="checkbox"
                        checked={strategySet.has(s.id)}
                        onChange={() => toggleStrategy(s.id)}
                        className="w-3.5 h-3.5 rounded accent-hp-accent flex-shrink-0"
                      />
                      <span className={`text-[10px] px-1.5 py-0.5 rounded border font-mono ${CAT_COLORS[s.cat]}`}>
                        {s.id.toString().padStart(2, '0')}
                      </span>
                      <div className="flex-1 min-w-0">
                        <span className="text-xs font-medium text-hp-text">{s.name}</span>
                        <span className="text-[10px] text-hp-muted ml-2">{s.desc}</span>
                      </div>
                    </label>
                  ))}
                </div>
              </div>

              {strategyMode === 'include' && strategySet.size === 0 && (
                <p className="text-xs text-hp-error">
                  ⚠ Select at least one strategy, otherwise auto-mode will be used.
                </p>
              )}
            </div>
          )}

          {/* Auto mode info */}
          {strategyMode === 'auto' && (
            <div className="p-4 bg-hp-bg rounded-xl border border-hp-border space-y-2">
              <div className="flex items-center gap-2">
                <span className="text-lg">🧠</span>
                <span className="text-sm font-medium text-hp-text">Auto Strategy Selection</span>
              </div>
              <p className="text-xs text-hp-muted leading-relaxed">
                HyperPack automatically analyzes your data and tests all 31 compression strategies
                per block, keeping the best result. No manual selection needed.
              </p>
              <button
                onClick={() => setShowStrategies(!showStrategies)}
                className="flex items-center gap-1 text-xs text-hp-accent hover:text-hp-accent-hover transition-colors"
              >
                {showStrategies ? <ChevronUp className="w-3 h-3" /> : <ChevronDown className="w-3 h-3" />}
                {showStrategies ? 'Hide' : 'View'} all {STRATEGIES.length} strategies
              </button>
              {showStrategies && (
                <div className="mt-2 grid grid-cols-2 gap-1">
                  {STRATEGIES.map((s) => (
                    <div key={s.id} className="flex items-center gap-1.5 text-[10px]">
                      <span className={`px-1 py-0.5 rounded border font-mono ${CAT_COLORS[s.cat]}`}>
                        {s.id.toString().padStart(2, '0')}
                      </span>
                      <span className="text-hp-muted truncate">{s.name}</span>
                    </div>
                  ))}
                </div>
              )}
            </div>
          )}

          {/* Block size */}
          <div className="space-y-2">
            <label className="block text-sm font-medium text-hp-text">Block size</label>
            <select 
              value={settings.blockSizeMB}
              onChange={(e) => onSettingsChange({ ...settings, blockSizeMB: Number(e.target.value) })}
              className="w-full bg-hp-bg border border-hp-border text-hp-text rounded-lg px-3 py-2 focus:outline-none focus:border-hp-accent"
            >
              <option value={1}>1 MB (~20 MB RAM)</option>
              <option value={2}>2 MB (~35 MB RAM)</option>
              <option value={4}>4 MB (~60 MB RAM)</option>
              <option value={8}>8 MB (~120 MB RAM) — default</option>
              <option value={16}>16 MB (~250 MB RAM)</option>
              <option value={32}>32 MB (~500 MB RAM)</option>
              <option value={64}>64 MB (~1 GB RAM) — max WASM</option>
            </select>
            <p className="text-xs text-hp-muted">
              Larger blocks = better compression ratio, but more memory and slower.
              BWT-based strategies benefit most from large blocks.
            </p>
          </div>

          {/* HPK6 Archive mode info */}
          <div className="p-4 bg-hp-bg rounded-xl border border-hp-border space-y-2">
            <div className="flex items-center gap-2">
              <img src={import.meta.env.BASE_URL + 'logo-32.png'} alt="" className="w-5 h-5" />
              <span className="text-sm font-medium text-hp-text">Archive Mode (HPK6)</span>
            </div>
            <p className="text-xs text-hp-muted leading-relaxed">
              Archive mode (HPK6) is automatically used when multiple files or a folder is selected. 
              Single files use the classic HPK5 format. HPK6 adds a file/directory table, CRC checksums, 
              and block-level deduplication for repeated content.
            </p>
          </div>

          {/* Threads (native only) */}
          {isNativeMode && (
            <div className="space-y-2">
              <label className="block text-sm font-medium text-hp-text">
                Threads
                <span className="ml-2 text-hp-muted font-normal">
                  {settings.nthreads === 0 ? '(auto)' : `${settings.nthreads} thread${settings.nthreads > 1 ? 's' : ''}`}
                </span>
              </label>
              <input
                type="range"
                min={0}
                max={navigator.hardwareConcurrency || 16}
                value={settings.nthreads}
                onChange={(e) => onSettingsChange({ ...settings, nthreads: Number(e.target.value) })}
                className="w-full accent-hp-accent"
              />
              <p className="text-xs text-hp-muted">
                0 = auto-detect (uses all available cores). Higher = faster on multi-block files.
              </p>
            </div>
          )}

          {/* Engine info */}
          <div className="p-4 bg-hp-accent/5 rounded-xl border border-hp-accent/20 space-y-2">
            <div className="flex items-center gap-2">
              <span className="text-lg">{isNativeMode ? '⚡' : '🌐'}</span>
              <span className="text-sm font-medium text-hp-text">
                {isNativeMode ? 'Native Engine' : 'Browser Engine'}
              </span>
            </div>
            <p className="text-xs text-hp-muted leading-relaxed">
              {isNativeMode
                ? 'Full multi-threaded HyperPack engine running natively. No file size limits. Output is saved directly to disk next to the input file.'
                : 'HyperPack engine compiled to WebAssembly. Processing is single-threaded in the browser. For large files, use the native desktop app or CLI.'}
            </p>
          </div>
        </div>
      </div>
    </div>
  );
}
