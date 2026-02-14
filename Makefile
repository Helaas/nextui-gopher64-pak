SHELL := /bin/bash

PAK_NAME := $(shell jq -r .name pak.json)
PAK_TYPE := $(shell jq -r .type pak.json)
PAK_FOLDER := $(shell echo $(PAK_TYPE) | cut -c1)$(shell echo $(PAK_TYPE) | tr '[:upper:]' '[:lower:]' | cut -c2-)s

ZIP_FILE ?= pak.zip
BUILD_SCRIPT ?= ./build.sh
BINARY ?= bin/tg5050/gopher64
STAGE_ROOT := .build
STAGE_DIR := $(STAGE_ROOT)/$(PAK_NAME).pak

PUSH_SDCARD_PATH ?= /mnt/SDCARD
PUSH_PLATFORM ?= tg5050

ARCHITECTURES := arm64
PLATFORMS := tg5050
# minui-list/minui-presenter/emit-key are published for tg5040 and run on tg5050.
MINUI_TOOLS_SOURCE_PLATFORM ?= tg5040

COREUTILS_VERSION := 0.0.28
EMIT_KEY_VERSION := 0.2.1
EVTEST_VERSION := 0.1.0
JQ_VERSION := 1.7
MINUI_LIST_VERSION := 0.12.0
MINUI_PRESENTER_VERSION := 0.10.0

UTILITY_TARGETS := \
	$(foreach platform,$(PLATFORMS),bin/$(platform)/minui-list bin/$(platform)/minui-presenter bin/$(platform)/emit-key) \
	$(foreach arch,$(ARCHITECTURES),bin/$(arch)/7zzs bin/$(arch)/evtest bin/$(arch)/coreutils bin/$(arch)/jq) \
	bin/arm64/7zzs.LICENSE \
	bin/arm64/coreutils.LICENSE \
	bin/arm64/evtest.LICENSE \
	bin/arm64/jq.LICENSE

PAK_CONTENT := launch.sh pak.json settings.json res data bin

DOCKER_IMAGE ?= gopher64-tg5050-builder
DOCKER_SYSROOT := /opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc
DOCKER_CC := clang --target=aarch64-unknown-linux-gnu --sysroot=$(DOCKER_SYSROOT) -fuse-ld=lld

TEST_TARGETS := tests/drm_plane_scale_test tests/drm_gbm_plane_test tests/drm_setplane_noscale_test

.PHONY: all build build-utils build-tests clean help

all: $(ZIP_FILE)

build-tests: $(TEST_TARGETS)

tests/drm_plane_scale_test: tests/drm_plane_scale_test.c
	docker run --rm -v "$(CURDIR)/tests:/tests" $(DOCKER_IMAGE) \
		$(DOCKER_CC) -o /tests/drm_plane_scale_test /tests/drm_plane_scale_test.c -ldrm

tests/drm_gbm_plane_test: tests/drm_gbm_plane_test.c
	docker run --rm -v "$(CURDIR)/tests:/tests" $(DOCKER_IMAGE) \
		$(DOCKER_CC) -o /tests/drm_gbm_plane_test /tests/drm_gbm_plane_test.c -ldrm -ldl

tests/drm_setplane_noscale_test: tests/drm_setplane_noscale_test.c
	docker run --rm -v "$(CURDIR)/tests:/tests" $(DOCKER_IMAGE) \
		$(DOCKER_CC) -o /tests/drm_setplane_noscale_test /tests/drm_setplane_noscale_test.c -ldrm

build: $(ZIP_FILE)

build-utils: $(UTILITY_TARGETS)

$(BINARY):
	@echo "Missing $(BINARY), running $(BUILD_SCRIPT)"
	@$(BUILD_SCRIPT)

$(ZIP_FILE): $(BINARY) build-utils
	@command -v zip >/dev/null 2>&1 || { echo "zip is required but was not found."; exit 1; }
	@rm -rf "$(STAGE_DIR)"
	@mkdir -p "$(STAGE_DIR)"
	@cp -R $(PAK_CONTENT) "$(STAGE_DIR)/"
	@rm -f "$(ZIP_FILE)"
	@cd "$(STAGE_ROOT)" && zip -qr "../$(ZIP_FILE)" "$(PAK_NAME).pak"
	@echo "Created $(ZIP_FILE)"

bin/arm64/7zzs:
	@set -e; \
	mkdir -p bin/arm64; \
	curl -f -sSL -o bin/arm64/7z.tar.xz "https://www.7-zip.org/a/7z2409-linux-arm64.tar.xz"; \
	rm -rf bin/arm64/7z; \
	mkdir -p bin/arm64/7z; \
	tar -xJf bin/arm64/7z.tar.xz -C bin/arm64/7z; \
	mv -f bin/arm64/7z/7zzs bin/arm64/7zzs; \
	mv -f bin/arm64/7z/License.txt bin/arm64/7zzs.LICENSE; \
	chmod +x bin/arm64/7zzs; \
	rm -rf bin/arm64/7z bin/arm64/7z.tar.xz

