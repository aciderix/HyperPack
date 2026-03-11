import { useState, useMemo, useCallback } from 'react';
import {
  File as FileIcon,
  Folder,
  FolderOpen,
  Download,
  FolderOutput,
  Archive,
  Search,
  ChevronRight,
  ChevronDown,
} from 'lucide-react';
import { ExtractedFile } from '../hooks/useHyperPack';

interface ExtractedFilesPanelProps {
  files: ExtractedFile[];
  onDownloadFile: (index: number) => void;
  onExtractAll: () => Promise<boolean>;
  onDownloadZip: () => void;
}

function formatSize(bytes: number) {
  if (bytes === 0) return '0 B';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

interface TreeNode {
  name: string;
  fullPath: string;
  isDir: boolean;
  size: number;
  fileIndex: number; // -1 for directories
  children: TreeNode[];
}

function buildTree(files: ExtractedFile[]): TreeNode[] {
  const root: TreeNode[] = [];
  const dirMap = new Map<string, TreeNode>();

  for (let i = 0; i < files.length; i++) {
    const file = files[i];
    const parts = file.name.split('/');

    let currentChildren = root;
    let currentPath = '';

    for (let j = 0; j < parts.length; j++) {
      const part = parts[j];
      const isLast = j === parts.length - 1;
      currentPath = currentPath ? currentPath + '/' + part : part;

      if (isLast) {
        // File node
        currentChildren.push({
          name: part,
          fullPath: currentPath,
          isDir: false,
          size: file.size,
          fileIndex: i,
          children: [],
        });
      } else {
        // Directory node
        let dirNode = dirMap.get(currentPath);
        if (!dirNode) {
          dirNode = {
            name: part,
            fullPath: currentPath,
            isDir: true,
            size: 0,
            fileIndex: -1,
            children: [],
          };
          dirMap.set(currentPath, dirNode);
          currentChildren.push(dirNode);
        }
        currentChildren = dirNode.children;
      }
    }
  }

  // Calculate directory sizes
  function calcDirSize(node: TreeNode): number {
    if (!node.isDir) return node.size;
    let total = 0;
    for (const child of node.children) {
      total += calcDirSize(child);
    }
    node.size = total;
    return total;
  }
  for (const node of root) calcDirSize(node);

  // Sort: dirs first, then files, alphabetically
  function sortTree(nodes: TreeNode[]) {
    nodes.sort((a, b) => {
      if (a.isDir !== b.isDir) return a.isDir ? -1 : 1;
      return a.name.localeCompare(b.name);
    });
    for (const n of nodes) if (n.isDir) sortTree(n.children);
  }
  sortTree(root);

  return root;
}

function TreeRow({
  node,
  depth,
  expanded,
  onToggle,
  onDownload,
  searchQuery,
}: {
  node: TreeNode;
  depth: number;
  expanded: Set<string>;
  onToggle: (path: string) => void;
  onDownload: (index: number) => void;
  searchQuery: string;
}) {
  const isExpanded = expanded.has(node.fullPath);
  const matchesSearch = !searchQuery ||
    node.name.toLowerCase().includes(searchQuery.toLowerCase());

  // For directories, check if any child matches
  const hasMatchingChild = useMemo(() => {
    if (!searchQuery) return true;
    if (!node.isDir) return matchesSearch;
    function checkChildren(n: TreeNode): boolean {
      if (n.name.toLowerCase().includes(searchQuery.toLowerCase())) return true;
      return n.children.some(checkChildren);
    }
    return checkChildren(node);
  }, [node, searchQuery, matchesSearch]);

  if (!matchesSearch && !hasMatchingChild) return null;

  return (
    <>
      <div
        className={`flex items-center gap-2 py-1.5 px-2 rounded-lg cursor-pointer transition-colors
          ${node.isDir ? 'hover:bg-hp-hover' : 'hover:bg-hp-hover group'}`}
        style={{ paddingLeft: `${depth * 20 + 8}px` }}
        onClick={() => {
          if (node.isDir) {
            onToggle(node.fullPath);
          } else {
            onDownload(node.fileIndex);
          }
        }}
      >
        {/* Expand/collapse icon for dirs */}
        {node.isDir ? (
          <span className="w-4 h-4 flex items-center justify-center text-hp-muted">
            {isExpanded ? <ChevronDown className="w-3.5 h-3.5" /> : <ChevronRight className="w-3.5 h-3.5" />}
          </span>
        ) : (
          <span className="w-4" />
        )}

        {/* File/folder icon */}
        {node.isDir ? (
          isExpanded ? <FolderOpen className="w-4 h-4 text-hp-accent" /> : <Folder className="w-4 h-4 text-hp-accent" />
        ) : (
          <FileIcon className="w-4 h-4 text-hp-muted" />
        )}

        {/* Name */}
        <span className={`flex-1 text-sm truncate ${node.isDir ? 'font-medium text-hp-text' : 'text-hp-text'}`}>
          {node.name}
        </span>

        {/* Size */}
        <span className="text-xs text-hp-muted tabular-nums whitespace-nowrap">
          {formatSize(node.size)}
        </span>

        {/* Download button (files only, visible on hover) */}
        {!node.isDir && (
          <Download className="w-3.5 h-3.5 text-hp-muted opacity-0 group-hover:opacity-100 transition-opacity" />
        )}
      </div>

      {/* Children */}
      {node.isDir && isExpanded && node.children.map((child) => (
        <TreeRow
          key={child.fullPath}
          node={child}
          depth={depth + 1}
          expanded={expanded}
          onToggle={onToggle}
          onDownload={onDownload}
          searchQuery={searchQuery}
        />
      ))}
    </>
  );
}

export function ExtractedFilesPanel({
  files,
  onDownloadFile,
  onExtractAll,
  onDownloadZip,
}: ExtractedFilesPanelProps) {
  const [searchQuery, setSearchQuery] = useState('');
  const [expanded, setExpanded] = useState<Set<string>>(() => {
    // Expand all directories by default
    const dirs = new Set<string>();
    for (const file of files) {
      const parts = file.name.split('/');
      let path = '';
      for (let i = 0; i < parts.length - 1; i++) {
        path = path ? path + '/' + parts[i] : parts[i];
        dirs.add(path);
      }
    }
    return dirs;
  });
  const [extracting, setExtracting] = useState(false);
  const [extractSuccess, setExtractSuccess] = useState(false);

  const supportsDirectoryPicker = 'showDirectoryPicker' in window;

  const tree = useMemo(() => buildTree(files), [files]);
  const totalSize = useMemo(() => files.reduce((sum, f) => sum + f.size, 0), [files]);

  const handleToggle = useCallback((path: string) => {
    setExpanded((prev) => {
      const next = new Set(prev);
      if (next.has(path)) {
        next.delete(path);
      } else {
        next.add(path);
      }
      return next;
    });
  }, []);

  const handleExtractAll = useCallback(async () => {
    setExtracting(true);
    try {
      const ok = await onExtractAll();
      if (ok) {
        setExtractSuccess(true);
        setTimeout(() => setExtractSuccess(false), 3000);
      }
    } finally {
      setExtracting(false);
    }
  }, [onExtractAll]);

  return (
    <div className="bg-hp-card border border-hp-border rounded-xl overflow-hidden animate-fade-in">
      {/* Header */}
      <div className="p-4 border-b border-hp-border space-y-3">
        <div className="flex items-center justify-between">
          <div className="text-sm text-hp-muted">
            <span className="text-hp-accent font-bold">{files.length}</span> files •{' '}
            <span className="font-medium">{formatSize(totalSize)}</span> total
          </div>
        </div>

        {/* Action buttons */}
        <div className="flex gap-2">
          {supportsDirectoryPicker && (
            <button
              onClick={handleExtractAll}
              disabled={extracting}
              className={`flex-1 flex items-center justify-center gap-2 py-2.5 rounded-lg text-sm font-medium transition-colors
                ${extractSuccess
                  ? 'bg-hp-success/20 text-hp-success border border-hp-success/30'
                  : 'bg-hp-accent hover:bg-hp-accent-hover text-white'
                }`}
            >
              <FolderOutput className="w-4 h-4" />
              {extracting ? 'Extracting...' : extractSuccess ? 'Extracted ✓' : 'Extract to Folder'}
            </button>
          )}
          <button
            onClick={onDownloadZip}
            className={`flex items-center justify-center gap-2 py-2.5 px-4 rounded-lg text-sm font-medium transition-colors
              ${supportsDirectoryPicker
                ? 'bg-hp-hover hover:bg-hp-border text-hp-text'
                : 'flex-1 bg-hp-accent hover:bg-hp-accent-hover text-white'
              }`}
          >
            <Archive className="w-4 h-4" />
            Download ZIP
          </button>
        </div>

        {/* Search */}
        {files.length > 10 && (
          <div className="relative">
            <Search className="absolute left-3 top-1/2 -translate-y-1/2 w-4 h-4 text-hp-muted" />
            <input
              type="text"
              placeholder="Search files..."
              value={searchQuery}
              onChange={(e) => setSearchQuery(e.target.value)}
              className="w-full pl-9 pr-3 py-2 bg-hp-bg border border-hp-border rounded-lg text-sm text-hp-text placeholder-hp-muted focus:outline-none focus:border-hp-accent transition-colors"
            />
          </div>
        )}
      </div>

      {/* File tree */}
      <div className="max-h-80 overflow-y-auto p-2">
        {tree.map((node) => (
          <TreeRow
            key={node.fullPath}
            node={node}
            depth={0}
            expanded={expanded}
            onToggle={handleToggle}
            onDownload={onDownloadFile}
            searchQuery={searchQuery}
          />
        ))}
      </div>
    </div>
  );
}
