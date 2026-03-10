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
import { CompressParams, FileEntry } from './workers/bridge';

const DEFAULT_SETTINGS: CompressParams = {
  blockSizeMB: 8,
  archiveMode: false,
  nthreads: 0, // 0 = auto (only meaningful in native mode)
};

export default function App() {
  const [isSettingsOpen, setIsSettingsOpen] = useState(false);
  const [mode, setMode] = useState<'compress' | 'decompress'>('compress');
  const [files, setFiles] = useState<FileEntry[]>([]);
  const [decompressFile, setDecompressFile] = useState<File | null>(null);
  const [settings, setSettings] = useState<CompressParams>(() => {
    const saved = localStorage.getItem('hyperpack_settings');
    if (saved) {
      try {
        const parsed = JSON.parse(saved);
        if (typeof parsed.blockSizeMB === 'number') return { ...DEFAULT_SETTINGS, ...parsed };
      } catch (_) {}
    }
    return DEFAULT_SETTINGS;
  });

  // Page-wide drag overlay
  const [isDraggingPage, setIsDraggingPage] = useState(false);
  const dragCounterRef = useRef(0);

  const { status, progress, result, error, compress, decompress, cancel, reset, download } = useHyperPack();

  // Auto-detect archive mode based on file count
  const archiveMode = files.length > 1;

  // Use refs to avoid stale closures in keyboard handler
  const filesRef = useRef(files);
  const decompressFileRef = useRef(decompressFile);
  const modeRef = useRef(mode);
  const settingsRef = useRef(settings);
  const statusRef = useRef(status);
  const archiveModeRef = useRef(archiveMode);
  filesRef.current = files;
  decompressFileRef.current = decompressFile;
  modeRef.current = mode;
  settingsRef.current = settings;
  statusRef.current = status;
  archiveModeRef.current = archiveMode;

  useEffect(() => {
    localStorage.setItem('hyperpack_settings', JSON.stringify(settings));
  }, [settings]);

  // Keyboard shortcuts — uses refs to avoid stale closures
  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && statusRef.current === 'processing') {
        cancel();
      } else if (e.key === 'Enter' && statusRef.current === 'idle') {
        if (modeRef.current === 'compress' && filesRef.current.length > 0) {
          compress(filesRef.current, { ...settingsRef.current, archiveMode: archiveModeRef.current });
        } else if (modeRef.current === 'decompress' && decompressFileRef.current) {
          decompress(decompressFileRef.current);
        }
      }
    };
    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [cancel, compress, decompress]);

  const handleFilesSelect = useCallback((selectedFiles: FileEntry[]) => {
    setFiles(selectedFiles);
    setDecompressFile(null);
    reset();
    // Auto-detect: if single .hpk file, switch to decompress mode
    if (selectedFiles.length === 1 && selectedFiles[0].name.endsWith('.hpk')) {
      setMode('decompress');
      // Create a File object for the decompress path
      const entry = selectedFiles[0];
      const blob = new Blob([entry.data]);
      const file = new File([blob], entry.name);
      setDecompressFile(file);
    }
    setIsDraggingPage(false);
    dragCounterRef.current = 0;
  }, [reset]);

  const handleClearFile = useCallback(() => {
    setFiles([]);
    setDecompressFile(null);
    reset();
  }, [reset]);

  const handleStart = () => {
    if (mode === 'compress' && files.length > 0) {
      compress(files, { ...settings, archiveMode });
    } else if (mode === 'decompress' && decompressFile) {
      decompress(decompressFile);
    }
  };

  const hasFiles = files.length > 0;

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

  const handlePageDrop = useCallback(async (e: React.DragEvent) => {
    e.preventDefault();
    dragCounterRef.current = 0;
    setIsDraggingPage(false);
    // Let the DropZone handle file processing; this is just for the overlay
    // Files dropped on the page overlay but outside DropZone:
    if (e.dataTransfer.files && e.dataTransfer.files.length > 0) {
      const entries: FileEntry[] = [];
      for (let i = 0; i < e.dataTransfer.files.length; i++) {
        const f = e.dataTransfer.files[i];
        const data = await f.arrayBuffer();
        entries.push({ name: f.name, data, size: f.size });
      }
      if (entries.length > 0) {
        handleFilesSelect(entries);
      }
    }
  }, [handleFilesSelect]);

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
          <p className="text-xl font-bold text-hp-text">Drop your files anywhere</p>
          <p className="text-hp-muted mt-1">Release to load files</p>
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

            {status === 'idle' && !hasFiles && (
              <DropZone onFiles={handleFilesSelect} />
            )}

            {hasFiles && status === 'idle' && (
              <FileInfo files={files} onClear={handleClearFile} />
            )}

            {status === 'processing' && !progress && (
              <div className="p-5 bg-hp-card border border-hp-border rounded-xl">
                <div className="flex justify-between items-center mb-3">
                  <h4 className="text-hp-text font-medium">
                    {mode === 'compress' ? 'Compressing' : 'Decompressing'}...
                  </h4>
                  <span className="text-sm text-hp-muted">Initializing…</span>
                </div>
                <div className="h-3 w-full bg-hp-bg rounded-full overflow-hidden">
                  <div className="h-full progress-shimmer rounded-full w-full animate-pulse" />
                </div>
              </div>
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
                disabled={!hasFiles && status === 'idle'}
                className={`w-full flex items-center justify-center gap-2 py-4 rounded-xl font-bold text-lg transition-all duration-200
                  ${status === 'processing' 
                    ? 'bg-hp-hover text-hp-error hover:bg-hp-error/20 border border-hp-error/30' 
                    : !hasFiles 
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
                    {mode === 'compress' ? (archiveMode ? 'Compress Archive (HPK6)' : 'Compress (HPK5)') : 'Decompress'} (Enter)
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
