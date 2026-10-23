# The format checks the GitHub workflow runs (.github/workflows/meson.yml),
# locally and at the same tool versions, so a change that would turn CI red
# fails here first.
#
#   make lint     clang-format and meson format checks, changing nothing
#   make format   rewrite the files those checks would flag
#
# `make test` runs `make lint` too. Both run in their own image
# (fork/lint.Dockerfile), the one place their versions are set besides the
# workflow: a version moved here moves there in the same commit.

# Ubuntu 24.04, as GitHub's ubuntu-latest runner, whose clang-format-20 the
# workflow installs.
LINT_BASE            := ubuntu@sha256:561618e2c15bf2397621dd04f96926663a3b5616c189cf7e38db7e82f5c538ea
CLANG_FORMAT_VERSION := 20.1.2
MESON_FORMAT_VERSION := 1.12.1
LINT_IMAGE           ?= kmscon-lint

# The workflow's file list, expanded by the container's shell.
LINT_C_FILES := src/*.c src/*.h src/*/*.c src/*/*.h tests/*.c tests/*.h

LINT_RUN := docker run --rm --user $(shell id -u):$(shell id -g) -e HOME=/tmp \
	-v $(CURDIR):/src -w /src $(LINT_IMAGE)

.PHONY: lint-image
lint-image:
	docker build -q -t $(LINT_IMAGE) --build-arg BASE=$(LINT_BASE) \
		--build-arg CLANG_FORMAT_VERSION=$(CLANG_FORMAT_VERSION) \
		--build-arg MESON_FORMAT_VERSION=$(MESON_FORMAT_VERSION) \
		-f fork/lint.Dockerfile fork >/dev/null

.PHONY: lint
lint: lint-image ## Run CI's clang-format and meson format checks
	$(LINT_RUN) sh -c 'clang-format-20 -n --Werror $(LINT_C_FILES)'
	$(LINT_RUN) meson format --recursive --check-diff

.PHONY: format
format: lint-image ## Reformat what make lint flags
	$(LINT_RUN) sh -c 'clang-format-20 -i $(LINT_C_FILES)'
	$(LINT_RUN) meson format --recursive --inplace

test: lint
