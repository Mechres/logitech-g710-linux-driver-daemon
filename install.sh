#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UDEV_RULE_SRC="${SCRIPT_DIR}/misc/90-logitech-g710-plus.rules"
UDEV_RULE_DST="/etc/udev/rules.d/90-logitech-g710-plus.rules"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    cat <<'EOF'
Usage: ./install.sh [--skip-udev]

Installs the Logitech G710+ kernel module:
  1. Builds the module
  2. Installs it into the running kernel modules path
  3. Runs depmod
  4. Installs and reloads the udev rule (unless --skip-udev is used)
EOF
    exit 0
fi

SKIP_UDEV=0
if [[ "${1:-}" == "--skip-udev" ]]; then
    SKIP_UDEV=1
elif [[ $# -gt 0 ]]; then
    echo "Unknown argument: $1" >&2
    echo "Use --help for usage." >&2
    exit 1
fi

if [[ "${EUID}" -ne 0 ]]; then
    if command -v sudo >/dev/null 2>&1; then
        exec sudo bash "$0" "$@"
    fi
    echo "Please run as root (or install sudo)." >&2
    exit 1
fi

echo "Building and installing module and daemon..."
make -C "${SCRIPT_DIR}" clean
make -C "${SCRIPT_DIR}"
make -C "${SCRIPT_DIR}" install
depmod -a

if [[ ! -f "/etc/g710d.conf" ]]; then
    echo "Installing default configuration to /etc/g710d.conf..."
    install -m 0644 "${SCRIPT_DIR}/g710d.conf.example" "/etc/g710d.conf"
fi

if [[ "${SKIP_UDEV}" -eq 0 ]]; then
    echo "Installing udev rule..."
    install -m 0644 "${UDEV_RULE_SRC}" "${UDEV_RULE_DST}"

    if command -v udevadm >/dev/null 2>&1; then
        udevadm control --reload-rules
        udevadm trigger --subsystem-match=hid
    fi
fi

if command -v systemctl >/dev/null 2>&1; then
    echo "Enabling and starting g710d service..."
    systemctl daemon-reload
    systemctl enable g710d.service
    systemctl restart g710d.service
fi

echo "Done."
