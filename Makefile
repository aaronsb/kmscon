# kmscon fork — development build
#
# Every step runs in the fork/dev.Dockerfile image as the invoking user, with
# this checkout bind-mounted, so the host needs only docker and make.
#
# kmscon is built against the libtsm fork as a static subproject: the system
# libtsm is never used, and nothing from libtsm is installed. By default meson
# fetches the fork named in subprojects/libtsm.wrap. Set LIBTSM to a local
# checkout to build against uncommitted libtsm work instead:
#
#   make test LIBTSM=../libtsm
#
#   make image   build the dev image
#   make build   configure and compile into fork/build
#   make test    build, then run kmscon's and libtsm's test suites
#   make clean   remove fork/build
#
# Further targets (conformance tiers, packaging) live in fork/*.mk, one file
# per concern, so they can be added without editing this file.

IMAGE  ?= kmscon-dev
BUILD  := fork/build
LIBTSM ?=

SETUP_ARGS := --force-fallback-for=libtsm -Dlibtsm:default_library=static

ifneq ($(LIBTSM),)
LIBTSM_MOUNT := -v $(abspath $(LIBTSM)):/src/subprojects/libtsm:ro
endif

RUN := docker run --rm --user $(shell id -u):$(shell id -g) -e HOME=/tmp \
	-v $(CURDIR):/src $(LIBTSM_MOUNT) -w /src $(IMAGE)

.DEFAULT_GOAL := help

.PHONY: help
help: ## Show this help
	@awk 'BEGIN{FS=":.*##"} /^[a-zA-Z0-9_-]+:.*##/ {printf "  \033[36m%-8s\033[0m %s\n", $$1, $$2}' $(MAKEFILE_LIST)

.PHONY: image
image: ## Build the dev image
	docker build -t $(IMAGE) -f fork/dev.Dockerfile fork

.PHONY: build
build: image ## Configure and compile into fork/build
	$(RUN) sh -c '[ -f $(BUILD)/build.ninja ] || meson setup $(BUILD) $(SETUP_ARGS); meson compile -C $(BUILD)'

.PHONY: test
test: build ## Run kmscon's and libtsm's test suites
	$(RUN) meson test -C $(BUILD) --print-errorlogs

.PHONY: clean
clean: ## Remove fork/build
	rm -rf $(BUILD)

-include fork/*.mk
