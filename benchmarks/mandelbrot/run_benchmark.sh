#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "=== Mandelbrot Benchmark (800x800, max_iter=100) ==="
echo ""

# --- C version ---
echo "Compiling C version..."
# -ffp-contract=off disables fused multiply-add so the C checksum matches
# Python/Roxy bit-for-bit (which don't use FMA in their interpreter loops).
cc -O2 -ffp-contract=off -o "$SCRIPT_DIR/mandelbrot_c" "$SCRIPT_DIR/mandelbrot.c"

echo "--- C (compiled, -O2) ---"
"$SCRIPT_DIR/mandelbrot_c"
echo ""

# --- Python version ---
echo "--- Python (interpreted) ---"
python3 "$SCRIPT_DIR/mandelbrot.py"
echo ""

# --- Roxy version ---
echo "--- Roxy (VM interpreter) ---"
ROXY="$PROJECT_ROOT/build/roxy"
if [ ! -f "$ROXY" ]; then
    echo "ERROR: $ROXY not found. Build the project first (cd build && ninja)."
    exit 1
fi
"$ROXY" "$SCRIPT_DIR/mandelbrot.roxy"
echo ""

# Cleanup
rm -f "$SCRIPT_DIR/mandelbrot_c"

echo "=== Done ==="
