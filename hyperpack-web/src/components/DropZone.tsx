import { useCallback, useState, useRef } from 'react';
import { UploadCloud, FolderOpen } from 'lucide-react';
import { FileEntry } from '../workers/bridge';
import * as native from '../lib/native';

interface DropZoneProps {
  onFiles: (files: FileEntry[]) => void;
}

/** Recursively read a FileSystemDirectoryEntry */
function readEntryRecursive(entry: FileSystemEntry, basePath: string): Promise<FileEntry[]> {
  return new Promise((resolve) => {
    if (entry.isFile) {
      (entry as FileSystemFileEntry).file((file) => {
        file.arrayBuffer().then((data) => {
          const relativePath = basePath ? basePath + '/' + file.name : file.name;
          resolve([{ name: relativePath, data, size: file.size }]);
        });
      });
    } else if (entry.isDirectory) {
      const reader = (entry as FileSystemDirectoryEntry).createReader();
      const allEntries: FileSystemEntry[] = [];

      const readBatch = () => {
        reader.readEntries((entries) => {
          if (entries.length === 0) {
            const dirPath = basePath ? basePath + '/' + entry.name : entry.name;
            Promise.all(allEntries.map((e) => readEntryRecursive(e, dirPath))).then((results) => {
              resolve(results.flat());
            });
          } else {
            allEntries.push(...entries);
            readBatch(); // Keep reading until no more entries
          }
        });
      };
      readBatch();
    } else {
      resolve([]);
    }
  });
}

/** Convert a FileList (from input) to FileEntry[], preserving webkitRelativePath */
async function fileListToEntries(fileList: FileList): Promise<FileEntry[]> {
  const entries: FileEntry[] = [];
  for (let i = 0; i < fileList.length; i++) {
    const file = fileList[i];
    const data = await file.arrayBuffer();
    // webkitRelativePath is "folderName/sub/file.txt" — use it if available
    const name = (file as File & { webkitRelativePath?: string }).webkitRelativePath || file.name;
    entries.push({ name, data, size: file.size });
  }
  return entries;
}

