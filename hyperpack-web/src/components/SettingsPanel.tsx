import { X } from 'lucide-react';
import { CompressParams } from '../workers/bridge';
import * as native from '../lib/native';

interface SettingsPanelProps {
  isOpen: boolean;
  onClose: () => void;
  settings: CompressParams;
  onSettingsChange: (settings: CompressParams) => void;
}

export function SettingsPanel({ isOpen, onClose, settings, onSettingsChange }: SettingsPanelProps) {
  if (!isOpen) return null;
  const isNativeMode = native.isNative();

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
          {/* Strategy info */}
          <div className="p-4 bg-hp-bg rounded-xl border border-hp-border space-y-2">
            <div className="flex items-center gap-2">
              <span className="text-lg">🧠</span>
              <span className="text-sm font-medium text-hp-text">Auto Strategy Selection</span>
            </div>
            <p className="text-xs text-hp-muted leading-relaxed">
              HyperPack automatically analyzes your data and tests 26 compression strategies 
              (BWT, LZMA, LZ77, LZP, Delta, Audio, Base64, BCJ+LZMA, and more) per block, 
              keeping the best result. No manual selection needed.
            </p>
          </div>

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
              <span className="text-lg">📦</span>
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
