import { useCallback, useState, useRef } from 'react';
import { UploadCloud } from 'lucide-react';

interface DropZoneProps {
  onFileSelect: (file: File) => void;
}

export function DropZone({ onFileSelect }: DropZoneProps) {
  const [isDragging, setIsDragging] = useState(false);
  const fileInputRef = useRef<HTMLInputElement>(null);

  const handleDragOver = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    setIsDragging(true);
  }, []);

  const handleDragLeave = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    setIsDragging(false);
  }, []);

  const handleDrop = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    setIsDragging(false);
    if (e.dataTransfer.files && e.dataTransfer.files.length > 0) {
      onFileSelect(e.dataTransfer.files[0]);
    }
  }, [onFileSelect]);

  const handleFileInput = useCallback((e: React.ChangeEvent<HTMLInputElement>) => {
    if (e.target.files && e.target.files.length > 0) {
      onFileSelect(e.target.files[0]);
    }
  }, [onFileSelect]);

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
      />
      <UploadCloud className={`w-12 h-12 mb-4 transition-colors duration-300 ${isDragging ? 'text-hp-accent' : 'text-hp-muted'}`} />
      <h3 className="text-lg font-medium text-hp-text mb-1">Drop file here</h3>
      <p className="text-hp-muted mb-6">or click to browse</p>
      
      <div className="text-xs text-hp-muted/70 text-center space-y-1">
        <p>Supports any file type</p>
        <p>Max recommended: 512 MB</p>
      </div>
    </div>
  );
}
