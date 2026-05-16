#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is not installed. Installing Homebrew..."
    NONINTERACTIVE=1 /bin/bash -c \
        "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
fi

brew_bin="$(command -v brew)"
brew_prefix="$("$brew_bin" --prefix)"

packages=(
    cmake
    ninja
    pkgconf
    git
    qt
    qtkeychain
    zstd
)

for pkg in "${packages[@]}"; do
    if "$brew_bin" list --formula "$pkg" >/dev/null 2>&1; then
        echo "✓ $pkg already installed"
    else
        echo "→ installing $pkg"
        "$brew_bin" install "$pkg"
    fi
done

qt_prefix="$("$brew_bin" --prefix qt)"
qtkeychain_prefix="$("$brew_bin" --prefix qtkeychain)"
zstd_prefix="$("$brew_bin" --prefix zstd)"
pkgconf_prefix="$("$brew_bin" --prefix pkgconf)"

cat > "$repo_root/CMakeUserPresets.json" <<JSON
{
    "version": 6,
    "configurePresets": [
        {
            "name": "macos-debug-local",
            "displayName": "macOS Debug (local Homebrew)",
            "inherits": "macos-debug",
            "cacheVariables": {
                "CMAKE_PREFIX_PATH": "$qt_prefix;$qtkeychain_prefix;$zstd_prefix",
                "PKG_CONFIG_EXECUTABLE": "$pkgconf_prefix/bin/pkgconf"
            }
        },
        {
            "name": "macos-release-local",
            "displayName": "macOS Release (local Homebrew)",
            "inherits": "macos-release",
            "cacheVariables": {
                "CMAKE_PREFIX_PATH": "$qt_prefix;$qtkeychain_prefix;$zstd_prefix",
                "PKG_CONFIG_EXECUTABLE": "$pkgconf_prefix/bin/pkgconf"
            }
        }
    ],
    "buildPresets": [
        { "name": "macos-debug-local", "configurePreset": "macos-debug-local" },
        { "name": "macos-release-local", "configurePreset": "macos-release-local" }
    ],
    "testPresets": [
        {
            "name": "macos-debug-local",
            "configurePreset": "macos-debug-local",
            "output": { "outputOnFailure": true }
        }
    ]
}
JSON

echo
echo "macOS setup complete."
echo "Configure: cmake --preset macos-debug-local"
echo "Build:     cmake --build --preset macos-debug-local"
echo "Test:      ctest --preset macos-debug-local"
