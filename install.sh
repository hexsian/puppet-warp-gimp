#!/usr/bin/env bash
#
# Puppet Warp for GIMP - Installer (offline, copies the files next to this script)
# Repository: https://github.com/hexsian/puppet-warp-gimp
#
# Usage (run from the extracted release folder):
#   ./install.sh             # Install or update from the files in this folder
#   ./install.sh --uninstall # Remove the plugin and GEGL binaries
#
# To update: download the newer release zip, extract it, run ./install.sh again.
#

set -euo pipefail

readonly PLUGIN_NAME="puppet-warp"
readonly GEGL_MODULE="puppet-warp.so"
readonly SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -t 1 ]]; then
    readonly C_RESET='\033[0m' C_RED='\033[0;31m' C_GREEN='\033[0;32m' C_YELLOW='\033[1;33m' C_BLUE='\033[0;34m'
else
    readonly C_RESET='' C_RED='' C_GREEN='' C_YELLOW='' C_BLUE=''
fi

log_info()    { printf "%b[INFO]%b  %s\n" "${C_BLUE}" "${C_RESET}" "$*"; }
log_success() { printf "%b[OK]%b    %s\n" "${C_GREEN}" "${C_RESET}" "$*"; }
log_warn()    { printf "%b[WARN]%b  %s\n" "${C_YELLOW}" "${C_RESET}" "$*"; }
log_error()   { printf "%b[ERROR]%b %s\n" "${C_RED}" "${C_RESET}" "$*" >&2; }

# Native, Flatpak and Snap GIMP 3.x plug-in folders
get_target_plugin_dirs() {
    local dirs=()
    local xdg_config="${XDG_CONFIG_HOME:-$HOME/.config}"
    local search_roots=(
        "$xdg_config/GIMP"
        "$HOME/.var/app/org.gimp.GIMP/config/GIMP"
        "$HOME/snap/gimp/current/.config/GIMP"
    )

    for root in "${search_roots[@]}"; do
        if [[ -d "$root" ]]; then
            for ver_dir in "$root"/3.*; do
                [[ -d "$ver_dir" ]] && dirs+=("$ver_dir/plug-ins/$PLUGIN_NAME")
            done
        fi
    done

    if [[ ${#dirs[@]} -eq 0 ]]; then
        dirs+=(
            "$xdg_config/GIMP/3.0/plug-ins/$PLUGIN_NAME"
            "$xdg_config/GIMP/3.2/plug-ins/$PLUGIN_NAME"
        )
    fi

    printf '%s\n' "${dirs[@]}"
}

get_gegl_dirs() {
    local xdg_data="${XDG_DATA_HOME:-$HOME/.local/share}"
    printf '%s\n' \
        "$xdg_data/gegl-0.4/plug-ins" \
        "$HOME/.var/app/org.gimp.GIMP/data/gegl-0.4/plug-ins" \
        "$HOME/snap/gimp/current/.local/share/gegl-0.4/plug-ins"
}

# Copy every file from this folder except the installers, VCS/cache junk,
# and native binaries (*.so / *.dll) — those go to the GEGL dir only.
copy_plugin_files() {
    local target_dir="$1"
    local item name

    for item in "$SRC_DIR"/* "$SRC_DIR"/.[!.]*; do
        [[ -e "$item" ]] || continue
        name="$(basename "$item")"
        case "$name" in
            install.sh|install.ps1|install.bat|.git|.github|.gitignore|__pycache__|*.zip|*.so|*.dll) continue ;;
        esac
        cp -a "$item" "$target_dir/"
    done
}

install_plugin() {
    local target_dir="$1"

    if [[ -d "$target_dir" ]]; then
        log_info "Replacing existing installation: $target_dir"
        rm -rf "$target_dir"
    else
        log_info "Installing into: $target_dir"
    fi
    mkdir -p "$target_dir"
    copy_plugin_files "$target_dir"
    log_success "Copied plugin files (Python sources only; native binaries go to GEGL dir)."

    # +x only on GIMP 3 entry points; helper modules stay non-executable
    local found_entry=0 py_file fname
    while IFS= read -r py_file; do
        [[ -z "$py_file" ]] && continue
        fname="$(basename "$py_file")"
        if [[ "$fname" == "$PLUGIN_NAME.py" ]] || grep -q "Gimp\.PlugIn" "$py_file" 2>/dev/null; then
            chmod +x "$py_file"
            log_success "Executable permission (+x) granted to entry point: $fname"
            found_entry=1
        fi
    done < <(find "$target_dir" -maxdepth 1 -type f -name "*.py")

    [[ $found_entry -eq 0 ]] && log_warn "No recognized GIMP 3 entry point found in $target_dir."

    if [[ -f "$SRC_DIR/$GEGL_MODULE" ]]; then
        local gegl_path
        while IFS= read -r gegl_path; do
            mkdir -p "$gegl_path"
            cp -f "$SRC_DIR/$GEGL_MODULE" "$gegl_path/"
            log_success "Deployed $GEGL_MODULE -> $gegl_path"
        done < <(get_gegl_dirs)
    else
        log_info "No prebuilt $GEGL_MODULE included; the plugin will compile it on first run (needs gcc + pkg-config + GEGL dev files)."
    fi
}

uninstall() {
    log_warn "Starting uninstallation process..."
    local d f
    while IFS= read -r d; do
        [[ -d "$d" ]] && rm -rf "$d" && log_success "Removed: $d"
    done < <(get_target_plugin_dirs)

    while IFS= read -r d; do
        f="$d/$GEGL_MODULE"
        [[ -f "$f" ]] && rm -f "$f" && log_success "Removed: $f"
    done < <(get_gegl_dirs)

    log_success "Uninstallation completed."
}

main() {
    local action="${1:-install}"

    case "$action" in
        --uninstall|-u|uninstall)
            uninstall
            ;;
        --help|-h|help)
            echo "Usage: $0 [install | --uninstall]"
            ;;
        install|--update|update)
            if [[ ! -f "$SRC_DIR/$PLUGIN_NAME.py" ]]; then
                log_error "$PLUGIN_NAME.py not found next to install.sh ($SRC_DIR)."
                log_error "Run this script from the extracted release folder."
                exit 1
            fi

            log_info "Starting Puppet Warp GIMP plugin deployment..."
            while IFS= read -r target_dir; do
                install_plugin "$target_dir"
            done < <(get_target_plugin_dirs)

            echo ""
            log_success "Process finished! Please restart GIMP to apply changes."
            ;;
        *)
            log_error "Unknown argument: $action"
            echo "Usage: $0 [install | --uninstall]"
            exit 1
            ;;
    esac
}

main "$@"
