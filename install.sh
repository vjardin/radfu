#!/bin/sh
# RADFU installer script
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/vjardin/radfu/master/install.sh | sh
#   curl -fsSL https://raw.githubusercontent.com/vjardin/radfu/master/install.sh | sudo sh
#   curl -fsSL https://raw.githubusercontent.com/vjardin/radfu/master/install.sh | sh -s -- --version v0.0.1
#   curl -fsSL https://raw.githubusercontent.com/vjardin/radfu/master/install.sh | sh -s -- --ci
#
# Options:
#   --version VERSION   Install specific version (e.g., v0.0.1)
#   --ci                Install latest CI build (unstable)
#   --help              Show this help message
#
# Installation locations:
#   - Root:     /usr/local/bin/radfu
#   - Non-root: ~/.local/bin/radfu (also updates .bashrc, .profile, .zshrc)

set -e

REPO="vjardin/radfu"
RADFU_VERSION=""
USE_CI=false
IS_ROOT=false
INSTALL_DIR=""
# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

error() {
    printf "${RED}Error: %s${NC}\n" "$1" >&2
    exit 1
}

warn() {
    printf "${YELLOW}Warning: %s${NC}\n" "$1" >&2
}

info() {
    printf "${GREEN}%s${NC}\n" "$1"
}

# Update PATH in shell config file if needed
update_shell_config() {
    CONFIG_FILE="$1"
    if [ ! -f "$CONFIG_FILE" ]; then
        return
    fi

    # Check if PATH already includes our install dir
    if grep -q "$INSTALL_DIR" "$CONFIG_FILE" 2>/dev/null; then
        return
    fi

    # Add PATH export
    {
        echo ""
        echo "# Added by radfu installer"
        echo "export PATH=\"\$PATH:$INSTALL_DIR\""
    } >> "$CONFIG_FILE"
    info "Updated $CONFIG_FILE with PATH"
}

usage() {
    cat <<EOF
RADFU Installer

Usage: $0 [OPTIONS]

Options:
    --version VERSION   Install specific version (e.g., v0.0.1)
    --ci                Install latest CI build (unstable)
    --help              Show this help message

Installation locations:
    Root:     /usr/local/bin/radfu
    Non-root: ~/.local/bin/radfu (also updates shell config with PATH)

Examples:
    # Install latest stable release (user install to ~/.local/bin)
    curl -fsSL https://raw.githubusercontent.com/vjardin/radfu/master/install.sh | sh

    # Install system-wide (requires root)
    curl -fsSL https://raw.githubusercontent.com/vjardin/radfu/master/install.sh | sudo sh

    # Install specific version
    curl -fsSL https://raw.githubusercontent.com/vjardin/radfu/master/install.sh | sh -s -- --version v0.0.1

    # Install latest CI build
    curl -fsSL https://raw.githubusercontent.com/vjardin/radfu/master/install.sh | sh -s -- --ci
EOF
    exit 0
}

# Parse arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --version)
            RADFU_VERSION="$2"
            shift 2
            ;;
        --ci)
            USE_CI=true
            shift
            ;;
        --help)
            usage
            ;;
        *)
            error "Unknown option: $1"
            ;;
    esac
done

# Check for root and set install directory
if [ "$(id -u)" -eq 0 ]; then
    IS_ROOT=true
    INSTALL_DIR="/usr/local/bin"
else
    IS_ROOT=false
    INSTALL_DIR="$HOME/.local/bin"
    warn "Not running as root. Installing to $INSTALL_DIR"
fi

# Ensure install directory exists
mkdir -p "$INSTALL_DIR" 2>/dev/null || error "Failed to create $INSTALL_DIR"

