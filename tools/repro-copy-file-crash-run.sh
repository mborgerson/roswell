#!/usr/bin/env bash
#
# repro-copy-file-crash-run.sh -- Minimum reproduction runner for copy file crash bug
#
set -euo pipefail

: "${NXKRNL_XEMU:?path to xemu binary}"
: "${NXKRNL_HDD:?Xbox HDD qcow2 image}"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPRO_DIR="$REPO/tests/xbe/copy-file-crash-repro"
NXDK_DIR="${NXDK_DIR:-$REPO/third_party/nxdk}"
BUILD_DIR="${NXKRNL_BUILD_DIR:-$REPO/build}"

echo "[1/3] Building kernel..."
cmake --build "$BUILD_DIR" --target flash.bin

echo "[2/3] Building copy-file-crash-repro XBE..."
(
    eval "$("$NXDK_DIR/bin/activate" -s)"
    make -C "$REPRO_DIR"
)

ISO="$REPRO_DIR/copy-file-crash-repro.iso"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dvd)
            ISO="$2"
            shift 2
            ;;
        *)
            shift
            ;;
    esac
done
[ -f "$ISO" ] || { echo "Missing $ISO" >&2; exit 125; }

WORKDIR="$(mktemp -d -t copy-file-crash-repro.XXXXXX)"
trap 'rm -rf "$WORKDIR"' EXIT

TMP_HDD="$WORKDIR/hdd.qcow2"
cp "$NXKRNL_HDD" "$TMP_HDD"

CONFIG="$WORKDIR/xemu.toml"
cat > "$CONFIG" <<EOF
[general]
show_welcome = false
skip_boot_anim = true

[sys]
mem_limit = '128'

[sys.files]
bootrom_path = ''
flashrom_path = '$BUILD_DIR/flash.bin'
eeprom_path = ''
hdd_path = '$TMP_HDD'
dvd_path = '$ISO'
EOF

echo "[3/3] Starting reproduction run..."
LOG="$WORKDIR/repro.log"
XEMU_LOG="$WORKDIR/xemu.log"

set +e
timeout -k 2 20 "$NXKRNL_XEMU" -config_path "$CONFIG" -device lpc47m157 -serial file:"$LOG" > "$XEMU_LOG" 2>&1
rc=$?
set -e

if [ -f "$LOG" ]; then
    cat "$LOG"
fi

if grep -q "== copy-file-crash-repro end PASS ==" "$LOG" 2>/dev/null; then
    echo ""
    echo "========================================================"
    echo "  TEST PASSED: Copy completed without crashing"
    echo "========================================================"
    exit 0
fi

if grep -q "== copy-file-crash-repro end FAIL ==" "$LOG" 2>/dev/null; then
    echo ""
    echo "========================================================"
    echo "  BUG REPRODUCED: Copy failed or crashed as expected!"
    echo "========================================================"
    exit 1
fi

echo ""
echo "========================================================"
echo "  XEMU halted or crashed with return code $rc"
echo "========================================================"
exit 1
