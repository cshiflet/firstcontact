# FirstContact developer Makefile.
# A thin wrapper around the CMake build so the common workflows are a one-liner.
# Run `make help` to list every target.
#
# Standard flow on a fresh box:
#   make setup     # install distro packages + fetch linuxdeploy
#   make build     # debug build
#   make test      # ctest
#   make appimage  # produces FirstContact-<ver>-<arch>.AppImage

# ---- Configurable knobs (override on the command line) --------------------
#   make build BUILD_TYPE=Release SANITIZERS=ON
BUILD_DIR        ?= build
RELEASE_DIR      ?= build-release
BUILD_TYPE       ?= Debug
GENERATOR        ?= Ninja
APP_NAME         ?= FirstContact
APP_VERSION      ?= 0.1.0
ARCH             := $(shell uname -m)
APPIMAGE         := $(APP_NAME)-$(APP_VERSION)-$(ARCH).AppImage
LINUXDEPLOY_DIR  ?= $(HOME)/.local/bin
SANITIZERS       ?= OFF
LOCK_FILE        := $(HOME)/.local/share/FirstContact/firstcontact.lock

CMAKE_FLAGS = -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
              -DFC_ENABLE_SANITIZERS=$(SANITIZERS)

# Treat every target as PHONY by default — none of them produce a single file
# the way Make's default rule expects.
.PHONY: help \
        setup setup-ubuntu setup-fedora setup-arch tools \
        configure build run smoke test \
        release-config release install appimage \
        clean distclean format

# ---- Help -----------------------------------------------------------------

help:  ## Show this help.
	@awk 'BEGIN {FS = ":.*?##"; print "Targets:"} \
	    /^[a-zA-Z][a-zA-Z0-9_-]*:.*?##/ \
	      { printf "  \033[36m%-18s\033[0m %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@echo
	@echo "Knobs (override on command line):"
	@echo "  BUILD_TYPE=$(BUILD_TYPE)        SANITIZERS=$(SANITIZERS)"
	@echo "  BUILD_DIR=$(BUILD_DIR)          RELEASE_DIR=$(RELEASE_DIR)"
	@echo "  APP_VERSION=$(APP_VERSION)"

# ---- Dependency installation ----------------------------------------------

setup: tools ## Install distro build deps + fetch linuxdeploy. Auto-detects distro.
	@if   [ -f /etc/debian_version ]; then $(MAKE) --no-print-directory setup-ubuntu; \
	elif  [ -f /etc/fedora-release ]; then $(MAKE) --no-print-directory setup-fedora; \
	elif  [ -f /etc/arch-release   ]; then $(MAKE) --no-print-directory setup-arch;   \
	else \
	    echo "Unknown distro. Install: Qt6 (Core/Gui/Widgets/Network/NetworkAuth/Sql/"; \
	    echo "  Concurrent), qtkeychain-qt6, libsecret, sqlite,"; \
	    echo "  cmake>=3.24, ninja, g++ with C++20, librsvg2 (icon), appstream (validation)."; \
	    exit 1; \
	fi

setup-ubuntu: ## Install build deps via apt (Ubuntu 24.04+, Debian 13+).
	sudo apt update && sudo apt install -y \
	    build-essential cmake ninja-build pkg-config git curl \
	    qt6-base-dev qt6-base-dev-tools qt6-tools-dev qt6-tools-dev-tools \
	    qt6-networkauth-dev libqt6sql6-sqlite \
	    qt6-l10n-tools qt6-declarative-dev \
	    qt6-svg-dev libqt6svg6 \
	    libsecret-1-dev qtkeychain-qt6-dev \
	    libsqlite3-dev sqlite3 \
	    libzstd-dev \
	    libxkbcommon-dev \
	    librsvg2-bin appstream xvfb \
	    xdg-utils
	@# wslu provides `wslview` (WSL → Windows browser bridge). Harmless on
	@# non-WSL boxes — apt picks it up if available, ignored otherwise.
	@if grep -qiE 'microsoft|wsl' /proc/version 2>/dev/null; then \
	    sudo apt install -y wslu; \
	fi

setup-fedora: ## Install build deps via dnf (Fedora 40+).
	sudo dnf install -y \
	    cmake ninja-build gcc-c++ git curl \
	    qt6-qtbase-devel qt6-qttools-devel qt6-qtnetworkauth-devel \
	    qt6-qtdeclarative-devel qt6-qt5compat-devel \
	    qt6-qtsvg-devel \
	    qt6-qtkeychain-devel libsecret-devel sqlite-devel \
	    libzstd-devel \
	    fuse-libs librsvg2-tools appstream xorg-x11-server-Xvfb

