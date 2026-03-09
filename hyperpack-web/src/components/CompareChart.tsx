interface CompareChartProps {
  ratio: number;
}

export function CompareChart({ ratio }: CompareChartProps) {
  // Mock estimations based on prompt
  const xzRatio = ratio * 0.93; // HP is ~7% better
  const bzip2Ratio = ratio * 0.88; // HP is ~12% better
  const gzipRatio = ratio * 0.56; // HP is ~44% better
  
  const maxRatio = Math.max(ratio, xzRatio, bzip2Ratio, gzipRatio);
  
  const items = [
    { name: 'HP v11', ratio, color: 'bg-hp-accent', isWinner: true },
    { name: 'xz -9', ratio: xzRatio, color: 'bg-hp-muted', isWinner: false },
    { name: 'bzip2', ratio: bzip2Ratio, color: 'bg-hp-muted', isWinner: false },
    { name: 'gzip', ratio: gzipRatio, color: 'bg-hp-muted', isWinner: false },
  ];

  return (
    <div className="p-5 bg-hp-card border border-hp-border rounded-xl space-y-4">
      <h4 className="text-sm font-medium text-hp-text">How HyperPack compares:</h4>
      <div className="space-y-3">
        {items.map((item) => (
          <div key={item.name} className="flex items-center gap-3 text-sm">
            <div className="w-16 text-hp-muted font-mono">{item.name}</div>
            <div className="flex-1 h-4 bg-hp-bg rounded-sm overflow-hidden flex items-center">
              <div 
                className={`h-full ${item.color} transition-all duration-1000 ease-out`}
                style={{ width: `${(item.ratio / maxRatio) * 100}%` }}
              />
            </div>
            <div className="w-16 text-right font-mono text-hp-text flex items-center justify-end gap-1">
              {item.ratio.toFixed(2)}x
              {item.isWinner && <span className="text-lg leading-none">🏆</span>}
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}
