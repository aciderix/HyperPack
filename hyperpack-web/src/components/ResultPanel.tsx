import { useEffect, useState, useRef } from 'react';
import { Download, RefreshCw, CheckCircle2, FolderOpen, FolderDown, FileIcon, ChevronDown, ChevronUp } from 'lucide-react';
import { HyperPackResult, ExtractedFile } from '../hooks/useHyperPack';
import * as native from '../lib/native';

interface ResultPanelProps {
  result: HyperPackResult;
  mode: 'compress' | 'decompress';
  onDownload: () => void;
  onReset: () => void;
  extractedFiles?: ExtractedFile[] | null;
  onDownloadFile?: (index: number) => void;
  onSaveToFolder?: () => Promise<boolean>;
  hasFSAccess?: boolean;
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

function FileListPanel({ files, onDownloadFile }: { files: ExtractedFile[]; onDownloadFile?: (i: number) => void }) {
  const [expanded, setExpanded] = useState(false);
  const PREVIEW_COUNT = 5;
  const showToggle = files.length > PREVIEW_COUNT;
  const visibleFiles = expanded ? files : files.slice(0, PREVIEW_COUNT);

  return (
    <div className="space-y-2">
      <div className="text-sm font-medium text-hp-text">
        Extracted files ({files.length}):
      </div>
      <div className="max-h-64 overflow-y-auto space-y-1 bg-hp-bg rounded-lg border border-hp-border p-2">
        {visibleFiles.map((file, i) => (
          <div key={i} className="flex items-center gap-2 text-sm py-1 px-2 rounded hover:bg-hp-hover group">
            <FileIcon className="w-3.5 h-3.5 text-hp-muted flex-shrink-0" />
            <span className="flex-1 text-hp-text truncate font-mono text-xs" title={file.name}>
              {file.name}
            </span>
            <span className="text-hp-muted text-xs flex-shrink-0">{formatSize(file.size)}</span>
            {onDownloadFile && (
              <button
                onClick={() => onDownloadFile(i)}
                className="opacity-0 group-hover:opacity-100 transition-opacity text-hp-accent hover:text-hp-accent-hover flex-shrink-0"
                title="Download this file"
              >
                <Download className="w-3.5 h-3.5" />
              </button>
            )}
          </div>
        ))}
      </div>
      {showToggle && (
        <button
          onClick={() => setExpanded(!expanded)}
          className="flex items-center gap-1 text-xs text-hp-accent hover:text-hp-accent-hover transition-colors"
        >
          {expanded ? <ChevronUp className="w-3 h-3" /> : <ChevronDown className="w-3 h-3" />}
          {expanded ? 'Show less' : `Show all ${files.length} files`}
        </button>
      )}
    </div>
  );
}

export function ResultPanel({ result, mode, onDownload, onReset, extractedFiles, onDownloadFile, onSaveToFolder, hasFSAccess }: ResultPanelProps) {
  const isCompress = mode === 'compress';
  const isMultiFile = extractedFiles && extractedFiles.length > 0;
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);
  
  const animatedRatio = useCountUp(result.ratio, 1000);
  const animatedInput = useCountUp(result.inputSize, 800);
  const animatedOutput = useCountUp(result.outputSize, 800);
  const animatedTime = useCountUp(result.totalMs / 1000, 600);
  const animatedSpeed = useCountUp(
    (result.inputSize / (1024 * 1024)) / (result.totalMs / 1000), 
    800
  );

  const handleSaveToFolder = async () => {
    if (!onSaveToFolder) return;
    setSaving(true);
    try {
      const ok = await onSaveToFolder();
      if (ok) setSaved(true);
    } catch (err) {
      console.error('Save to folder failed:', err);
    } finally {
      setSaving(false);
    }
  };
  
  return (
    <div className="p-6 bg-hp-card border border-hp-border rounded-xl space-y-6 animate-fade-in">
      <div className="flex items-center gap-3 text-hp-success">
        <CheckCircle2 className="w-6 h-6" />
        <h3 className="text-lg font-medium">
          {isCompress ? 'Compression' : 'Decompression'} complete!
          {isCompress && result.fileCount && result.fileCount > 1 && (
            <span className="text-sm font-normal text-hp-muted ml-2">({result.fileCount} files → HPK6)</span>
          )}
          {!isCompress && isMultiFile && (
            <span className="text-sm font-normal text-hp-muted ml-2">({extractedFiles.length} files extracted)</span>
          )}
        </h3>
      </div>
      
      <div className="flex items-center justify-center gap-6 py-4 bg-hp-bg rounded-lg border border-hp-border">
        <div className="text-center">
          <div className="text-2xl font-bold text-hp-text tabular-nums">{formatSize(Math.round(animatedInput))}</div>
          <div className="text-sm text-hp-muted mt-1">{isCompress ? 'Original' : 'Compressed'}</div>
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
      
      {/* Archive info (HPK6 compression) */}
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

      {/* Extracted files list (HPK6 decompression) */}
      {isMultiFile && (
        <FileListPanel files={extractedFiles} onDownloadFile={onDownloadFile} />
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
        {result.outputPath ? (
          /* Native mode: reveal output in file manager */
          <button
            onClick={() => native.revealInFinder(result.outputPath!)}
            className="flex-1 flex items-center justify-center gap-2 py-3 bg-hp-accent hover:bg-hp-accent-hover text-white rounded-lg font-medium transition-colors"
          >
            <FolderOpen className="w-5 h-5" />
            Show in {navigator.userAgent.includes('Mac') ? 'Finder' : 'Explorer'}
          </button>
        ) : isMultiFile ? (
          /* Multi-file: Save to Folder with File System Access API */
          <div className="flex-1 flex gap-2">
            {hasFSAccess && (
              <button
                onClick={handleSaveToFolder}
                disabled={saving || saved}
                className={`flex-1 flex items-center justify-center gap-2 py-3 rounded-lg font-medium transition-colors ${
                  saved
                    ? 'bg-hp-success/20 text-hp-success border border-hp-success/30'
                    : 'bg-hp-accent hover:bg-hp-accent-hover text-white'
                }`}
              >
                {saved ? (
                  <>
                    <CheckCircle2 className="w-5 h-5" />
                    Saved!
                  </>
                ) : saving ? (
                  <>
                    <FolderDown className="w-5 h-5 animate-pulse" />
                    Saving...
                  </>
                ) : (
                  <>
                    <FolderDown className="w-5 h-5" />
                    Save to folder
                  </>
                )}
              </button>
            )}
            {!hasFSAccess && (
              <div className="flex-1 text-center text-sm text-hp-muted py-3">
                Download files individually from the list above
              </div>
            )}
          </div>
        ) : (
          /* Web / WASM mode: download single file */
          <button
            onClick={onDownload}
            className="flex-1 flex items-center justify-center gap-2 py-3 bg-hp-accent hover:bg-hp-accent-hover text-white rounded-lg font-medium transition-colors"
          >
            <Download className="w-5 h-5" />
            Download {isCompress ? '.hpk' : ''}
          </button>
        )}
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
