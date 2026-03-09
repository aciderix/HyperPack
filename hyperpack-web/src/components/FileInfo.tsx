import { File, X } from 'lucide-react';

interface FileInfoProps {
  file: globalThis.File;
  onClear: () => void;
}

function formatSize(bytes: number) {
  if (bytes === 0) return '0 B';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

export function FileInfo({ file, onClear }: FileInfoProps) {
  const isHp = file.name.endsWith('.hpk');
  const typeStr = isHp ? 'HyperPack Archive' : (file.type || 'Binary');

  return (
    <div className="flex items-center justify-between p-4 bg-hp-card border border-hp-border rounded-xl">
      <div className="flex items-center gap-4 overflow-hidden">
        <div className="p-3 bg-hp-bg rounded-lg shrink-0">
          <File className="w-6 h-6 text-hp-accent" />
        </div>
        <div className="min-w-0">
          <h4 className="text-hp-text font-medium truncate">{file.name}</h4>
          <div className="flex items-center gap-3 text-sm text-hp-muted mt-0.5">
            <span>Size: {formatSize(file.size)}</span>
            <span className="w-1 h-1 rounded-full bg-hp-border"></span>
            <span className="truncate">Type: {typeStr}</span>
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
