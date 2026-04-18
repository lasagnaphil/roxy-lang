#!/usr/bin/env bash
# Run the Crafting Interpreters Lox benchmarks against Roxy's tree-walk Lox
# interpreter, optionally comparing to clox for a reference bytecode VM.
#
# Usage:
#   ./run_benchmarks.sh                   # scaled-down (_small) benchmarks, all engines
#   ./run_benchmarks.sh --full            # upstream full-size benchmarks
#   ./run_benchmarks.sh fib trees         # only the named benchmarks (suffix auto-applied)
#   ./run_benchmarks.sh --roxy-only       # skip clox even if available
#   ./run_benchmarks.sh --clox-only       # skip roxy
#   ./run_benchmarks.sh --timeout=60      # per-benchmark wall timeout (seconds)
#   CLOX=/path/to/clox ./run_benchmarks.sh
#
# By default the runner uses the _small variants, which complete in seconds on
# Roxy's tree-walk interpreter. --full runs the upstream Crafting Interpreters
# sizes (tens of minutes on Roxy at current perf, seconds on clox).
set -u

# Roxy's tree-walk Lox interpreter running on top of the Roxy VM uses enough C
# stack that some benchmarks blow the default 8 MB limit. Raise it for this
# process (inherited by child bash / roxy invocations). clox is unaffected.
ulimit -s 65536 2>/dev/null || true

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

ROXY_BIN="${ROXY:-$PROJECT_ROOT/build/roxy}"
ROXY_MAIN="$PROJECT_ROOT/examples/lox/main.roxy"
CLOX_BIN="${CLOX:-/tmp/craftinginterpreters/clox}"

TIMEOUT_SECS=600
RUN_ROXY=1
RUN_CLOX=1
USE_FULL=0
SELECTED=()

# Benchmarks with a _small variant. Ones without (zoo_batch, string_equality)
# are only available at full size.
HAS_SMALL=(fib trees instantiation method_call invocation properties zoo equality binary_trees)
ALL_FULL=(fib trees instantiation method_call invocation properties zoo zoo_batch equality string_equality binary_trees)

for arg in "$@"; do
    case "$arg" in
        --full) USE_FULL=1 ;;
        --roxy-only) RUN_CLOX=0 ;;
        --clox-only) RUN_ROXY=0 ;;
        --timeout=*) TIMEOUT_SECS="${arg#--timeout=}" ;;
        -h|--help)
            sed -n '2,18p' "$0"
            exit 0 ;;
        --*) echo "unknown flag: $arg" >&2; exit 2 ;;
        *) SELECTED+=("$arg") ;;
    esac
done

resolve_file() {
    # $1 = benchmark name (with or without _small suffix).
    # Prints the full path; returns 1 if not found.
    local name="$1" base="$1"
    # Strip an explicit _small suffix so default-size selection still works.
    if [[ "$name" == *_small ]]; then base="${name%_small}"; fi
    local candidate
    if [[ $USE_FULL -eq 1 ]]; then
        candidate="$SCRIPT_DIR/$base.lox"
    else
        candidate="$SCRIPT_DIR/${base}_small.lox"
        # Fall back to full size if no _small variant exists.
        [[ -f "$candidate" ]] || candidate="$SCRIPT_DIR/$base.lox"
    fi
    [[ -f "$candidate" ]] || return 1
    printf "%s" "$candidate"
}

if [[ ${#SELECTED[@]} -eq 0 ]]; then
    if [[ $USE_FULL -eq 1 ]]; then
        BENCHES=("${ALL_FULL[@]}")
    else
        BENCHES=("${HAS_SMALL[@]}")
    fi
else
    BENCHES=("${SELECTED[@]}")
fi

if [[ $RUN_ROXY -eq 1 ]]; then
    if [[ ! -x "$ROXY_BIN" ]]; then
        echo "ERROR: roxy binary not found at $ROXY_BIN (build first: cd build && ninja)" >&2
        exit 1
    fi
    if [[ ! -f "$ROXY_MAIN" ]]; then
        echo "ERROR: roxy Lox entry not found at $ROXY_MAIN" >&2
        exit 1
    fi
fi

HAVE_CLOX=0
if [[ $RUN_CLOX -eq 1 ]] && [[ -x "$CLOX_BIN" ]]; then
    HAVE_CLOX=1
fi

# Pick a wall-clock timer. macOS `date` lacks %N; fall back to Python.
now_secs() {
    python3 -c 'import time; print(f"{time.perf_counter():.6f}")'
}

run_one() {
    # $1 = label, $2 = command (as eval string), $3 = lox file
    local label="$1" cmd="$2" lox="$3"
    local out_file rc elapsed start end
    out_file="$(mktemp)"
    start="$(now_secs)"
    # shellcheck disable=SC2086
    timeout --foreground --kill-after=5 "$TIMEOUT_SECS" bash -c "$cmd" >"$out_file" 2>&1
    rc=$?
    end="$(now_secs)"
    elapsed="$(python3 -c "print(f'{$end - $start:.3f}')")"

    if [[ $rc -eq 124 || $rc -eq 137 ]]; then
        printf "  %-6s %10s   (TIMEOUT after %ss)\n" "$label" "-" "$TIMEOUT_SECS"
    elif [[ $rc -ne 0 ]]; then
        printf "  %-6s %10s   (exit %d)\n" "$label" "-" "$rc"
        sed -n '1,5p' "$out_file" | sed 's/^/    | /'
    else
        # The last numeric line of a Lox benchmark is its self-reported elapsed
        # time (or a final `print clock()-start`). Show that alongside the
        # wall-clock time we measured so the user sees both views.
        local self
        self="$(grep -Eo '^-?[0-9]+\.?[0-9]*$' "$out_file" | tail -1)"
        if [[ -n "$self" ]]; then
            printf "  %-6s wall=%7.3fs  self=%ss\n" "$label" "$elapsed" "$self"
        else
            printf "  %-6s wall=%7.3fs\n" "$label" "$elapsed"
        fi
    fi
    rm -f "$out_file"
}

echo "=== Lox benchmarks ==="
echo "  roxy:    $ROXY_BIN $ROXY_MAIN <file>"
if [[ $HAVE_CLOX -eq 1 ]]; then
    echo "  clox:    $CLOX_BIN <file>"
elif [[ $RUN_CLOX -eq 1 ]]; then
    echo "  clox:    (not found at $CLOX_BIN — skipping comparison)"
fi
echo "  timeout: ${TIMEOUT_SECS}s per benchmark"
echo ""

for bench in "${BENCHES[@]}"; do
    if ! lox="$(resolve_file "$bench")"; then
        echo "-- $bench: no .lox file found, skipping"
        continue
    fi
    # Display name includes which size was actually selected.
    display="$(basename "$lox" .lox)"
    echo "-- $display"
    if [[ $RUN_ROXY -eq 1 ]]; then
        run_one "roxy" "'$ROXY_BIN' '$ROXY_MAIN' '$lox'" "$lox"
    fi
    if [[ $HAVE_CLOX -eq 1 ]]; then
        run_one "clox" "'$CLOX_BIN' '$lox'" "$lox"
    fi
    echo ""
done

echo "=== Done ==="
