#!/usr/bin/env bash
# ============================================================
#  HyperPack — Standard Corpus Benchmark
#  Corpora: Canterbury · Calgary · Silesia (official)
#  Competes: HyperPack v11  vs  gzip -9  vs  bzip2 -9  vs  xz -9
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CORPORA_DIR="$SCRIPT_DIR/corpora"
TMP_DIR="$SCRIPT_DIR/tmp_std"
RESULTS_DIR="$SCRIPT_DIR/results"
HP_BIN="$ROOT_DIR/hyperpack"

mkdir -p "$TMP_DIR" "$RESULTS_DIR"

BOLD='\033[1m'; CYAN='\033[0;36m'; GREEN='\033[0;32m'
YELLOW='\033[1;33m'; RED='\033[0;31m'; RESET='\033[0m'

METHODS=(hyperpack gzip bzip2 xz)
LABELS=("HyperPack v11" "gzip -9" "bzip2 -9" "xz -9")
EXT=(hpk gz bz2 xz)

# ── helpers ────────────────────────────────────────────────
measure_compress() {
    local method="$1" input="$2" output="$3"
    rm -f "$output"
    local start end
    start=$(date +%s%N)
    case "$method" in
        hyperpack) "$HP_BIN" c "$input" "$output" 2>/dev/null ;;
        gzip)      gzip  -9 -k -c "$input" > "$output" ;;
        bzip2)     bzip2 -9 -k -c "$input" > "$output" ;;
        xz)        xz    -9 -k -c "$input" > "$output" ;;
    esac
    end=$(date +%s%N)
    echo "$(( (end - start) / 1000000 )) $(stat -c%s "$output")"
}

verify_roundtrip() {
    local method="$1" compressed="$2" original="$3"
    local restored="$TMP_DIR/rt_check"
    rm -f "$restored"
    case "$method" in
        hyperpack) "$HP_BIN" d "$compressed" "$restored" 2>/dev/null ;;
        gzip)      gzip  -d -c "$compressed" > "$restored" ;;
        bzip2)     bzip2 -d -c "$compressed" > "$restored" ;;
        xz)        xz    -d -c "$compressed" > "$restored" ;;
    esac
    cmp -s "$original" "$restored" && echo "OK" || echo "FAIL"
}