# Remove existing installation
remove_existing() {
    # Check for package installation
    if [ "$IS_ROOT" = true ]; then
        if command -v dpkg >/dev/null 2>&1 && dpkg -l radfu 2>/dev/null | grep -q "^ii"; then
            info "Removing existing deb package..."
            apt-get remove -y radfu >/dev/null 2>&1 || true
        elif command -v rpm >/dev/null 2>&1 && rpm -q radfu >/dev/null 2>&1; then
            info "Removing existing rpm package..."
            if command -v dnf >/dev/null 2>&1; then
                dnf remove -y radfu >/dev/null 2>&1 || true
            elif command -v yum >/dev/null 2>&1; then
                yum remove -y radfu >/dev/null 2>&1 || true
            fi
        fi
    fi

    # Check for binary installations
    for dir in /usr/local/bin /usr/bin "$HOME/.local/bin"; do
        if [ -x "$dir/radfu" ]; then
            if [ "$IS_ROOT" = true ] || [ "$dir" = "$HOME/.local/bin" ]; then
                info "Removing existing binary at $dir/radfu..."
                rm -f "$dir/radfu" 2>/dev/null || true
            fi
        fi
    done
}

# Detect OS and architecture
detect_platform() {
    OS=$(uname -s | tr '[:upper:]' '[:lower:]')
    ARCH=$(uname -m)

    case "$OS" in
        linux)
            OS="linux"
            ;;
        darwin)
            OS="macos"
            ;;
        *)
            error "Unsupported operating system: $OS"
            ;;
    esac

    case "$ARCH" in
        x86_64|amd64)
            ARCH="x86_64"
            ;;
        aarch64|arm64)
            ARCH="arm64"
            ;;
        *)
            error "Unsupported architecture: $ARCH"
            ;;
    esac

    info "Detected platform: $OS $ARCH"
}

# Detect package manager
detect_package_manager() {
    PKG_MANAGER=""
    if [ "$OS" = "linux" ]; then
        if command -v apt-get >/dev/null 2>&1; then
            PKG_MANAGER="apt"
            # Detect if Debian or Ubuntu
            if [ -f /etc/os-release ]; then
                # shellcheck source=/dev/null
                . /etc/os-release
                case "$ID" in
                    debian)
                        PKG_DISTRO="debian"
                        ;;
                    ubuntu)
                        PKG_DISTRO="ubuntu"
                        ;;
                    *)
                        PKG_DISTRO="debian"  # Default to debian for apt-based
                        ;;
                esac
            fi
        elif command -v dnf >/dev/null 2>&1; then
            PKG_MANAGER="dnf"
        elif command -v yum >/dev/null 2>&1; then
            PKG_MANAGER="yum"
        fi
    fi

    if [ -n "$PKG_MANAGER" ]; then
        info "Detected package manager: $PKG_MANAGER"
    fi
}

# Get latest release version
get_latest_version() {
    LATEST=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" 2>/dev/null | \
        grep '"tag_name"' | sed -E 's/.*"([^"]+)".*/\1/')
    if [ -z "$LATEST" ]; then
        error "Failed to fetch latest release version"
    fi
    echo "$LATEST"
}

# Get latest CI run ID
get_latest_ci_run() {
    RUN_ID=$(curl -fsSL "https://api.github.com/repos/$REPO/actions/runs?branch=master&status=success&per_page=1" 2>/dev/null | \
        grep '"id"' | head -1 | sed -E 's/.*: ([0-9]+).*/\1/')
    if [ -z "$RUN_ID" ]; then
        error "Failed to fetch latest CI run"
    fi
    echo "$RUN_ID"
}

# Download and install from release
install_from_release() {
    VER="$1"
    info "Installing radfu $VER..."

    # Try package first for x86_64 Linux (only when running as root)
    if [ "$IS_ROOT" = true ] && [ "$OS" = "linux" ] && [ "$ARCH" = "x86_64" ] && [ -n "$PKG_MANAGER" ]; then
        case "$PKG_MANAGER" in
            apt)
                DEB_URL="https://github.com/$REPO/releases/download/$VER/radfu_${VER}_amd64-${PKG_DISTRO}.deb"
                info "Downloading $DEB_URL..."
                TMPFILE=$(mktemp)
                if curl -fsSL "$DEB_URL" -o "$TMPFILE" 2>/dev/null; then
                    info "Installing deb package..."
                    dpkg -i "$TMPFILE"
                    rm -f "$TMPFILE"
                    info "radfu $VER installed successfully via deb package!"
                    return 0
                else
                    warn "Package not available, falling back to binary"
                    rm -f "$TMPFILE"
                fi
                ;;
            dnf|yum)
                # Find the RPM file (version format differs)
                RPM_URL="https://github.com/$REPO/releases/download/$VER/radfu-${VER#v}-1.fc43.x86_64.rpm"
                info "Downloading $RPM_URL..."
                if $PKG_MANAGER install -y "$RPM_URL" 2>/dev/null; then
                    info "radfu $VER installed successfully via rpm package!"
                    return 0
                else
                    warn "Package not available, falling back to binary"
                fi
                ;;
        esac
    fi

    # Fall back to binary
    if [ "$OS" = "linux" ]; then
        if [ "$ARCH" = "arm64" ]; then
            BIN_NAME="radfu-$VER-linux-arm64-static"
        else
            BIN_NAME="radfu-$VER-linux-x86_64-static"
        fi
    elif [ "$OS" = "macos" ]; then
        BIN_NAME="radfu-$VER-macos-arm64"
    fi

    BIN_URL="https://github.com/$REPO/releases/download/$VER/$BIN_NAME"
    info "Downloading $BIN_URL..."

    TMPFILE=$(mktemp)
    if ! curl -fsSL "$BIN_URL" -o "$TMPFILE"; then
        rm -f "$TMPFILE"
        error "Failed to download binary"
    fi

    chmod +x "$TMPFILE"
    mv "$TMPFILE" "$INSTALL_DIR/radfu"
    info "radfu $VER installed successfully to $INSTALL_DIR/radfu"
}

