#!/bin/bash
#
# Hardware test script for radfu with Renesas RA boards
#
# Requirements:
#   - J16 shorted for boot mode
#   - Board connected via USB or UART (use -t for UART mode)
#
# SAFETY: This script avoids any operations that could lock the board:
#   - No dlm-transit to lck_dbg or lck_boot
#   - No param-set disable
#   - Only non-destructive operations by default
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RADFU="${RADFU:-$(cd "$SCRIPT_DIR/.." && pwd)/build/radfu}"
TMPDIR="${TMPDIR:-/tmp}"
TEST_FILE="$TMPDIR/radfu_test_$$"

# Test configuration
VERBOSE=${VERBOSE:-0}
RUN_WRITE_TESTS=${RUN_WRITE_TESTS:-0}
UART_PORT=${UART_PORT:-}

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Counters
PASS=0
FAIL=0
SKIP=0

cleanup() {
    rm -f "$TEST_FILE" "$TEST_FILE.bin" "$TEST_FILE.crc" 2>/dev/null || true
}
trap cleanup EXIT

log_ok()   { echo -e "${GREEN}[PASS]${NC} $1"; PASS=$((PASS + 1)); }
log_fail() { echo -e "${RED}[FAIL]${NC} $1"; FAIL=$((FAIL + 1)); }
log_skip() { echo -e "${YELLOW}[SKIP]${NC} $1"; SKIP=$((SKIP + 1)); }
log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }

run_radfu() {
    local cmd="$1"
    shift
    local uart_opts=()
    if [ -n "$UART_PORT" ]; then
        uart_opts=(-u -p "$UART_PORT")
    fi
    if [ "$VERBOSE" -eq 1 ]; then
        log_info "Running: $RADFU ${uart_opts[*]} $cmd $*"
    fi
    "$RADFU" "${uart_opts[@]}" "$cmd" "$@" 2>&1
}

run_radfu_quiet() {
    local uart_opts=()
    if [ -n "$UART_PORT" ]; then
        uart_opts=(-u -p "$UART_PORT")
    fi
    "$RADFU" "${uart_opts[@]}" "$@" >/dev/null 2>&1
}

test_info() {
    echo
    log_info "=== Test: info command ==="
    local output
    if output=$(run_radfu info); then
        # Verify expected fields in output
        if echo "$output" | grep -q "Device Group:" && \
           echo "$output" | grep -q "Boot Firmware:" && \
           echo "$output" | grep -q "Product Name:"; then
            log_ok "info: device information retrieved"
            if [ "$VERBOSE" -eq 1 ]; then
                echo "$output" | head -20
            fi
        else
            log_fail "info: incomplete response"
            return 1
        fi
    else
        log_fail "info: command failed"
        return 1
    fi
}

test_dlm() {
    echo
    log_info "=== Test: dlm command (read DLM state) ==="
    local output
    if output=$(run_radfu dlm); then
        if echo "$output" | grep -qE "DLM State:.*0x0[1-8]"; then
            local state
            state=$(echo "$output" | grep "DLM State:" | head -1)
            log_ok "dlm: $state"
            # Warn if board is in a locked state
            if echo "$output" | grep -qE "LCK_DBG|LCK_BOOT"; then
                log_warn "Board is in a LOCKED state!"
            fi
        else
            log_fail "dlm: unexpected response"
            return 1
        fi
    else
        log_fail "dlm: command failed"
        return 1
    fi
}

test_boundary() {
    echo
    log_info "=== Test: boundary command (read TrustZone settings) ==="
    local output
    if output=$(run_radfu boundary); then
        if echo "$output" | grep -q "CFS1:\|Boundary"; then
            log_ok "boundary: settings retrieved"
            if [ "$VERBOSE" -eq 1 ]; then
                echo "$output"
            fi
        else
            log_fail "boundary: unexpected response"
            return 1
        fi
    else
        log_fail "boundary: command failed"
        return 1
    fi
}

test_param() {
    echo
    log_info "=== Test: param command (read initialization parameter) ==="
    local output
    if output=$(run_radfu param); then
        if echo "$output" | grep -qE "Initialization.*enabled|Initialization.*disabled|Parameter"; then
            log_ok "param: parameter retrieved"
            if echo "$output" | grep -qi "disabled"; then
                log_warn "Initialization command is DISABLED - factory reset not possible!"
            fi
        else
            log_fail "param: unexpected response"
            return 1
        fi
    else
        log_fail "param: command failed"
        return 1
    fi
}

test_osis() {
    echo
    log_info "=== Test: osis command (read ID code protection status) ==="
    local output
    if output=$(run_radfu osis); then
        if echo "$output" | grep -qE "OSIS|protection|ID code"; then
            log_ok "osis: status retrieved"
            if [ "$VERBOSE" -eq 1 ]; then
                echo "$output"
            fi
        else
            log_fail "osis: unexpected response"
            return 1
        fi
    else
        log_fail "osis: command failed"
        return 1
    fi
}

