#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "=== N-body Benchmark (5 bodies, 500,000 steps) ==="
echo ""

# --- C version ---
echo "Compiling C version..."
# -ffp-contract=off disables fused multiply-add so the C output matches
# Python/Roxy bit-for-bit (which don't use FMA in their interpreter loops).
cc -O2 -ffp-contract=off -o "$SCRIPT_DIR/nbody_c" "$SCRIPT_DIR/nbody.c" -lm

echo "--- C (compiled, -O2) ---"
"$SCRIPT_DIR/nbody_c"
echo ""

# --- Python version ---
echo "--- Python (interpreted) ---"
python3 "$SCRIPT_DIR/nbody.py"
echo ""

# --- Roxy version ---
echo "--- Roxy (VM interpreter) ---"
ROXY="$PROJECT_ROOT/build/roxy"
if [ ! -f "$ROXY" ]; then
    echo "ERROR: $ROXY not found. Build the project first (cd build && ninja)."
    exit 1
fi
"$ROXY" "$SCRIPT_DIR/nbody.roxy"
echo ""

# Cleanup
rm -f "$SCRIPT_DIR/nbody_c"

echo "=== Done ==="
