import { Settings, Trophy } from 'lucide-react';

export function Header({ onOpenSettings }: { onOpenSettings: () => void }) {
  return (
    <header className="flex items-center justify-between p-4 bg-hp-card border-b border-hp-border rounded-t-xl">
      <div className="flex items-center gap-2">
        <Trophy className="w-6 h-6 text-hp-accent" />
        <h1 className="text-xl font-bold text-hp-text tracking-tight">HyperPack <span className="text-sm font-normal text-hp-muted ml-1">v11</span></h1>
      </div>
      <button 
        onClick={onOpenSettings}
        className="flex items-center gap-2 px-3 py-1.5 rounded-lg hover:bg-hp-hover text-hp-muted hover:text-hp-text transition-colors"
      >
        <Settings className="w-4 h-4" />
        <span className="text-sm font-medium">Settings</span>
      </button>
    </header>
  );
}