setup-arch: ## Install build deps via pacman (Arch / Manjaro).
	sudo pacman -S --needed --noconfirm \
	    base-devel cmake ninja git curl \
	    qt6-base qt6-tools qt6-networkauth qt6-declarative \
	    qt6-svg \
	    qtkeychain-qt6 libsecret sqlite zstd \
	    librsvg appstream xorg-server-xvfb

tools: $(LINUXDEPLOY_DIR)/linuxdeploy $(LINUXDEPLOY_DIR)/linuxdeploy-plugin-qt  ## Fetch linuxdeploy + qt plugin into ~/.local/bin.

$(LINUXDEPLOY_DIR)/linuxdeploy:
	@mkdir -p $(LINUXDEPLOY_DIR)
	@echo "→ fetching linuxdeploy"
	curl -fsSL -o $@ \
	    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
	chmod +x $@

$(LINUXDEPLOY_DIR)/linuxdeploy-plugin-qt:
	@mkdir -p $(LINUXDEPLOY_DIR)
	@echo "→ fetching linuxdeploy-plugin-qt"
	curl -fsSL -o $@ \
	    https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
	chmod +x $@

# ---- Build / test ---------------------------------------------------------

configure: ## Configure CMake into $(BUILD_DIR). Uses BUILD_TYPE / SANITIZERS.
	cmake -S . -B $(BUILD_DIR) -G $(GENERATOR) $(CMAKE_FLAGS)

build: configure ## Compile (defaults to Debug; override with BUILD_TYPE=Release).
	cmake --build $(BUILD_DIR)

run: build ## Launch firstcontact from the build tree.
	$(BUILD_DIR)/src/app/firstcontact

run-dry: build ## Launch with FC_DRY_RUN=1 — destructive ops (delete, label edit, star/archive/trash, pending-ops flush) are blocked.
	FC_DRY_RUN=1 $(BUILD_DIR)/src/app/firstcontact

smoke: build ## Headless smoke launch — boots the GUI under offscreen for 4s.
	@rm -f $(LOCK_FILE) 2>/dev/null || true
	@bash -c 'QT_QPA_PLATFORM=offscreen timeout --signal=TERM 4 \
	            $(BUILD_DIR)/src/app/firstcontact >/dev/null 2>&1; \
	          rc=$$?; \
	          if [ $$rc -eq 124 ] || [ $$rc -eq 143 ]; then \
	            echo "✓ smoke ok (event loop ran for 4s)"; exit 0; \
	          else \
	            echo "✗ smoke FAILED: exited $$rc within 4s"; exit 1; \
	          fi'

test: build ## Build and run the unit tests under ctest.
	QT_QPA_PLATFORM=offscreen ctest --test-dir $(BUILD_DIR) --output-on-failure

# ---- Release + AppImage ---------------------------------------------------

release-config:
	cmake -S . -B $(RELEASE_DIR) -G $(GENERATOR) \
	    -DCMAKE_BUILD_TYPE=Release \
	    -DCMAKE_INSTALL_PREFIX=/usr \
	    -DFC_BUILD_TESTS=OFF

release: release-config ## Configure + build Release into $(RELEASE_DIR).
	cmake --build $(RELEASE_DIR)

install: release ## Stage the Release install tree into AppDir/usr/...
	rm -rf AppDir
	DESTDIR=$(CURDIR)/AppDir cmake --install $(RELEASE_DIR)

appimage: install tools ## Build the portable AppImage (single executable file).
	@echo "→ bundling Qt + deps with linuxdeploy"
	@PATH="$(LINUXDEPLOY_DIR):$$PATH" \
	  LINUXDEPLOY_OUTPUT_VERSION=$(APP_VERSION) \
	  linuxdeploy --appdir AppDir --plugin qt --output appimage
	@echo
	@ls -lh $(APPIMAGE) 2>/dev/null && \
	  echo "→ Built $(APPIMAGE)" && \
	  echo "  Run: ./$(APPIMAGE)"

# ---- Cleanup --------------------------------------------------------------

clean: ## Delete $(BUILD_DIR).
	rm -rf $(BUILD_DIR)

distclean: clean ## Delete every build artifact: $(BUILD_DIR), $(RELEASE_DIR), AppDir, *.AppImage.
	rm -rf $(RELEASE_DIR) AppDir *.AppImage

format: ## Run clang-format over src/ and tests/ in place (no-op if clang-format missing).
	@command -v clang-format >/dev/null 2>&1 || { \
	    echo "clang-format not installed (apt install clang-format) — skipping"; exit 0; }
	find src tests -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
	    | xargs -0 clang-format -i
	@echo "✓ formatted"