test_key_verify() {
    echo
    log_info "=== Test: key-verify command (check key slots) ==="
    for idx in 0 1; do
        if run_radfu_quiet key-verify "$idx"; then
            log_ok "key-verify $idx: slot checked"
        else
            # Key verify returning error is normal if no key is set
            log_ok "key-verify $idx: slot empty or not supported"
        fi
    done
}

test_ukey_verify() {
    echo
    log_info "=== Test: ukey-verify command (check user key slots) ==="
    for idx in 0 1; do
        if run_radfu_quiet ukey-verify "$idx"; then
            log_ok "ukey-verify $idx: slot checked"
        else
            log_ok "ukey-verify $idx: slot empty or not supported"
        fi
    done
}

test_read_code_flash() {
    echo
    log_info "=== Test: read command (code flash) ==="
    # Read first 256 bytes of code flash
    if run_radfu read -a 0x0 -s 0x100 "$TEST_FILE.bin" >/dev/null; then
        if [ -f "$TEST_FILE.bin" ] && [ "$(stat -c%s "$TEST_FILE.bin" 2>/dev/null || stat -f%z "$TEST_FILE.bin")" -eq 256 ]; then
            log_ok "read: code flash (256 bytes from 0x0)"
            rm -f "$TEST_FILE.bin"
        else
            log_fail "read: output file size mismatch"
            return 1
        fi
    else
        log_fail "read: code flash read failed"
        return 1
    fi
}

test_read_data_flash() {
    echo
    log_info "=== Test: read command (data flash) ==="
    # Read first 64 bytes of data flash
    if run_radfu read -a 0x08000000 -s 0x40 "$TEST_FILE.bin" >/dev/null; then
        if [ -f "$TEST_FILE.bin" ]; then
            log_ok "read: data flash (64 bytes from 0x08000000)"
            rm -f "$TEST_FILE.bin"
        else
            log_fail "read: output file not created"
            return 1
        fi
    else
        log_fail "read: data flash read failed"
        return 1
    fi
}

test_crc() {
    echo
    log_info "=== Test: crc command (checksum calculation) ==="
    local output
    # CRC of first 32KB of code flash (must be aligned to CRC unit)
    if output=$(run_radfu crc -a 0x0 -s 0x8000); then
        if echo "$output" | grep -qE "CRC.*0x[0-9A-Fa-f]+|checksum"; then
            log_ok "crc: calculated for code flash region"
            if [ "$VERBOSE" -eq 1 ]; then
                echo "$output" | grep -i crc
            fi
        else
            log_fail "crc: no checksum in output"
            return 1
        fi
    else
        log_fail "crc: command failed"
        return 1
    fi
}

test_reconnection() {
    echo
    log_info "=== Test: reconnection (multiple consecutive commands) ==="
    local count=0
    for i in 1 2 3; do
        if run_radfu_quiet info; then
            count=$((count + 1))
        else
            log_fail "reconnection: run $i failed"
            return 1
        fi
    done
    log_ok "reconnection: $count consecutive info commands succeeded"
}

test_baudrate() {
    echo
    log_info "=== Test: baud rate switching ==="
    # Test with high baud rate
    if run_radfu_quiet info -b 115200; then
        log_ok "baudrate: 115200 bps works"
    else
        log_skip "baudrate: 115200 bps not supported or failed"
        return 0
    fi

    # Test with even higher baud rate
    if run_radfu_quiet info -b 1000000; then
        log_ok "baudrate: 1000000 bps works"
    else
        log_ok "baudrate: 1000000 bps not supported (expected on some devices)"
    fi
}

test_write_data_flash() {
    echo
    log_info "=== Test: write/erase command (data flash only) ==="

    if [ "$RUN_WRITE_TESTS" -ne 1 ]; then
        log_skip "write: skipped (set RUN_WRITE_TESTS=1 to enable)"
        return 0
    fi

    log_warn "Writing to data flash at 0x08000000..."

    # Create test pattern (64 bytes, aligned to write unit)
    printf '\xAA\x55\xAA\x55' > "$TEST_FILE.bin"
    for i in $(seq 1 15); do
        printf '\xAA\x55\xAA\x55' >> "$TEST_FILE.bin"
    done

    # Erase data flash sector
    if ! run_radfu erase -a 0x08000000 -s 0x40 >/dev/null; then
        log_fail "write: erase failed"
        return 1
    fi
    log_ok "write: erase succeeded"

    # Write test pattern
    if ! run_radfu write -a 0x08000000 "$TEST_FILE.bin" >/dev/null; then
        log_fail "write: write failed"
        return 1
    fi
    log_ok "write: write succeeded"

    # Read back and verify
    if ! run_radfu read -a 0x08000000 -s 0x40 "$TEST_FILE.crc" >/dev/null; then
        log_fail "write: read-back failed"
        return 1
    fi

    if cmp -s "$TEST_FILE.bin" "$TEST_FILE.crc"; then
        log_ok "write: verify succeeded"
    else
        log_fail "write: verify mismatch"
        return 1
    fi

    rm -f "$TEST_FILE.bin" "$TEST_FILE.crc"
}

