#!/bin/bash
#
# Copyright (C) 2026 Vincent Jardin <vjardin@free.fr> Free Mobile
#
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Bootloader command scanner for Renesas RA MCUs
#
# Iterates through all known bootloader commands and displays their responses.
# Useful for protocol exploration and documentation.
#
# Requirements:
#   - J16 shorted for boot mode
#   - Board connected via USB
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RADFU="${RADFU:-$(cd "$SCRIPT_DIR/.." && pwd)/build/radfu}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Scan bootloader commands and display responses.

Options:
  -a, --all         Scan all commands 0x00-0x7F (slow)
  -k, --known       Scan only known/documented commands (default)
  -r, --range M-N   Scan command range (hex), e.g., 0x00-0x20
  -c, --cmd CMD     Run single command with optional data
  -v, --verbose     Show full packet details for each command
  -q, --quiet       Show only summary (no packet details)
  -h, --help        Show this help

Examples:
  $0                    # Scan known commands
  $0 -a                 # Scan all 0x00-0x7F
  $0 -r 0x30-0x40       # Scan specific range
  $0 -c 0x3A            # Single command (signature)
  $0 -c "0x3B 0x00"     # Command with data (area 0)

Known Commands:
  No data:  0x00 (INQ), 0x2C (DLM), 0x3A (SIG), 0x4F (BND)
  With data: 0x12 (ERA), 0x13 (WRI), 0x15 (REA), 0x18 (CRC),
             0x28-0x2B (KEY), 0x30 (AUTH), 0x34 (BAU),
             0x3B (ARE), 0x4E (BND_SET), 0x50-0x52 (INI/PRM),
             0x71 (DLM_TRANSIT)
EOF
    exit 0
}

check_device() {
    if [ ! -x "$RADFU" ]; then
        echo -e "${RED}Error:${NC} radfu not found at $RADFU"
        echo "Build with: meson compile -C build"
        exit 1
    fi

    if ! "$RADFU" info -q >/dev/null 2>&1; then
        echo -e "${RED}Error:${NC} Cannot connect to device"
        echo "Check: J16 shorted for boot mode, USB connected"
        exit 1
    fi
}

run_cmd() {
    local cmd="$1"
    shift
    local data="$*"
    local output
    local status

    if [ -n "$data" ]; then
        # shellcheck disable=SC2086
        output=$("$RADFU" raw "$cmd" $data 2>&1)
    else
        output=$("$RADFU" raw "$cmd" 2>&1)
    fi
    status=$?

    echo "$output"
    return $status
}

print_cmd_header() {
    local cmd="$1"
    local name="$2"
    echo -e "\n${CYAN}----------------------------------------${NC}"
    echo -e "${CYAN}Command: $cmd ($name)${NC}"
    echo -e "${CYAN}----------------------------------------${NC}"
}

scan_single() {
    local cmd="$1"
    shift
    local data="$*"
    local output
    local res

    if [ -n "$data" ]; then
        # shellcheck disable=SC2086
        output=$(run_cmd "$cmd" $data)
    else
        output=$(run_cmd "$cmd")
    fi

    if [ "$QUIET" -eq 1 ]; then
        res=$(echo "$output" | grep -E "RES:|STS:" | head -2)
        printf "%-6s: %s\n" "$cmd" "$res"
    else
        echo "$output"
    fi
}

scan_known() {
    echo -e "${BLUE}=== Scanning Known Commands ===${NC}"

    # Commands without data
    print_cmd_header "0x00" "INQ - Inquiry"
    scan_single 0x00

    print_cmd_header "0x2C" "DLM - DLM State Request"
    scan_single 0x2C

    print_cmd_header "0x3A" "SIG - Signature Request"
    scan_single 0x3A

    print_cmd_header "0x4F" "BND - Boundary Request"
    scan_single 0x4F

    # Area information for all areas
    print_cmd_header "0x3B" "ARE - Area Information"
    echo -e "${YELLOW}Area 0:${NC}"
    scan_single 0x3B 0x00
    echo -e "${YELLOW}Area 1:${NC}"
    scan_single 0x3B 0x01
    echo -e "${YELLOW}Area 2:${NC}"
    scan_single 0x3B 0x02
    echo -e "${YELLOW}Area 3:${NC}"
    scan_single 0x3B 0x03

    # Parameter request
    print_cmd_header "0x52" "PRM - Parameter Request"
    echo -e "${YELLOW}Parameter 0x01 (Initialize command):${NC}"
    scan_single 0x52 0x01

    # Commands that need data (show hint)
    echo -e "\n${BLUE}=== Commands Requiring Data (hints) ===${NC}"

    for cmd in 0x12 0x13 0x15 0x18; do
        printf "%-6s: " "$cmd"
        run_cmd "$cmd" 2>&1 | grep -E "Hint:" | head -1
    done

    for cmd in 0x28 0x29 0x2A 0x2B 0x30 0x34 0x4E 0x50 0x51 0x71; do
        printf "%-6s: " "$cmd"
        run_cmd "$cmd" 2>&1 | grep -E "Hint:" | head -1
    done
}

