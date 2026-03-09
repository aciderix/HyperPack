import { File as FileIcon, Archive, X } from 'lucide-react';
import { FileEntry } from '../workers/bridge';

interface FileInfoProps {
  files: FileEntry[];
  onClear: () => void;
}

function formatSize(bytes: number) {
  if (bytes === 0) return '0 B';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

export function FileInfo({ files, onClear }: FileInfoProps) {
  const totalSize = files.reduce((sum, f) => sum + f.size, 0);
  const isMultiple = files.length > 1;
  const isHpk = files.length === 1 && files[0].name.endsWith('.hpk');
  const modeLabel = isHpk ? 'HyperPack Archive' : isMultiple ? 'HPK6 Archive' : 'HPK5 Single';

  const MAX_SHOWN = 3;
  const shownFiles = files.slice(0, MAX_SHOWN);
  const remaining = files.length - MAX_SHOWN;

  return (
    <div className="flex items-center justify-between p-4 bg-hp-card border border-hp-border rounded-xl">
      <div className="flex items-center gap-4 overflow-hidden">
        <div className="p-3 bg-hp-bg rounded-lg shrink-0">
          {isMultiple ? (
            <Archive className="w-6 h-6 text-hp-accent" />
          ) : (
            <FileIcon className="w-6 h-6 text-hp-accent" />
          )}
        </div>
        <div className="min-w-0">
          {isMultiple ? (
            <>
              <h4 className="text-hp-text font-medium truncate">
                {files.length} files
              </h4>
              <div className="text-xs text-hp-muted mt-0.5 space-y-0.5">
                {shownFiles.map((f, i) => (
                  <div key={i} className="truncate">{f.name} ({formatSize(f.size)})</div>
                ))}
                {remaining > 0 && (
                  <div className="text-hp-accent">... and {remaining} more</div>
                )}
              </div>
            </>
          ) : (
            <h4 className="text-hp-text font-medium truncate">{files[0].name}</h4>
          )}
          <div className="flex items-center gap-3 text-sm text-hp-muted mt-0.5">
            <span>Size: {formatSize(totalSize)}</span>
            <span className="w-1 h-1 rounded-full bg-hp-border"></span>
            <span className="truncate">{modeLabel}</span>
          </div>
        </div>
      </div>
      <button
        onClick={onClear}
        className="p-2 text-hp-muted hover:text-hp-error hover:bg-hp-error/10 rounded-lg transition-colors shrink-0 ml-4"
        title="Clear file"
      >
        <X className="w-5 h-5" />
      </button>
    </div>
  );
}
