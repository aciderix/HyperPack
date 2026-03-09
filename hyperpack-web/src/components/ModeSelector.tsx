import { Archive, FolderOpen } from 'lucide-react';

interface ModeSelectorProps {
  mode: 'compress' | 'decompress';
  onChange: (mode: 'compress' | 'decompress') => void;
  disabled?: boolean;
}

export function ModeSelector({ mode, onChange, disabled }: ModeSelectorProps) {
  return (
    <div className="flex p-1 bg-hp-bg rounded-lg border border-hp-border">
      <button
        disabled={disabled}
        onClick={() => onChange('compress')}
        className={`flex-1 flex items-center justify-center gap-2 py-2 px-4 rounded-md text-sm font-medium transition-all duration-200
          ${mode === 'compress' 
            ? 'bg-hp-card text-hp-text shadow-sm' 
            : 'text-hp-muted hover:text-hp-text hover:bg-hp-hover/50'}
          ${disabled ? 'opacity-50 cursor-not-allowed' : ''}`}
      >
        <Archive className="w-4 h-4" />
        Compress
      </button>
      <button
        disabled={disabled}
        onClick={() => onChange('decompress')}
        className={`flex-1 flex items-center justify-center gap-2 py-2 px-4 rounded-md text-sm font-medium transition-all duration-200
          ${mode === 'decompress' 
            ? 'bg-hp-card text-hp-text shadow-sm' 
            : 'text-hp-muted hover:text-hp-text hover:bg-hp-hover/50'}
          ${disabled ? 'opacity-50 cursor-not-allowed' : ''}`}
      >
        <FolderOpen className="w-4 h-4" />
        Decompress
      </button>
    </div>
  );
}
