#!/usr/bin/env bash
# ============================================================
#  HyperPack — Traditional Benchmark Suite
#  Compares: HyperPack v11 vs gzip -9 vs bzip2 -9 vs xz -9
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CORPUS_DIR="$SCRIPT_DIR/corpus"
TMP_DIR="$SCRIPT_DIR/tmp"
RESULTS_DIR="$SCRIPT_DIR/results"
HP_BIN="$ROOT_DIR/hyperpack"

mkdir -p "$TMP_DIR" "$RESULTS_DIR"

# Colours
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'

# ── helpers ────────────────────────────────────────────────
human_size() {
    local bytes=$1
    if   [ "$bytes" -ge 1048576 ]; then printf "%.1f MB" "$(echo "scale=1; $bytes/1048576" | bc)"
    elif [ "$bytes" -ge 1024 ];    then printf "%.1f KB" "$(echo "scale=1; $bytes/1024"    | bc)"
    else printf "%d B" "$bytes"; fi
}

ratio() {
    local orig=$1 comp=$2
    echo "scale=3; $orig / $comp" | bc
}

# measure: compress, return elapsed_ms and compressed_size
# usage: measure_compress <method> <input> <output>
measure_compress() {
    local method="$1" input="$2" output="$3"
    local start end elapsed

    rm -f "$output"
    start=$(date +%s%N)
    case "$method" in
        hyperpack) "$HP_BIN" c "$input" "$output" 2>/dev/null ;;
        gzip)      gzip  -9 -k -c "$input" > "$output" ;;
        bzip2)     bzip2 -9 -k -c "$input" > "$output" ;;
        xz)        xz    -9 -k -c "$input" > "$output" ;;
    esac
    end=$(date +%s%N)
    elapsed=$(( (end - start) / 1000000 ))   # ms
    echo "$elapsed $(stat -c%s "$output")"
}

# verify roundtrip decompression
verify_roundtrip() {
    local method="$1" compressed="$2" original="$3"
    local restored="$TMP_DIR/verify_restored"
    rm -f "$restored"
    case "$method" in
        hyperpack) "$HP_BIN" d "$compressed" "$restored" 2>/dev/null ;;
        gzip)      gzip  -d -c "$compressed" > "$restored" ;;
        bzip2)     bzip2 -d -c "$compressed" > "$restored" ;;
        xz)        xz    -d -c "$compressed" > "$restored" ;;
    esac
    if cmp -s "$original" "$restored"; then echo "OK"; else echo "FAIL"; fi
}

# ── main benchmark ─────────────────────────────────────────
METHODS=(hyperpack gzip bzip2 xz)
LABELS=("HyperPack v11" "gzip -9" "bzip2 -9" "xz -9")
EXT=(hpk gz bz2 xz)

declare -A TOTAL_ORIG TOTAL_COMP TOTAL_TIME WINS
for m in "${METHODS[@]}"; do
    TOTAL_ORIG[$m]=0
    TOTAL_COMP[$m]=0
    TOTAL_TIME[$m]=0
    WINS[$m]=0
done

CSV_FILE="$RESULTS_DIR/benchmark_$(date +%Y%m%d_%H%M%S).csv"
echo "file,size_bytes,method,compressed_bytes,ratio,time_ms,roundtrip" > "$CSV_FILE"

echo ""
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════════════════${RESET}"
echo -e "${BOLD}${CYAN}  HyperPack v11 — Benchmark Suite  ($(date '+%Y-%m-%d %H:%M'))${RESET}"
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════════════════${RESET}"
echo ""

FILES=()
while IFS= read -r -d '' f; do FILES+=("$f"); done < <(find "$CORPUS_DIR" -maxdepth 1 -type f -print0 | sort -z)

