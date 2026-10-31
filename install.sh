#!/usr/bin/env bash
# install.sh -- install the mesa-terakan Vulkan driver system-wide.
#
# Writes (prefix = /usr/local, outside pacman's /usr):
#   /usr/local/lib/libvulkan_terascale.so
#   /usr/local/share/vulkan/icd.d/terascale_icd.<cpu>.json
#   /etc/vulkan/icd.d/terascale_icd.<cpu>.json         (symlink)
#
# The /etc/vulkan/icd.d/ symlink is intentional: the Vulkan loader
# always scans that directory, while /usr/local/share is only picked
# up via XDG_DATA_DIRS -- which the Steam Linux Runtime sandbox does
# not propagate.
#
# Usage:
#   ./install.sh            # install
#   ./install.sh --uninstall

set -euo pipefail

BUILD_DIR=${BUILD_DIR:-build}
LOADER_DIR=/etc/vulkan/icd.d

if [ "${1:-}" = "--uninstall" ]; then
    sudo rm -f "$LOADER_DIR"/terascale_icd.*.json
    sudo rm -f /usr/local/share/vulkan/icd.d/terascale_icd.*.json
    sudo rm -f /usr/local/lib/libvulkan_terascale.so
    echo "mesa-terakan uninstalled."
    exit 0
fi

if ! [ -d "$BUILD_DIR" ]; then
    echo "no build directory at '$BUILD_DIR' -- run 'meson setup $BUILD_DIR' first" >&2
    exit 1
fi

sudo meson install -C "$BUILD_DIR"

MANIFEST=$(ls /usr/local/share/vulkan/icd.d/terascale_icd.*.json 2>/dev/null | head -1 || true)
if [ -z "$MANIFEST" ]; then
    echo "ERROR: no terascale ICD manifest found after meson install" >&2
    exit 1
fi

sudo install -d "$LOADER_DIR"
sudo ln -sf "$MANIFEST" "$LOADER_DIR/$(basename "$MANIFEST")"

echo
echo "Installed:"
echo "  $MANIFEST"
echo "  -> $LOADER_DIR/$(basename "$MANIFEST")"
echo
echo "To force a specific Steam game onto Terakan, set its launch option to:"
echo "    VK_DRIVER_FILES=$MANIFEST %command%"