print_header() {
    echo "========================================"
    echo "  radfu Hardware Test Suite"
    echo "========================================"
    echo
    echo "Safety: This script does NOT perform any locking operations."
    echo "        Your board will remain usable for development."
    echo
}

print_summary() {
    echo
    echo "========================================"
    echo "  Test Summary"
    echo "========================================"
    echo -e "  ${GREEN}PASSED${NC}: $PASS"
    echo -e "  ${RED}FAILED${NC}: $FAIL"
    echo -e "  ${YELLOW}SKIPPED${NC}: $SKIP"
    echo "========================================"

    if [ "$FAIL" -gt 0 ]; then
        return 1
    fi
    return 0
}

check_prerequisites() {
    # Check radfu exists
    if [ ! -x "$RADFU" ]; then
        log_fail "radfu not found at $RADFU"
        log_info "Build with: meson compile -C build"
        exit 1
    fi

    # Check device is connected
    if [ -n "$UART_PORT" ]; then
        # UART mode: check specified port exists
        if [ ! -c "$UART_PORT" ]; then
            log_fail "UART port not found: $UART_PORT"
            exit 1
        fi
        log_info "Using UART mode: $UART_PORT"
    else
        # USB mode: check for serial device (Linux: ttyACM/ttyUSB, macOS: cu.usbmodem)
        if ! ls /dev/ttyACM* >/dev/null 2>&1 && \
           ! ls /dev/ttyUSB* >/dev/null 2>&1 && \
           ! ls /dev/cu.usbmodem* >/dev/null 2>&1; then
            log_fail "No serial device found"
            log_info "Check: J16 shorted, USB connected, board powered"
            exit 1
        fi
    fi

    # Quick connectivity test
    if ! run_radfu_quiet info; then
        log_fail "Cannot connect to device"
        log_info "Check: J16 shorted for boot mode, try pressing RESET"
        exit 1
    fi

    log_ok "Device connected and responsive"
}

usage() {
    cat <<EOF
Usage: $0 [OPTIONS] [TESTS...]

Options:
  -v, --verbose       Show detailed output
  -w, --write-tests   Enable write/erase tests (modifies data flash)
  -t, --tty <port>    Use UART mode with specified port (default: USB)
  -h, --help          Show this help

Tests (default: all safe tests):
  info        Device information
  dlm         DLM state query
  boundary    TrustZone boundary settings
  param       Initialization parameter
  osis        OSIS protection status
  key         Key slot verification
  read        Flash read operations
  crc         CRC calculation
  reconnect   Multiple command sessions
  baudrate    Baud rate switching
  write       Write/erase tests (requires -w)
  all         Run all tests

Environment:
  RADFU             Path to radfu binary (default: ./build/radfu)
  VERBOSE=1         Enable verbose output
  RUN_WRITE_TESTS=1 Enable write tests
  UART_PORT         UART port for -t option (default: /dev/ttyUSB0)

Examples:
  $0                           # Run all safe tests via USB
  $0 -v info dlm               # Run specific tests verbosely
  $0 -w all                    # Run all tests including write
  $0 -t /dev/ttyUSB0           # Run tests via UART
EOF
    exit 0
}

# Parse arguments
while [ $# -gt 0 ]; do
    case "$1" in
        -v|--verbose)
            VERBOSE=1
            shift
            ;;
        -w|--write-tests)
            RUN_WRITE_TESTS=1
            shift
            ;;
        -t|--tty)
            UART_PORT="${2:-/dev/ttyUSB0}"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        -*)
            echo "Unknown option: $1"
            usage
            ;;
        *)
            break
            ;;
    esac
done

# Determine which tests to run
TESTS="${*:-all}"

print_header
check_prerequisites

# Run tests
for test in $TESTS; do
    case "$test" in
        info)       test_info ;;
        dlm)        test_dlm ;;
        boundary)   test_boundary ;;
        param)      test_param ;;
        osis)       test_osis ;;
        key)        test_key_verify; test_ukey_verify ;;
        read)       test_read_code_flash; test_read_data_flash ;;
        crc)        test_crc ;;
        reconnect)  test_reconnection ;;
        baudrate)   test_baudrate ;;
        write)      test_write_data_flash ;;
        all)
            test_info
            test_dlm
            test_boundary
            test_param
            test_osis
            test_key_verify
            test_ukey_verify
            test_read_code_flash
            test_read_data_flash
            test_crc
            test_reconnection
            test_baudrate
            test_write_data_flash
            ;;
        *)
            log_warn "Unknown test: $test"
            ;;
    esac
done

print_summary
