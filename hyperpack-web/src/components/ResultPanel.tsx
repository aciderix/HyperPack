import { useEffect, useState, useRef } from 'react';
import { Download, RefreshCw, CheckCircle2 } from 'lucide-react';
import { HyperPackResult } from '../hooks/useHyperPack';

interface ResultPanelProps {
  result: HyperPackResult;
  mode: 'compress' | 'decompress';
  onDownload: () => void;
  onReset: () => void;
}

function formatSize(bytes: number) {
  if (bytes === 0) return '0 B';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

// Count-up hook: animates from 0 to target value
function useCountUp(target: number, duration: number = 800): number {
  const [value, setValue] = useState(0);
  const startTimeRef = useRef<number | null>(null);

  useEffect(() => {
    startTimeRef.current = null;
    
    const animate = (timestamp: number) => {
      if (startTimeRef.current === null) startTimeRef.current = timestamp;
      const elapsed = timestamp - startTimeRef.current;
      const progress = Math.min(elapsed / duration, 1);
      // Ease-out cubic
      const eased = 1 - Math.pow(1 - progress, 3);
      setValue(eased * target);
      if (progress < 1) {
        requestAnimationFrame(animate);
      } else {
        setValue(target);
      }
    };
    
    requestAnimationFrame(animate);
  }, [target, duration]);

  return value;
}

export function ResultPanel({ result, mode, onDownload, onReset }: ResultPanelProps) {
  const isCompress = mode === 'compress';
  
  const animatedRatio = useCountUp(result.ratio, 1000);
  const animatedInput = useCountUp(result.inputSize, 800);
  const animatedOutput = useCountUp(result.outputSize, 800);
  const animatedTime = useCountUp(result.totalMs / 1000, 600);
  const animatedSpeed = useCountUp(
    (result.inputSize / (1024 * 1024)) / (result.totalMs / 1000), 
    800
  );
  
  return (
    <div className="p-6 bg-hp-card border border-hp-border rounded-xl space-y-6 animate-fade-in">
      <div className="flex items-center gap-3 text-hp-success">
        <CheckCircle2 className="w-6 h-6" />
        <h3 className="text-lg font-medium">
          {isCompress ? 'Compression' : 'Decompression'} complete!
          {isCompress && result.fileCount && result.fileCount > 1 && (
            <span className="text-sm font-normal text-hp-muted ml-2">({result.fileCount} files → HPK6)</span>
          )}
        </h3>
      </div>
      
      <div className="flex items-center justify-center gap-6 py-4 bg-hp-bg rounded-lg border border-hp-border">
        <div className="text-center">
          <div className="text-2xl font-bold text-hp-text tabular-nums">{formatSize(Math.round(animatedInput))}</div>
          <div className="text-sm text-hp-muted mt-1">Original</div>
        </div>
        <div className="text-hp-muted text-xl">→</div>
        <div className="text-center">
          <div className="text-2xl font-bold text-hp-accent tabular-nums">{formatSize(Math.round(animatedOutput))}</div>
          <div className="text-sm text-hp-muted mt-1">{isCompress ? 'Compressed' : 'Decompressed'}</div>
        </div>
      </div>
      
      <div className="grid grid-cols-3 gap-4 text-center divide-x divide-hp-border">
        <div>
          <div className="text-sm text-hp-muted mb-1">Ratio</div>
          <div className="font-bold text-lg text-hp-accent tabular-nums">{animatedRatio.toFixed(2)}x</div>
        </div>
        <div>
          <div className="text-sm text-hp-muted mb-1">Time</div>
          <div className="font-medium text-hp-text tabular-nums">{animatedTime.toFixed(1)}s</div>
        </div>
        <div>
          <div className="text-sm text-hp-muted mb-1">Speed</div>
          <div className="font-medium text-hp-text tabular-nums">{animatedSpeed.toFixed(1)} MB/s</div>
        </div>
      </div>
      
      {/* Archive info (HPK6) */}
      {isCompress && result.fileCount && result.fileCount > 1 && (
        <div className="flex items-center gap-4 text-sm text-hp-muted bg-hp-bg rounded-lg border border-hp-border p-3">
          <div className="flex items-center gap-1.5">
            <span className="text-hp-accent font-bold">{result.fileCount}</span> files archived (HPK6)
          </div>
          {result.dedupCount !== undefined && result.dedupCount > 0 && (
            <>
              <span className="w-1 h-1 rounded-full bg-hp-border"></span>
              <div className="flex items-center gap-1.5">
                <span className="text-hp-accent font-bold">{result.dedupCount}</span> dedup blocks
              </div>
            </>
          )}
          {result.dedupSaved !== undefined && result.dedupSaved > 0 && (
            <>
              <span className="w-1 h-1 rounded-full bg-hp-border"></span>
              <div>Dedup saved: <span className="text-hp-accent font-bold">{formatSize(result.dedupSaved)}</span></div>
            </>
          )}
        </div>
      )}

      {isCompress && result.blocks.length > 0 && (
        <div className="space-y-2">
          <div className="text-sm font-medium text-hp-text">Strategy breakdown:</div>
          <div className="space-y-1.5">
            {Object.entries(
              result.blocks.reduce((acc, b) => {
                acc[b.strategy] = (acc[b.strategy] || 0) + 1;
                return acc;
              }, {} as Record<string, number>)
            ).map(([strategy, count]) => (
              <div key={strategy} className="flex items-center gap-3 text-sm">
                <div className="w-24 text-hp-muted font-mono">{strategy}</div>
                <div className="flex-1 h-2 bg-hp-bg rounded-full overflow-hidden">
                  <div 
                    className="h-full bg-hp-accent rounded-full transition-all duration-1000 ease-out" 
                    style={{ width: `${(count / result.blocks.length) * 100}%` }}
                  />
                </div>
                <div className="w-16 text-right text-hp-muted">{count} block{count > 1 ? 's' : ''}</div>
              </div>
            ))}
          </div>
        </div>
      )}
      
      <div className="flex gap-3 pt-2">
        <button
          onClick={onDownload}
          className="flex-1 flex items-center justify-center gap-2 py-3 bg-hp-accent hover:bg-hp-accent-hover text-white rounded-lg font-medium transition-colors"
        >
          <Download className="w-5 h-5" />
          Download {isCompress ? '.hpk' : ''}
        </button>
        <button
          onClick={onReset}
          className="flex items-center justify-center gap-2 px-6 py-3 bg-hp-hover hover:bg-hp-border text-hp-text rounded-lg font-medium transition-colors"
        >
          <RefreshCw className="w-5 h-5" />
          New file
        </button>
      </div>
    </div>
  );
}
