# Tier 3 of the conformance harness (ADR-301): the fork's package, installed in
# a pinned Arch Linux guest under QEMU/KVM, drawn on a real virtio-gpu KMS
# device and compared with golden screenshots. See fork/tier3/README.md.
#
#   make pkg            build the package from this checkout -> fork/build/pkg
#   make tier3-image    fetch, verify and provision the guest base image
#   make tier3          install the package in a guest and run every variant
#   make tier3-gl       the same for the GL (virgl) variant alone
#   make tier3-minimal  the minimal build's package (make pkg-minimal) on the
#                       potato machine, with unifont and with a PSF font
#   make tier3-run      a run without rebuilding the package; PKG_DIR=
#                       fork/build/pkg-minimal for the minimal variants
#   make tier3-bless    rewrite the goldens from the last run's captures
#
# JOBS=N runs up to N variants at once, each in its own VM (the vCPUs and
# memory each variant names in fork/tier3/t3_scenario.py);
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
# The commits and pins a report records, so a saved run says what it measured:
# $(call T3_ENV,kmscon tree,libtsm tree,kmscon repository,libtsm repository).
# fork/upstream.mk runs the same scenario on upstream's trees. libtsm's tree
# counts as dirty with untracked files too, as fork/pkg/PKGBUILD records it in
# the package, apart from the hash file meson writes into a wrap's checkout.
T3_DIRTY = $(if $(shell git -C $(1) status --porcelain --untracked-files=$(or $(2),no) -- . \
	':(exclude).meson-subproject-wrap-hash.txt' 2>/dev/null),+dirty)
T3_REV = $(shell git -C $(1) rev-parse --short HEAD 2>/dev/null)$(call T3_DIRTY,$(1),$(2))
T3_ENV = T3_KMSCON_REV=$(call T3_REV,$(1)) T3_LIBTSM_REV=$(call T3_REV,$(2),all) \
	T3_KMSCON_REPO=$(3) T3_LIBTSM_REPO=$(4) \
	DEV_IMAGE_BASE=$(DEV_BASE) DEV_ARCHIVE_DATE=$(DEV_ARCHIVE) \
	T3_IMAGE=$(T3_IMAGE) T3_IMAGE_SHA256=$(T3_IMAGE_SHA256) T3_ARCHIVE=$(T3_ARCHIVE)
T3_RUN = $(call T3_ENV,.,$(or $(LIBTSM),subprojects/libtsm),https://github.com/aaronsb/kmscon,https://github.com/aaronsb/libtsm) \
	$(T3_PY) $(T3_DIR)/t3.py run --jobs $(JOBS) --deadline $(T3_DEADLINE)

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

# The base image is provisioned once per set of inputs (the guest files,
# the pristine image and the archive date) and kept in fork/vm/images under a
# name made from them, which worktrees share by linking that directory from
# the main checkout; fork/vm/base.qcow2 links to it. tier3-image builds it
# only when no checkout has, so a branch that changes the guest's packages
# downloads them once, and every checkout on that branch reuses the build.
.PHONY: tier3-image
tier3-image: $(T3_VM)/images/$(T3_IMAGE) ## Fetch, verify and provision the tier 3 guest base image, or reuse one built from the same inputs
	$(T3_PY) $(T3_DIR)/t3.py provision --image $(T3_IMAGE) --archive $(T3_ARCHIVE)

.PHONY: tier3
tier3: tier3-image ## Tier 3: every variant, or VARIANTS="2d-1280x800 gl-1280x800 ..."; JOBS=N runs N at once
	$(T3_RUN) --pkg-target pkg $(VARIANTS)

.PHONY: tier3-gl
tier3-gl: tier3-image ## Tier 3 on the GL variant (virgl) alone
	$(T3_RUN) --pkg-target pkg gl-1280x800

# The minimal build (ADR-103) under the same package names, in a directory of
# its own, for the potato-minimal variants, which run in their own run since
# every variant of a run installs the same packages.
T3_MINPKGOUT := fork/build/pkg-minimal

.PHONY: pkg-minimal
pkg-minimal: image ## Build the minimal build's package (scripts/profiles/minimal.ini) -> fork/build/pkg-minimal
	mkdir -p $(T3_MINPKGOUT)/work
	docker run --rm --user $(shell id -u):$(shell id -g) -e HOME=/tmp \
		-e KMSCON_SRC=/src -e KMSCON_PKGVER=$(T3_PKGVER) -e KMSCON_PROFILE=minimal \
		-e PKGDEST=/out -e BUILDDIR=/out/work -e SRCDEST=/out/work \
		-v $(CURDIR):/src:ro $(LIBTSM_MOUNT) -v $(CURDIR)/$(T3_MINPKGOUT):/out \
		-w /out/work $(IMAGE) \
		sh -c 'cp /src/fork/pkg/PKGBUILD . && makepkg --nodeps --cleanbuild --force'

.PHONY: tier3-minimal
tier3-minimal: tier3-image ## Tier 3 on the potato machine with the minimal build: unifont and a PSF font
	$(T3_RUN) --pkg-target pkg-minimal --pkg-dir $(T3_MINPKGOUT) \
		$(or $(VARIANTS),2d-potato-min 2d-potato-min-psf)

.PHONY: tier3-run
tier3-run: ## Tier 3 without rebuilding the package (VARIANTS=, JOBS= as for tier3; PKG_DIR=fork/build/pkg-minimal for the minimal variants)
	$(T3_RUN) $(if $(PKG_DIR),--pkg-dir $(PKG_DIR)) $(VARIANTS)

.PHONY: tier3-bless
tier3-bless: ## Copy the last tier 3 run's captures over the goldens
	$(T3_PY) $(T3_DIR)/t3.py bless $(VARIANTS)

.PHONY: tier3-selftest
tier3-selftest: ## Self-test tier 3's image compare module and screen text
	$(T3_PY) $(T3_DIR)/test_t3img.py
	$(T3_PY) $(T3_DIR)/test_t3.py