export function DropZone({ onFiles }: DropZoneProps) {
  const [isDragging, setIsDragging] = useState(false);
  const fileInputRef = useRef<HTMLInputElement>(null);
  const folderInputRef = useRef<HTMLInputElement>(null);
  const isNativeMode = native.isNative();

  // Native: open file dialog and build FileEntry with path (no data read)
  const handleNativeBrowseFiles = useCallback(async () => {
    const paths = await native.openFilePicker({ multiple: true });
    if (!paths || paths.length === 0) return;
    const entries: FileEntry[] = paths.map((p) => ({
      name: p.split(/[\\/]/).pop() ?? p,
      data: new ArrayBuffer(0),
      size: 0, // size not known without stat; will be reported by native engine
      path: p,
    }));
    onFiles(entries);
  }, [onFiles]);

  const handleNativeBrowseFolder = useCallback(async () => {
    const path = await native.openFolderPicker();
    if (!path) return;
    const entry: FileEntry = {
      name: path.split(/[\\/]/).pop() ?? path,
      data: new ArrayBuffer(0),
      size: 0,
      path,
    };
    onFiles([entry]);
  }, [onFiles]);

  const handleDragOver = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    setIsDragging(true);
  }, []);

  const handleDragLeave = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    setIsDragging(false);
  }, []);

  const handleDrop = useCallback(async (e: React.DragEvent) => {
    e.preventDefault();
    setIsDragging(false);

    const items = e.dataTransfer.items;
    if (items && items.length > 0) {
      // Try to use webkitGetAsEntry for directory support
      const entryPromises: Promise<FileEntry[]>[] = [];
      let hasEntries = false;

      for (let i = 0; i < items.length; i++) {
        const entry = items[i].webkitGetAsEntry?.();
        if (entry) {
          hasEntries = true;
          entryPromises.push(readEntryRecursive(entry, ''));
        }
      }

      if (hasEntries) {
        const results = await Promise.all(entryPromises);
        const allFiles = results.flat();
        if (allFiles.length > 0) {
          onFiles(allFiles);
          return;
        }
      }
    }

    // Fallback: use dataTransfer.files
    if (e.dataTransfer.files && e.dataTransfer.files.length > 0) {
      const entries = await fileListToEntries(e.dataTransfer.files);
      if (entries.length > 0) {
        onFiles(entries);
      }
    }
  }, [onFiles]);

  const handleFileInput = useCallback(async (e: React.ChangeEvent<HTMLInputElement>) => {
    if (e.target.files && e.target.files.length > 0) {
      const entries = await fileListToEntries(e.target.files);
      if (entries.length > 0) {
        onFiles(entries);
      }
    }
  }, [onFiles]);

  const handleFolderInput = useCallback(async (e: React.ChangeEvent<HTMLInputElement>) => {
    if (e.target.files && e.target.files.length > 0) {
      const entries = await fileListToEntries(e.target.files);
      if (entries.length > 0) {
        onFiles(entries);
      }
    }
  }, [onFiles]);

  // Native mode: simple dialog buttons (no drag-drop needed)
  if (isNativeMode) {
    return (
      <div className="relative flex flex-col items-center justify-center p-12 border-2 border-dashed rounded-xl border-hp-border bg-hp-card">
        <UploadCloud className="w-12 h-12 mb-4 text-hp-muted" />
        <h3 className="text-lg font-medium text-hp-text mb-1">Select files to compress or decompress</h3>
        <p className="text-hp-muted mb-6">Native mode — no size limit, fully multi-threaded</p>
        <div className="flex gap-3">
          <button
            type="button"
            onClick={handleNativeBrowseFiles}
            className="flex items-center gap-2 px-5 py-2.5 text-sm font-medium text-white bg-hp-accent hover:bg-hp-accent-hover rounded-lg transition-colors"
          >
            <UploadCloud className="w-4 h-4" />
            Browse files
          </button>
          <button
            type="button"
            onClick={handleNativeBrowseFolder}
            className="flex items-center gap-2 px-5 py-2.5 text-sm font-medium text-hp-accent bg-hp-accent/10 hover:bg-hp-accent/20 border border-hp-accent/30 rounded-lg transition-colors"
          >
            <FolderOpen className="w-4 h-4" />
            Browse folder (HPK6)
          </button>
        </div>
        <p className="text-xs text-hp-muted/70 text-center mt-4">
          Single file → HPK5 &nbsp;|&nbsp; Folder / multiple files → HPK6 Archive
        </p>
      </div>
    );
  }

  // Web / WASM mode: standard drag-drop
  return (
    <div
      className={`relative flex flex-col items-center justify-center p-12 border-2 border-dashed rounded-xl transition-all duration-300 cursor-pointer
        ${isDragging ? 'border-hp-accent bg-hp-accent/10 scale-[1.02]' : 'border-hp-border bg-hp-card hover:border-hp-muted hover:bg-hp-hover'}`}
      onDragOver={handleDragOver}
      onDragLeave={handleDragLeave}
      onDrop={handleDrop}
      onClick={() => fileInputRef.current?.click()}
    >
      <input
        type="file"
        className="hidden"
        ref={fileInputRef}
        onChange={handleFileInput}
        multiple
      />
      <input
        type="file"
        className="hidden"
        ref={folderInputRef}
        onChange={handleFolderInput}
        // eslint-disable-next-line @typescript-eslint/ban-ts-comment
        {...{ webkitdirectory: '', directory: '' } as Record<string, string>}
        multiple
      />
      <UploadCloud className={`w-12 h-12 mb-4 transition-colors duration-300 ${isDragging ? 'text-hp-accent' : 'text-hp-muted'}`} />
      <h3 className="text-lg font-medium text-hp-text mb-1">Drop files or folder here</h3>
      <p className="text-hp-muted mb-4">or click to browse files</p>

      <button
        type="button"
        onClick={(e) => {
          e.stopPropagation();
          folderInputRef.current?.click();
        }}
        className="flex items-center gap-2 px-4 py-2 mb-4 text-sm font-medium text-hp-accent bg-hp-accent/10 hover:bg-hp-accent/20 border border-hp-accent/30 rounded-lg transition-colors"
      >
        <FolderOpen className="w-4 h-4" />
        Select Folder (HPK6 Archive)
      </button>

      <div className="text-xs text-hp-muted/70 text-center space-y-1">
        <p>Single file → HPK5 &nbsp;|&nbsp; Multiple files / folder → HPK6 Archive</p>
        <p>Max recommended: 512 MB total</p>
      </div>
    </div>
  );
}
