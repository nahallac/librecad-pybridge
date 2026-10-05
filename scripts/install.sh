#!/usr/bin/env bash
# Build the plugin and install it where LibreCAD looks for user plugins.
#
# RS_System::getDirectoryList("plugins") includes
# $HOME/.<QC_APPDIR>/plugins, and LibreCAD is built with QC_APPDIR=librecad,
# so ~/.librecad/plugins works and no root access is needed. The system
# directory /usr/lib/librecad/plugins is also searched but is root-owned.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
plugin_dir="${LIBRECAD_PLUGIN_DIR:-$HOME/.librecad/plugins}"

qmake_bin="${QMAKE:-/usr/bin/qmake}"
qt_version="$("$qmake_bin" -query QT_VERSION)"
case "$qt_version" in
    5.*) ;;
    *)   echo "error: $qmake_bin reports Qt $qt_version; Qt 5 is required" >&2
         exit 1 ;;
esac

make -C "$repo_root" QMAKE="$qmake_bin"

mkdir -p "$plugin_dir"
install -m 0755 "$repo_root/build/liblc_pybridge.so" "$plugin_dir/"

echo "installed $plugin_dir/liblc_pybridge.so (Qt $qt_version)"
echo "restart LibreCAD; the entries appear under the Plugins menu."
