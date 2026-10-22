# Tier 3 of the conformance harness (ADR-301): the fork's package, installed in
# a pinned Arch Linux guest under QEMU/KVM, drawn on a real virtio-gpu KMS
# device and compared with golden screenshots. See fork/tier3/README.md.
#
#   make pkg            build the package from this checkout -> fork/build/pkg
#   make tier3-image    fetch, verify and provision the guest base image
#   make tier3          install the package in a guest and run the 2D variants
#   make tier3-gl       the same for the GL (virgl) variant, see #17
#   make tier3-bless    rewrite the goldens from the last run's captures
#
# JOBS=N runs up to N variants at once, each in its own VM (4 vCPUs, 4 GiB);
# the default, 1, runs them one after another. GL variants never run together.
# With JOBS above 1, T3_DEADLINE=S stops a variant still running after S seconds.
#
# Everything the guest needs lives in fork/vm (gitignored). QEMU runs headless
# as the invoking user.

T3_DIR     := fork/tier3
T3_VM      := fork/vm
T3_PKGOUT  := fork/build/pkg
T3_PKGVER  := $(shell git describe --tags --long 2>/dev/null | sed 's/^v//;s/-\([0-9]*\)-g/.r\1.g/')
T3_PY      := python3 -I -B
JOBS       ?= 1
# Seconds a variant may run, with JOBS above 1, before its worker is stopped.
T3_DEADLINE ?= 1800
# tier3 and tier3-gl name the package target for t3.py, which builds it under
# the run lock, so a second run cannot rebuild the package under a running
# one. The recipes hold no $(MAKE), so `make -n` runs nothing.
# The commits and pins a report records, so a saved run says what it measured.
T3_DIRTY = $(if $(shell git -C $(1) status --porcelain --untracked-files=no 2>/dev/null),+dirty)
T3_ENV = T3_KMSCON_REV=$(shell git rev-parse --short HEAD)$(call T3_DIRTY,.) \
	T3_LIBTSM_REV=$(shell git -C $(or $(LIBTSM),subprojects/libtsm) rev-parse --short HEAD 2>/dev/null)$(call T3_DIRTY,$(or $(LIBTSM),subprojects/libtsm)) \
	DEV_IMAGE_BASE=$(DEV_BASE) DEV_ARCHIVE_DATE=$(DEV_ARCHIVE) \
	T3_IMAGE=$(T3_IMAGE) T3_IMAGE_SHA256=$(T3_IMAGE_SHA256) T3_ARCHIVE=$(T3_ARCHIVE)
T3_RUN = $(T3_ENV) $(T3_PY) $(T3_DIR)/t3.py run --jobs $(JOBS) --deadline $(T3_DEADLINE)

# Pinned guest: the official Arch Linux cloud image, verified against the
# arch-boxes signing key, and the Arch Linux Archive snapshot its packages
# come from, so a rerun installs the same package versions.
T3_IMAGE_VER   := 20261015.611680
T3_IMAGE       := Arch-Linux-x86_64-cloudimg-$(T3_IMAGE_VER).qcow2
T3_IMAGE_URL   := https://geo.mirror.pkgbuild.com/images/v$(T3_IMAGE_VER)
T3_IMAGE_SHA256 := 08754e28d91b1680086c6211ef4df13559e766fff7cdb4006489b14b5d281f08
T3_IMAGE_KEY   := 1B9A16984A4E8CB448712D2AE0B78BF4326C6F8F
T3_ARCHIVE     := 2026/10/20

.PHONY: pkg
pkg: image ## Build the kmscon-aaronsb package from this checkout -> fork/build/pkg
	mkdir -p $(T3_PKGOUT)/work
	docker run --rm --user $(shell id -u):$(shell id -g) -e HOME=/tmp \
		-e KMSCON_SRC=/src -e KMSCON_PKGVER=$(T3_PKGVER) \
		-e PKGDEST=/out -e BUILDDIR=/out/work -e SRCDEST=/out/work \
		-v $(CURDIR):/src:ro $(LIBTSM_MOUNT) -v $(CURDIR)/$(T3_PKGOUT):/out \
		-w /out/work $(IMAGE) \
		sh -c 'cp /src/fork/pkg/PKGBUILD . && makepkg --nodeps --cleanbuild --force'

T3_BASE := $(T3_VM)/base.qcow2
T3_GUEST := $(wildcard $(T3_DIR)/guest/user-data.in $(T3_DIR)/guest/meta-data \
	$(T3_DIR)/guest/tier3-install $(T3_DIR)/guest/tier3-install.service)

$(T3_VM)/images/$(T3_IMAGE):
	$(T3_DIR)/fetch-image.sh $(T3_VM)/images $(T3_IMAGE_URL) $(T3_IMAGE) \
		$(T3_IMAGE_SHA256) $(T3_IMAGE_KEY)

$(T3_BASE): $(T3_VM)/images/$(T3_IMAGE) $(T3_GUEST)
	$(T3_PY) $(T3_DIR)/t3.py provision --image $(T3_IMAGE) --archive $(T3_ARCHIVE)

.PHONY: tier3-image
tier3-image: $(T3_BASE) ## Fetch, verify and provision the tier 3 guest base image

.PHONY: tier3
tier3: $(T3_BASE) ## Tier 3: the 2D variants, or VARIANTS="2d-1280x800 gl-1280x800 ..."; JOBS=N runs N at once
	$(T3_RUN) --pkg-target pkg $(VARIANTS)

.PHONY: tier3-gl
tier3-gl: $(T3_BASE) ## Tier 3 on the GL variant (virgl), outside the default run: #17
	$(T3_RUN) --pkg-target pkg gl-1280x800

.PHONY: tier3-run
tier3-run: ## Tier 3 without rebuilding the package (VARIANTS=, JOBS= as for tier3)
	$(T3_RUN) $(VARIANTS)

.PHONY: tier3-bless
tier3-bless: ## Copy the last tier 3 run's captures over the goldens
	$(T3_PY) $(T3_DIR)/t3.py bless $(VARIANTS)

.PHONY: tier3-selftest
tier3-selftest: ## Self-test tier 3's image compare module and screen text
	$(T3_PY) $(T3_DIR)/test_t3img.py
	$(T3_PY) $(T3_DIR)/test_t3.py