run_corpus() {
    local corpus_name="$1"
    local corpus_dir="$2"
    local note="${3:-}"

    echo ""
    echo -e "${BOLD}${CYAN}╔══════════════════════════════════════════════════════════════╗${RESET}"
    echo -e "${BOLD}${CYAN}║  Corpus: ${corpus_name}  ${note}${RESET}"
    echo -e "${BOLD}${CYAN}╚══════════════════════════════════════════════════════════════╝${RESET}"

    declare -A c_orig c_comp c_time c_wins
    for m in "${METHODS[@]}"; do c_orig[$m]=0; c_comp[$m]=0; c_time[$m]=0; c_wins[$m]=0; done

    local files=()
    while IFS= read -r -d '' f; do files+=("$f"); done < <(find "$corpus_dir" -maxdepth 1 -type f -print0 | sort -z)

    local csv="$RESULTS_DIR/${corpus_name}_$(date +%Y%m%d_%H%M%S).csv"
    echo "corpus,file,size_bytes,method,compressed_bytes,ratio,time_ms,roundtrip" > "$csv"

    for filepath in "${files[@]}"; do
        local fname orig_size
        fname=$(basename "$filepath")
        orig_size=$(stat -c%s "$filepath")

        printf "\n  ${BOLD}%-22s${RESET} %s\n" "$fname" "$(numfmt --to=iec-i --suffix=B "$orig_size")"
        printf "  %-16s %10s %8s %8s %s\n" "Compressor" "Compressed" "Ratio" "Time" "RT"
        printf "  %-16s %10s %8s %8s %s\n" "──────────" "──────────" "─────" "────" "──"

        local best_ratio=0 best_method=""
        for i in "${!METHODS[@]}"; do
            local m="${METHODS[$i]}" label="${LABELS[$i]}" ext="${EXT[$i]}"
            local compressed="$TMP_DIR/${fname}.${ext}"

            local elapsed comp_size
            read -r elapsed comp_size < <(measure_compress "$m" "$filepath" "$compressed")
            local rt; rt=$(verify_roundtrip "$m" "$compressed" "$filepath")
            local ratio; ratio=$(echo "scale=3; $orig_size / $comp_size" | bc)

            c_orig[$m]=$(( c_orig[$m] + orig_size ))
            c_comp[$m]=$(( c_comp[$m] + comp_size ))
            c_time[$m]=$(( c_time[$m] + elapsed ))

            if (( $(echo "$ratio > $best_ratio" | bc -l) )); then
                best_ratio=$ratio; best_method=$m
            fi

            echo "$corpus_name,$fname,$orig_size,$m,$comp_size,$ratio,$elapsed,$rt" >> "$csv"

            local rt_col="$GREEN"; [ "$rt" = "FAIL" ] && rt_col="$RED"
            printf "  %-16s %10s %8s %7sms %s\n" \
                "$label" \
                "$(numfmt --to=iec-i --suffix=B "$comp_size")" \
                "${ratio}x" "$elapsed" \
                "$(echo -e "${rt_col}${rt}${RESET}")"
        done

        c_wins[$best_method]=$(( c_wins[$best_method] + 1 ))
        local winner_label="${LABELS[$(for i in "${!METHODS[@]}"; do [ "${METHODS[$i]}" = "$best_method" ] && echo $i; done)]}"
        echo -e "  ${GREEN}► Winner: $winner_label${RESET}"
    done

    # ── Corpus summary ──────────────────────────────────────
    local total_files="${#files[@]}"
    local total_orig="${c_orig[hyperpack]}"
    local hp_ratio; hp_ratio=$(echo "scale=3; $total_orig / ${c_comp[hyperpack]}" | bc)

    echo ""
    echo -e "  ${BOLD}── ${corpus_name} Summary  ($total_files files, $(numfmt --to=iec-i --suffix=B "$total_orig")) ──${RESET}"
    printf "  %-16s %10s %10s %9s %6s %8s\n" "Compressor" "Ratio" "Time" "Speed" "Wins" "vs HP"
    printf "  %-16s %10s %10s %9s %6s %8s\n" "──────────" "─────" "────" "─────" "────" "─────"

    for i in "${!METHODS[@]}"; do
        local m="${METHODS[$i]}" label="${LABELS[$i]}"
        local avg_r; avg_r=$(echo "scale=3; ${c_orig[$m]} / ${c_comp[$m]}" | bc)
        local t_s; t_s=$(echo "scale=1; ${c_time[$m]}/1000" | bc)
        local spd; spd=$(echo "scale=2; ${c_orig[$m]} / 1048576 / (${c_time[$m]} / 1000)" | bc)

        local vs_hp col
        if [ "$m" = "hyperpack" ]; then
            vs_hp="—"; col="$GREEN"
        else
            local d; d=$(echo "scale=1; ($hp_ratio - $avg_r) / $avg_r * 100" | bc)
            if (( $(echo "$d > 0" | bc -l) )); then
                vs_hp="+${d}%"; col="$GREEN"
            else
                vs_hp="${d}%"; col="$YELLOW"
            fi
        fi

        printf "  ${col}%-16s %10s %9ss %8s/s %6d %8s${RESET}\n" \
            "$label" "${avg_r}x" "$t_s" "${spd}MB" "${c_wins[$m]}" "$vs_hp"
    done

    echo -e "  ${CYAN}CSV: $csv${RESET}"
}

# ── Main ───────────────────────────────────────────────────
echo ""
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════════════════${RESET}"
echo -e "${BOLD}${CYAN}  HyperPack v11 — Official Corpus Benchmark  ($(date '+%Y-%m-%d %H:%M'))${RESET}"
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════════════════${RESET}"

run_corpus "Canterbury" "$CORPORA_DIR/canterbury" "(11 files, 2.8 MB)"
run_corpus "Calgary"    "$CORPORA_DIR/calgary"    "(18 files, 3.1 MB)"
run_corpus "Silesia"    "$CORPORA_DIR/silesia"    "(12 files, 203 MB)"

echo ""
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════════════════${RESET}"
echo -e "${BOLD}  ALL DONE — results in $RESULTS_DIR${RESET}"
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════════════════${RESET}"
echo ""
