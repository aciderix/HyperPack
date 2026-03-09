import { useState, useEffect, useRef, useCallback } from 'react';
import { Play, Square, UploadCloud } from 'lucide-react';
import { Header } from './components/Header';
import { DropZone } from './components/DropZone';
import { ModeSelector } from './components/ModeSelector';
import { FileInfo } from './components/FileInfo';
import { ProgressBar } from './components/ProgressBar';
import { ResultPanel } from './components/ResultPanel';
import { CompareChart } from './components/CompareChart';
import { SettingsPanel } from './components/SettingsPanel';
import { useHyperPack } from './hooks/useHyperPack';
import { CompressParams } from './workers/bridge';

const DEFAULT_SETTINGS: CompressParams = {
  blockSizeMB: 8,
};

export default function App() {
  const [isSettingsOpen, setIsSettingsOpen] = useState(false);
  const [mode, setMode] = useState<'compress' | 'decompress'>('compress');
  const [file, setFile] = useState<File | null>(null);
  const [settings, setSettings] = useState<CompressParams>(() => {
    const saved = localStorage.getItem('hyperpack_settings');
    if (saved) {
      try {
        const parsed = JSON.parse(saved);
        // Validate saved settings have the right shape
        if (typeof parsed.blockSizeMB === 'number') return parsed;
      } catch (_) {}
    }
    return DEFAULT_SETTINGS;
  });

  // Page-wide drag overlay
  const [isDraggingPage, setIsDraggingPage] = useState(false);
  const dragCounterRef = useRef(0);

  const { status, progress, result, error, compress, decompress, cancel, reset, download } = useHyperPack();

  // Use refs to avoid stale closures in keyboard handler
  const fileRef = useRef(file);
  const modeRef = useRef(mode);
  const settingsRef = useRef(settings);
  const statusRef = useRef(status);
  fileRef.current = file;
  modeRef.current = mode;
  settingsRef.current = settings;
  statusRef.current = status;

  useEffect(() => {
    localStorage.setItem('hyperpack_settings', JSON.stringify(settings));
  }, [settings]);

  // Keyboard shortcuts — uses refs to avoid stale closures
  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && statusRef.current === 'processing') {
        cancel();
      } else if (e.key === 'Enter' && fileRef.current && statusRef.current === 'idle') {
        if (modeRef.current === 'compress') {
          compress(fileRef.current, settingsRef.current);
        } else {
          decompress(fileRef.current);
        }
      }
    };
    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [cancel, compress, decompress]);

  const handleFileSelect = useCallback((selectedFile: File) => {
    setFile(selectedFile);
    reset();
    if (selectedFile.name.endsWith('.hpk')) {
      setMode('decompress');
    }
    setIsDraggingPage(false);
    dragCounterRef.current = 0;
  }, [reset]);

  const handleClearFile = useCallback(() => {
    setFile(null);
    reset();
  }, [reset]);

  const handleStart = () => {
    if (!file) return;
    if (mode === 'compress') {
      compress(file, settings);
    } else {
      decompress(file);
    }
  };

  // Page-wide drag handlers
  const handlePageDragEnter = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    dragCounterRef.current++;
    if (dragCounterRef.current === 1 && status === 'idle') {
      setIsDraggingPage(true);
    }
  }, [status]);

  const handlePageDragLeave = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    dragCounterRef.current--;
    if (dragCounterRef.current === 0) {
      setIsDraggingPage(false);
    }
  }, []);

  const handlePageDragOver = useCallback((e: React.DragEvent) => {
    e.preventDefault();
  }, []);

  const handlePageDrop = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    dragCounterRef.current = 0;
    setIsDraggingPage(false);
    if (e.dataTransfer.files && e.dataTransfer.files.length > 0) {
      handleFileSelect(e.dataTransfer.files[0]);
    }
  }, [handleFileSelect]);

  return (
    <div 
      className="min-h-screen bg-hp-bg text-hp-text p-4 md:p-8 font-sans relative"
      onDragEnter={handlePageDragEnter}
      onDragLeave={handlePageDragLeave}
      onDragOver={handlePageDragOver}
      onDrop={handlePageDrop}
    >
      {/* Page-wide drag overlay */}
      {isDraggingPage && (
        <div className="drag-overlay fixed inset-0 z-40 bg-hp-bg/80 backdrop-blur-sm border-4 border-dashed border-hp-accent rounded-xl flex flex-col items-center justify-center pointer-events-none">
          <UploadCloud className="w-16 h-16 text-hp-accent mb-4" />
          <p className="text-xl font-bold text-hp-text">Drop your file anywhere</p>
          <p className="text-hp-muted mt-1">Release to load the file</p>
        </div>
      )}

      <div className="max-w-2xl mx-auto space-y-6">
        <div className="bg-hp-card rounded-xl shadow-2xl border border-hp-border overflow-hidden">
          <Header onOpenSettings={() => setIsSettingsOpen(true)} />
          
          <div className="p-6 space-y-6">
            <ModeSelector 
              mode={mode} 
              onChange={setMode} 
              disabled={status === 'processing'} 
            />

            {status === 'idle' && !file && (
              <DropZone onFileSelect={handleFileSelect} />
            )}

            {file && status === 'idle' && (
              <FileInfo file={file} onClear={handleClearFile} />
            )}

            {status === 'processing' && progress && (
              <ProgressBar progress={progress} mode={mode} />
            )}

            {status === 'complete' && result && (
              <ResultPanel 
                result={result} 
                mode={mode} 
                onDownload={download} 
                onReset={handleClearFile} 
              />
            )}

            {error && (
              <div className="p-4 bg-hp-error/10 border border-hp-error/20 rounded-xl text-hp-error text-sm">
                Error: {error}
              </div>
            )}

            {(status === 'idle' || status === 'processing') && (
              <button
                onClick={status === 'processing' ? cancel : handleStart}
                disabled={!file && status === 'idle'}
                className={`w-full flex items-center justify-center gap-2 py-4 rounded-xl font-bold text-lg transition-all duration-200
                  ${status === 'processing' 
                    ? 'bg-hp-hover text-hp-error hover:bg-hp-error/20 border border-hp-error/30' 
                    : !file 
                      ? 'bg-hp-hover text-hp-muted cursor-not-allowed' 
                      : 'bg-hp-accent hover:bg-hp-accent-hover text-white shadow-lg shadow-hp-accent/20'
                  }`}
              >
                {status === 'processing' ? (
                  <>
                    <Square className="w-5 h-5 fill-current" />
                    Cancel (Esc)
                  </>
                ) : (
                  <>
                    <Play className="w-5 h-5 fill-current" />
                    {mode === 'compress' ? 'Compress' : 'Decompress'} (Enter)
                  </>
                )}
              </button>
            )}
          </div>
        </div>

        {status === 'complete' && result && mode === 'compress' && (
          <CompareChart ratio={result.ratio} />
        )}
      </div>

      <SettingsPanel 
        isOpen={isSettingsOpen} 
        onClose={() => setIsSettingsOpen(false)} 
        settings={settings}
        onSettingsChange={setSettings}
      />
    </div>
  );
}
