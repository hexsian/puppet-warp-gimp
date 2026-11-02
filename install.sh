#!/usr/bin/env bash
#
# Puppet Warp for GIMP - Installer & Updater Script
# Repository: https://github.com/hexsian/puppet-warp-gimp
#
# Usage:
#   ./install.sh             # Install or update automatically
#   ./install.sh --update    # Force pull latest updates
#   ./install.sh --uninstall # Remove the plugin and GEGL binaries
#

set -euo pipefail

# --- Configuration & Constants ---
readonly REPO_URL="https://github.com/hexsian/puppet-warp-gimp.git"
readonly PLUGIN_NAME="puppet-warp"
readonly GEGL_MODULE="puppet-warp.so"

# Terminal formatting (auto-disable if stdout is not a TTY)
if [[ -t 1 ]]; then
    readonly C_RESET='\033[0m'
    readonly C_RED='\033[0;31m'
    readonly C_GREEN='\033[0;32m'
    readonly C_YELLOW='\033[1;33m'
    readonly C_BLUE='\033[0;34m'
else
    readonly C_RESET=''
    readonly C_RED=''
    readonly C_GREEN=''
    readonly C_YELLOW=''
    readonly C_BLUE=''
fi

# --- Logging Helpers ---
log_info()    { printf "%b[INFO]%b  %s\n" "${C_BLUE}" "${C_RESET}" "$*"; }
log_success() { printf "%b[OK]%b    %s\n" "${C_GREEN}" "${C_RESET}" "$*"; }
log_warn()    { printf "%b[WARN]%b  %s\n" "${C_YELLOW}" "${C_RESET}" "$*"; }
log_error()   { printf "%b[ERROR]%b %s\n" "${C_RED}" "${C_RESET}" "$*" >&2; }

# --- System Checks ---
check_dependencies() {
    local missing=()
    for cmd in git find grep; do
        if ! command -v "$cmd" &>/dev/null; then
            missing+=("$cmd")
        fi
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        log_error "Missing required dependencies: ${missing[*]}"
        log_error "Please install them via your distribution package manager."
        exit 1
    fi
}

# --- Directory Discovery ---
# Discovers target plug-in directories for Native, Flatpak, and Snap versions of GIMP 3.x
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
                if [[ -d "$ver_dir" ]]; then
                    dirs+=("$ver_dir/plug-ins/$PLUGIN_NAME")
                fi
            done
        fi
    done

    # Fallback to standard GIMP 3 locations if no runtime directory is detected
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

# --- Core Actions ---
install_or_update() {
    local target_dir="$1"
    local parent_dir
    parent_dir="$(dirname "$target_dir")"
    mkdir -p "$parent_dir"

    if [[ -d "$target_dir/.git" ]]; then
        log_info "Existing Git installation detected at: $target_dir"
        log_info "Fetching latest updates..."
        git -C "$target_dir" fetch --quiet origin
        git -C "$target_dir" pull --ff-only --quiet || git -C "$target_dir" reset --hard origin/HEAD --quiet
        log_success "Updated $PLUGIN_NAME to latest commit."
    elif [[ -d "$target_dir" ]]; then
        log_warn "Directory exists without Git metadata: $target_dir"
        log_info "Reinstalling repository..."
        rm -rf "$target_dir"
        git clone --quiet "$REPO_URL" "$target_dir"
        log_success "Reinstalled successfully."
    else
        log_info "Installing into: $target_dir"
        git clone --quiet "$REPO_URL" "$target_dir"
        log_success "Installed successfully."
    fi

    # Detect and grant executable permissions (+x) only to GIMP 3 entry points
    # Helper modules (e.g., utils.py) remain non-executable to prevent GIMP query conflicts
    local found_entry=0
    while IFS= read -r py_file; do
        [[ -z "$py_file" ]] && continue
        local fname
        fname="$(basename "$py_file")"

        # Check if the file matches plugin directory name or registers with GIMP 3 API
        if [[ "$fname" == "$PLUGIN_NAME.py" ]] || grep -q "Gimp\.PlugIn" "$py_file" 2>/dev/null; then
            chmod +x "$py_file"
            log_success "Executable permission (+x) granted to entry point: $fname"
            found_entry=1
        fi
    done < <(find "$target_dir" -maxdepth 1 -type f -name "*.py")

    if [[ $found_entry -eq 0 ]]; then
        log_warn "No recognized GIMP 3 entry point found in $target_dir."
    fi

    # Link/Copy prebuilt GEGL shared library if provided in the repo
    if [[ -f "$target_dir/$GEGL_MODULE" ]]; then
        while IFS= read -r gegl_path; do
            mkdir -p "$gegl_path"
            cp -f "$target_dir/$GEGL_MODULE" "$gegl_path/"
            log_success "Deployed $GEGL_MODULE -> $gegl_path"
        done < <(get_gegl_dirs)
    fi
}

uninstall() {
    log_warn "Starting uninstallation process..."

    while IFS= read -r target_dir; do
        if [[ -d "$target_dir" ]]; then
            rm -rf "$target_dir"
            log_success "Removed: $target_dir"
        fi
    done < <(get_target_plugin_dirs)

    while IFS= read -r gegl_dir; do
        local target_file="$gegl_dir/$GEGL_MODULE"
        if [[ -f "$target_file" ]]; then
            rm -f "$target_file"
            log_success "Removed: $target_file"
        fi
    done < <(get_gegl_dirs)

    log_success "Uninstallation completed."
}

# --- Main Entry Point ---
main() {
    local action="${1:-install}"

    case "$action" in
        --uninstall|-u|uninstall)
            uninstall
            ;;
        --help|-h|help)
            echo "Usage: $0 [install | --update | --uninstall]"
            exit 0
            ;;
        install|--update|update)
            check_dependencies

            log_info "Starting Puppet Warp GIMP plugin deployment..."
            while IFS= read -r target_dir; do
                install_or_update "$target_dir"
            done < <(get_target_plugin_dirs)

            echo ""
            log_success "Process finished! Please restart GIMP to apply changes."
            log_info "To update in the future, simply run this script again."
            ;;
        *)
            log_error "Unknown argument: $action"
            echo "Usage: $0 [install | --update | --uninstall]"
            exit 1
            ;;
    esac
}

main "$@"
