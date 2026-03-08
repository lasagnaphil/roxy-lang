#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "=== Quicksort Benchmark (100,000 elements) ==="
echo ""

# --- C version ---
echo "Compiling C version..."
cc -O2 -o "$SCRIPT_DIR/quicksort_c" "$SCRIPT_DIR/quicksort.c"

echo "--- C (compiled, -O2) ---"
"$SCRIPT_DIR/quicksort_c"
echo ""

# --- Python version ---
echo "--- Python (interpreted) ---"
python3 "$SCRIPT_DIR/quicksort.py"
echo ""

# --- Roxy version ---
echo "--- Roxy (VM interpreter) ---"
ROXY="$PROJECT_ROOT/build/roxy"
if [ ! -f "$ROXY" ]; then
    echo "ERROR: $ROXY not found. Build the project first (cd build && ninja)."
    exit 1
fi
"$ROXY" "$SCRIPT_DIR/quicksort.roxy"
echo ""

# Cleanup
rm -f "$SCRIPT_DIR/quicksort_c"

echo "=== Done ==="