bin/arm64/coreutils:
	@set -e; \
	mkdir -p bin/arm64; \
	curl -f -sSL -o bin/arm64/coreutils.tar.gz "https://github.com/uutils/coreutils/releases/download/$(COREUTILS_VERSION)/coreutils-$(COREUTILS_VERSION)-aarch64-unknown-linux-gnu.tar.gz"; \
	rm -rf bin/arm64/coreutils.tmp; \
	mkdir -p bin/arm64/coreutils.tmp; \
	tar -xzf bin/arm64/coreutils.tar.gz -C bin/arm64/coreutils.tmp --strip-components=1; \
	mv -f bin/arm64/coreutils.tmp/coreutils bin/arm64/coreutils; \
	if [ -f bin/arm64/coreutils.tmp/LICENSE ]; then mv -f bin/arm64/coreutils.tmp/LICENSE bin/arm64/coreutils.LICENSE; fi; \
	chmod +x bin/arm64/coreutils; \
	rm -rf bin/arm64/coreutils.tmp bin/arm64/coreutils.tar.gz

bin/%/evtest:
	@mkdir -p bin/$*
	@curl -f -sSL -o bin/$*/evtest "https://github.com/josegonzalez/compiled-evtest/releases/download/$(EVTEST_VERSION)/evtest-$*"
	@curl -f -sSL -o bin/$*/evtest.LICENSE "https://raw.githubusercontent.com/freedesktop-unofficial-mirror/evtest/refs/heads/master/COPYING"
	@chmod +x bin/$*/evtest

bin/arm64/jq:
	@mkdir -p bin/arm64
	@curl -f -sSL -o bin/arm64/jq "https://github.com/jqlang/jq/releases/download/jq-$(JQ_VERSION)/jq-linux-arm64"
	@curl -f -sSL -o bin/arm64/jq.LICENSE "https://raw.githubusercontent.com/jqlang/jq/jq-$(JQ_VERSION)/COPYING"
	@chmod +x bin/arm64/jq

bin/%/emit-key:
	@mkdir -p bin/$*
	@curl -f -sSL -o bin/$*/emit-key "https://github.com/josegonzalez/emit-key/releases/download/$(EMIT_KEY_VERSION)/emit-key-$(MINUI_TOOLS_SOURCE_PLATFORM)"
	@chmod +x bin/$*/emit-key

bin/%/minui-list:
	@mkdir -p bin/$*
	@curl -f -sSL -o bin/$*/minui-list "https://github.com/josegonzalez/minui-list/releases/download/$(MINUI_LIST_VERSION)/minui-list-$(MINUI_TOOLS_SOURCE_PLATFORM)"
	@chmod +x bin/$*/minui-list

bin/%/minui-presenter:
	@mkdir -p bin/$*
	@curl -f -sSL -o bin/$*/minui-presenter "https://github.com/josegonzalez/minui-presenter/releases/download/$(MINUI_PRESENTER_VERSION)/minui-presenter-$(MINUI_TOOLS_SOURCE_PLATFORM)"
	@chmod +x bin/$*/minui-presenter

bin/arm64/7zzs.LICENSE: bin/arm64/7zzs
	@test -f "$@"

bin/arm64/coreutils.LICENSE: bin/arm64/coreutils
	@test -f "$@"

bin/arm64/evtest.LICENSE: bin/arm64/evtest
	@test -f "$@"

bin/arm64/jq.LICENSE: bin/arm64/jq
	@test -f "$@"

clean:
	@rm -rf "$(STAGE_ROOT)" "$(ZIP_FILE)"
	@rm -f bin/*/7zzs bin/*/7zzs.LICENSE
	@rm -f bin/*/evtest bin/*/evtest.LICENSE
	@rm -f bin/*/jq bin/*/jq.LICENSE
	@rm -f bin/*/minui-list bin/*/minui-presenter
	@rm -f bin/*/coreutils bin/*/coreutils.LICENSE
	@rm -f bin/*/emit-key
	@rm -f bin/*/coreutils.tar.gz bin/*/7z.tar.xz
	@rm -rf bin/*/7z bin/*/coreutils.tmp
	@echo "Cleaned build artifacts"

help:
	@echo "Targets:"
	@echo "  make / make build     Build $(ZIP_FILE)"
	@echo "  make build-utils      Download helper binaries"
	@echo "  make clean            Remove staged files, $(ZIP_FILE), and downloaded helper binaries"
	@echo "Variables:"
	@echo "  ZIP_FILE=<name>.zip"
	@echo "  MINUI_TOOLS_SOURCE_PLATFORM=tg5040"
