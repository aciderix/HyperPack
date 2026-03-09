import { HyperPackProgress } from '../hooks/useHyperPack';

interface ProgressBarProps {
  progress: HyperPackProgress;
  mode: 'compress' | 'decompress';
}

function formatSpeed(bytesPerSec: number) {
  if (bytesPerSec === 0) return '—';
  return (bytesPerSec / (1024 * 1024)).toFixed(1) + ' MB/s';
}

function formatTime(seconds: number) {
  if (!isFinite(seconds) || seconds <= 0) return '~';
  if (seconds < 60) return `${Math.round(seconds)}s`;
  const m = Math.floor(seconds / 60);
  const s = Math.round(seconds % 60);
  return `${m}m ${s}s`;
}

export function ProgressBar({ progress, mode }: ProgressBarProps) {
  const actionText = mode === 'compress' ? 'Compressing' : 'Decompressing';
  
  return (
    <div className="p-5 bg-hp-card border border-hp-border rounded-xl space-y-4">
      <div className="flex justify-between items-end">
        <div>
          <h4 className="text-hp-text font-medium">
            {actionText}... Block {progress.currentBlock}/{progress.totalBlocks}
          </h4>
          <p className="text-sm text-hp-muted mt-1">
            Strategy: <span className="text-hp-accent font-medium">{progress.strategy}</span>
          </p>
        </div>
        <div className="text-right">
          <div className="text-2xl font-bold text-hp-accent tabular-nums">
            {progress.percent}%
          </div>
          {progress.currentRatio !== undefined && progress.currentRatio > 0 && (
            <div className="text-xs text-hp-muted mt-0.5">
              Ratio: <span className="text-hp-text font-medium">{progress.currentRatio.toFixed(2)}x</span>
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
      
      <div className="flex justify-between text-sm text-hp-muted">
        <span>Speed: {formatSpeed(progress.speed)}</span>
        <span>ETA: {formatTime(progress.eta)}</span>
      </div>
    </div>
  );
}