# Download and install from CI artifacts (requires gh CLI)
install_from_ci() {
    if ! command -v gh >/dev/null 2>&1; then
        error "GitHub CLI (gh) is required for CI installation. Install it from https://cli.github.com/"
    fi

    if ! gh auth status >/dev/null 2>&1; then
        error "GitHub CLI is not authenticated. Run: gh auth login"
    fi

    info "Fetching latest CI build..."
    RUN_ID=$(get_latest_ci_run)
    info "Using CI run: $RUN_ID"

    # Determine artifact name
    if [ "$OS" = "linux" ]; then
        if [ "$ARCH" = "arm64" ]; then
            ARTIFACT="radfu-linux-arm64-static"
        else
            ARTIFACT="radfu-linux-x86_64-static"
        fi
    elif [ "$OS" = "macos" ]; then
        ARTIFACT="radfu-macos-arm64"
    fi

    TMPDIR=$(mktemp -d)
    info "Downloading artifact: $ARTIFACT..."

    if ! gh run download "$RUN_ID" -n "$ARTIFACT" -D "$TMPDIR" -R "$REPO"; then
        rm -rf "$TMPDIR"
        error "Failed to download CI artifact"
    fi

    chmod +x "$TMPDIR/radfu"
    mv "$TMPDIR/radfu" "$INSTALL_DIR/radfu"
    rm -rf "$TMPDIR"

    info "radfu (CI build) installed successfully to $INSTALL_DIR/radfu"
}

# Main
detect_platform
detect_package_manager
remove_existing

if [ "$USE_CI" = true ]; then
    install_from_ci
else
    if [ -z "$RADFU_VERSION" ]; then
        RADFU_VERSION=$(get_latest_version)
        info "Latest version: $RADFU_VERSION"
    fi
    install_from_release "$RADFU_VERSION"
fi

# Update shell configs for non-root installation
if [ "$IS_ROOT" = false ]; then
    update_shell_config "$HOME/.bashrc"
    update_shell_config "$HOME/.profile"
    update_shell_config "$HOME/.zshrc"

    # Also check if install dir is already in PATH
    case ":$PATH:" in
        *":$INSTALL_DIR:"*) ;;
        *) export PATH="$PATH:$INSTALL_DIR" ;;
    esac
fi

# Verify installation
info ""
info "Installation complete!"
info ""

# Check if in PATH and show version
if command -v radfu >/dev/null 2>&1; then
    radfu --version
    info ""
    RADFU_PATH=$(command -v radfu)
    info "Installed to: $RADFU_PATH"
elif [ -x "$INSTALL_DIR/radfu" ]; then
    "$INSTALL_DIR/radfu" --version
    info ""
    info "Installed to: $INSTALL_DIR/radfu"
    if [ "$IS_ROOT" = false ]; then
        warn "radfu not yet in PATH. Run: source ~/.bashrc (or restart your shell)"
    fi
else
    warn "Installation verification failed"
fi

info ""
info "Run 'radfu --help' to get started."
