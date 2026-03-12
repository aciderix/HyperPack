import { HyperPackProgress, TestedStrategy } from '../hooks/useHyperPack';

interface ProgressBarProps {
  progress: HyperPackProgress;
  mode: 'compress' | 'decompress';
}

function formatSpeed(bytesPerSec: number) {
  if (bytesPerSec === 0) return '—';
  if (bytesPerSec >= 1048576) return (bytesPerSec / 1048576).toFixed(1) + ' MB/s';
  if (bytesPerSec >= 1024) return (bytesPerSec / 1024).toFixed(0) + ' KB/s';
  return bytesPerSec.toFixed(0) + ' B/s';
}

function formatTime(seconds: number) {
  if (!isFinite(seconds) || seconds <= 0) return '~';
  if (seconds < 60) return `${Math.round(seconds)}s`;
  const m = Math.floor(seconds / 60);
  const s = Math.round(seconds % 60);
  return `${m}m ${s}s`;
}

function formatBytes(bytes: number) {
  if (bytes === 0) return '0 B';
  if (bytes >= 1073741824) return (bytes / 1073741824).toFixed(2) + ' GB';
  if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + ' MB';
  if (bytes >= 1024) return (bytes / 1024).toFixed(0) + ' KB';
  return bytes + ' B';
}

function phaseLabel(phase?: string): string {
  switch (phase) {
    case 'init': return 'Initializing…';
    case 'scanning': return 'Scanning files…';
    case 'analyzing': return 'Analyzing data…';
    case 'testing': return 'Testing strategies';
    case 'block-done': return 'Block complete';
    case 'done': return 'Finishing…';
    default: return 'Processing…';
  }
}

function phaseIcon(phase?: string): string {
  switch (phase) {
    case 'init': return '⏳';
    case 'scanning': return '🔍';
    case 'analyzing': return '📊';
    case 'testing': return '⚡';
    case 'block-done': return '✅';
    case 'done': return '🎉';
    default: return '⏳';
  }
}

export function ProgressBar({ progress, mode }: ProgressBarProps) {
  const actionText = mode === 'compress' ? 'Compressing' : 'Decompressing';
  const isTesting = progress.phase === 'testing';
  const hasStrategies = progress.testedStrategies && progress.testedStrategies.length > 0;

  return (
    <div className="p-5 bg-hp-card border border-hp-border rounded-xl space-y-3">
      {/* Header row: action + phase + percentage */}
      <div className="flex justify-between items-start">
        <div className="min-w-0 flex-1">
          <h4 className="text-hp-text font-medium flex items-center gap-2">
            <span>{actionText}…</span>
            {progress.totalBlocks > 1 && (
              <span className="text-hp-muted text-sm font-normal">
                Block {progress.currentBlock}/{progress.totalBlocks}
              </span>
            )}
          </h4>

          {/* Phase indicator */}
          <div className="flex items-center gap-1.5 mt-1">
            <span className="text-xs">{phaseIcon(progress.phase)}</span>
            <span className="text-sm text-hp-muted">{phaseLabel(progress.phase)}</span>
          </div>
        </div>

        <div className="text-right flex-shrink-0 ml-4">
          <div className="text-2xl font-bold text-hp-accent tabular-nums">
            {progress.percent}%
          </div>
          {progress.bestRatio !== undefined && progress.bestRatio > 0 && (
            <div className="text-xs text-hp-muted mt-0.5">
              Ratio: <span className="text-hp-text font-medium">{progress.bestRatio.toFixed(2)}x</span>
            </div>
          )}
        </div>
      </div>

      {/* Progress bar with shimmer */}
      <div className="h-3 w-full bg-hp-bg rounded-full overflow-hidden">
        <div 
          className="h-full progress-shimmer rounded-full transition-all duration-300 ease-out"
          style={{ width: `${Math.max(2, progress.percent)}%` }}
        />
      </div>

      {/* Current strategy being tested — the key improvement */}
      {progress.currentStrategy && (
        <div className="flex items-center gap-2 px-3 py-2 bg-hp-bg/60 rounded-lg">
          {isTesting && (
            <span className="relative flex h-2 w-2 flex-shrink-0">
              <span className="animate-ping absolute inline-flex h-full w-full rounded-full bg-hp-accent opacity-75"></span>
              <span className="relative inline-flex rounded-full h-2 w-2 bg-hp-accent"></span>
            </span>
          )}
          <span className="text-sm text-hp-muted truncate">
            {isTesting ? 'Testing: ' : ''}
            <span className="text-hp-accent font-medium">{progress.currentStrategy}</span>
          </span>
          {progress.bestStrategy && isTesting && progress.bestStrategy !== progress.currentStrategy && (
            <span className="text-xs text-hp-muted ml-auto flex-shrink-0">
              Best: <span className="text-green-400 font-medium">{progress.bestStrategy}</span>
              {progress.bestRatio ? ` (${progress.bestRatio.toFixed(2)}x)` : ''}
            </span>
          )}
        </div>
      )}

      {/* Tested strategies log (compact, scrollable) — only during strategy testing */}
      {hasStrategies && isTesting && (
        <div className="max-h-20 overflow-y-auto scrollbar-thin">
          <div className="flex flex-wrap gap-1.5">
            {(progress.testedStrategies as TestedStrategy[]).map((s, i) => {
              const isBest = s.name === progress.bestStrategy;
              return (
                <span
                  key={i}
                  className={`inline-flex items-center text-xs px-2 py-0.5 rounded-full border ${
                    isBest
                      ? 'border-green-500/30 bg-green-500/10 text-green-400'
                      : 'border-hp-border bg-hp-bg/40 text-hp-muted'
                  }`}
                >
                  {s.name}
                  <span className={`ml-1 font-medium ${isBest ? 'text-green-300' : 'text-hp-text/70'}`}>
                    {s.ratio.toFixed(2)}x
                  </span>
                </span>
              );
            })}
          </div>
        </div>
      )}

      {/* Current file name for HPK6 archives */}
      {progress.currentFile && (
        <div className="text-xs text-hp-muted truncate">
          📄 {progress.currentFile}
        </div>
      )}

      {/* Speed / ETA / bytes processed */}
      <div className="flex justify-between text-sm text-hp-muted">
        <span>Speed: {formatSpeed(progress.speed)}</span>
        {progress.totalBytes && progress.totalBytes > 0 ? (
          <span className="text-xs tabular-nums">
            {formatBytes(progress.bytesProcessed || 0)} / {formatBytes(progress.totalBytes)}
          </span>
        ) : null}
        <span>ETA: {formatTime(progress.eta)}</span>
      </div>
    </div>
  );
}