for filepath in "${FILES[@]}"; do
    filename=$(basename "$filepath")
    orig_size=$(stat -c%s "$filepath")

    echo -e "${BOLD}▶ ${filename}${RESET}  $(human_size "$orig_size")"
    printf "  %-16s %12s %8s %8s %10s\n" "Compressor" "Compressed" "Ratio" "Time" "Roundtrip"
    printf "  %-16s %12s %8s %8s %10s\n" "──────────" "──────────" "─────" "────" "─────────"

    BEST_RATIO=0
    BEST_METHOD=""

    for i in "${!METHODS[@]}"; do
        method="${METHODS[$i]}"
        label="${LABELS[$i]}"
        ext="${EXT[$i]}"
        compressed="$TMP_DIR/${filename}.${ext}"

        read -r elapsed comp_size < <(measure_compress "$method" "$filepath" "$compressed")
        rt=$(verify_roundtrip "$method" "$compressed" "$filepath")
        r=$(ratio "$orig_size" "$comp_size")

        # track best
        if (( $(echo "$r > $BEST_RATIO" | bc -l) )); then
            BEST_RATIO=$r
            BEST_METHOD=$method
        fi

        # accumulate totals
        TOTAL_ORIG[$method]=$(( TOTAL_ORIG[$method] + orig_size ))
        TOTAL_COMP[$method]=$(( TOTAL_COMP[$method] + comp_size ))
        TOTAL_TIME[$method]=$(( TOTAL_TIME[$method] + elapsed ))

        # CSV
        echo "$filename,$orig_size,$method,$comp_size,$r,${elapsed},${rt}" >> "$CSV_FILE"

        rt_color="$GREEN"
        [ "$rt" = "FAIL" ] && rt_color="$RED"

        printf "  %-16s %12s %8s %7sms %10s\n" \
            "$label" \
            "$(human_size "$comp_size")" \
            "${r}x" \
            "$elapsed" \
            "$(echo -e "${rt_color}${rt}${RESET}")"
    done

    WINS[$BEST_METHOD]=$(( WINS[$BEST_METHOD] + 1 ))
    echo -e "  ${GREEN}Winner: ${LABELS[$(for i in "${!METHODS[@]}"; do [ "${METHODS[$i]}" = "$BEST_METHOD" ] && echo $i; done)]}${RESET}"
    echo ""
done

# ── Global Summary ─────────────────────────────────────────
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════════════════${RESET}"
echo -e "${BOLD}  GLOBAL SUMMARY  (${#FILES[@]} files — $(human_size ${TOTAL_ORIG[hyperpack]}) total)${RESET}"
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════════════════${RESET}"
printf "\n  %-16s %12s %12s %8s %10s %6s\n" \
    "Compressor" "Orig→Comp" "Total Time" "Avg Ratio" "Wins" "vs HP"
printf "  %-16s %12s %12s %8s %10s %6s\n" \
    "──────────" "─────────" "──────────" "─────────" "────" "─────"

HP_RATIO=$(ratio "${TOTAL_ORIG[hyperpack]}" "${TOTAL_COMP[hyperpack]}")

for i in "${!METHODS[@]}"; do
    method="${METHODS[$i]}"
    label="${LABELS[$i]}"
    avg_r=$(ratio "${TOTAL_ORIG[$method]}" "${TOTAL_COMP[$method]}")
    total_time_s=$(echo "scale=1; ${TOTAL_TIME[$method]}/1000" | bc)

    if [ "$method" = "hyperpack" ]; then
        vs_hp="—"
        color="$GREEN"
    else
        diff_pct=$(echo "scale=1; ($HP_RATIO - $avg_r) / $avg_r * 100" | bc)
        if (( $(echo "$diff_pct > 0" | bc -l) )); then
            vs_hp="+${diff_pct}%"
            color="$YELLOW"
        else
            vs_hp="${diff_pct}%"
            color="$RED"
        fi
    fi

    printf "  ${color}%-16s %12s %11ss %8sx %10d %6s${RESET}\n" \
        "$label" \
        "$(human_size "${TOTAL_ORIG[$method]}")→$(human_size "${TOTAL_COMP[$method]}")" \
        "$total_time_s" \
        "$avg_r" \
        "${WINS[$method]}" \
        "$vs_hp"
done

echo ""
echo -e "  ${CYAN}Full CSV results: ${CSV_FILE}${RESET}"
echo ""

# ── Speed comparison (MB/s) ────────────────────────────────
echo -e "${BOLD}  COMPRESSION SPEED (MB/s)${RESET}"
printf "  %-16s %12s\n" "Compressor" "Throughput"
printf "  %-16s %12s\n" "──────────" "──────────"
for i in "${!METHODS[@]}"; do
    method="${METHODS[$i]}"
    label="${LABELS[$i]}"
    mbps=$(echo "scale=2; ${TOTAL_ORIG[$method]} / 1048576 / (${TOTAL_TIME[$method]} / 1000)" | bc)
    printf "  %-16s %10s MB/s\n" "$label" "$mbps"
done
echo ""