scan_range() {
    local start="$1"
    local end="$2"
    local start_dec=$((start))
    local end_dec=$((end))

    echo -e "${BLUE}=== Scanning Range $(printf '0x%02X' $start_dec)-$(printf '0x%02X' $end_dec) ===${NC}"

    for ((i=start_dec; i<=end_dec; i++)); do
        local cmd
        local output
        local res_line
        local sts_line

        cmd=$(printf "0x%02X" $i)
        output=$(run_cmd "$cmd" 2>&1)
        res_line=$(echo "$output" | grep "RES:" | head -1)
        sts_line=$(echo "$output" | grep "STS:" | head -1)

        if echo "$res_line" | grep -q "| OK)"; then
            local name
            name="${res_line#*(}"
            name="${name% | OK)*}"
            echo -e "${GREEN}$cmd${NC}: OK - $name"
            if [ "$VERBOSE" -eq 1 ]; then
                echo "$output" | grep -E "^\s+(Raw:|DATA|->)" | head -10
                echo
            fi
        elif echo "$sts_line" | grep -q "ERR_PCKT"; then
            echo -e "${YELLOW}$cmd${NC}: Recognized (needs data)"
            if [ "$VERBOSE" -eq 1 ]; then
                echo "$output" | grep -E "Hint:" | head -1
                echo
            fi
        elif echo "$sts_line" | grep -q "ERR_NSCM"; then
            echo -e "${RED}$cmd${NC}: Unrecognized"
        else
            echo -e "$cmd: $res_line"
        fi
    done
}

scan_all() {
    echo -e "${BLUE}=== Full Command Scan (0x00-0x7F) ===${NC}"
    echo "This may take a while..."
    echo

    local ok_cmds=""
    local data_cmds=""
    local unrec_cmds=""

    for ((i=0; i<=127; i++)); do
        local cmd
        local output
        local res_line
        local sts_line

        cmd=$(printf "0x%02X" $i)
        output=$(run_cmd "$cmd" 2>&1)
        res_line=$(echo "$output" | grep "RES:" | head -1)
        sts_line=$(echo "$output" | grep "STS:" | head -1)

        if echo "$res_line" | grep -q "| OK)"; then
            ok_cmds="$ok_cmds $cmd"
            if [ "$VERBOSE" -eq 1 ]; then
                local name
                name="${res_line#*(}"
                name="${name% | OK)*}"
                echo -e "${GREEN}$cmd${NC}: OK - $name"
                echo "$output" | grep -E "^\s+(Raw:|DATA|->)" | head -10
                echo
            else
                echo -ne "${GREEN}.${NC}"
            fi
        elif echo "$sts_line" | grep -q "ERR_PCKT"; then
            data_cmds="$data_cmds $cmd"
            if [ "$VERBOSE" -eq 1 ]; then
                echo -e "${YELLOW}$cmd${NC}: Recognized (needs data)"
                echo "$output" | grep -E "Hint:" | head -1
                echo
            else
                echo -ne "${YELLOW}.${NC}"
            fi
        else
            unrec_cmds="$unrec_cmds $cmd"
            if [ "$VERBOSE" -ne 1 ]; then
                echo -ne "${RED}.${NC}"
            fi
        fi

        # Progress indicator every 16 commands (non-verbose only)
        if [ "$VERBOSE" -ne 1 ] && [ $((i % 16)) -eq 15 ]; then
            printf " %s\n" "$(printf '0x%02X' $i)"
        fi
    done

    echo -e "\n\n${BLUE}=== Summary ===${NC}"
    echo -e "${GREEN}OK (no data needed):${NC}$ok_cmds"
    echo -e "${YELLOW}Recognized (needs data):${NC}$data_cmds"
    echo -e "${RED}Unrecognized:${NC} $(echo "$unrec_cmds" | wc -w) commands"
}

# Defaults
MODE="known"
QUIET=0
VERBOSE=0
RANGE_START=""
RANGE_END=""
SINGLE_CMD=""

# Parse arguments
while [ $# -gt 0 ]; do
    case "$1" in
        -a|--all)
            MODE="all"
            shift
            ;;
        -k|--known)
            MODE="known"
            shift
            ;;
        -r|--range)
            MODE="range"
            IFS='-' read -r RANGE_START RANGE_END <<< "$2"
            shift 2
            ;;
        -c|--cmd)
            MODE="single"
            SINGLE_CMD="$2"
            shift 2
            ;;
        -v|--verbose)
            VERBOSE=1
            shift
            ;;
        -q|--quiet)
            QUIET=1
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
done

echo -e "${BLUE}Renesas RA Bootloader Command Scanner${NC}"
echo

check_device

case "$MODE" in
    known)
        scan_known
        ;;
    all)
        scan_all
        ;;
    range)
        scan_range "$RANGE_START" "$RANGE_END"
        ;;
    single)
        # shellcheck disable=SC2086
        scan_single $SINGLE_CMD
        ;;
esac

echo -e "\n${GREEN}Done.${NC}"
