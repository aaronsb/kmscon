# Upstream kmscon through tier 3, for the comparison page that sets upstream
# beside the fork (docs/fork/conformance.md, "Comparison pages"). Upstream is
# packaged with the fork's recipe and run in the same guest, on the same
# variants and screens, as `make tier3` runs the fork.
#
#   make upstream-src     check out upstream kmscon and libtsm at the pins below
#                         in fork/build/upstream/kmscon, libtsm as its
#                         subprojects/libtsm
#   make pkg-upstream     build kmscon-upstream from them -> fork/build/pkg-upstream
#   make tier3-upstream   install it in the tier 3 guest and run the 2D variants
#
# The run's report and captures go where `make tier3` puts the fork's
# (fork/build/tier3-report.json, fork/build/tier3/), so save each run with
# `make sheet-save` before the next. The goldens are the fork's, so a case
# where upstream draws differently reports `fail`; tier3-upstream fails only
# when a case has no capture. Any pin can be overridden on the command line,
# such as UP_KMSCON_REV=v10.0.4.

# Upstream's main on 2026-10-22, the newest commit when the comparison was first
# run: the page compares the fork with upstream as it is, not its last release.
UP_KMSCON_URL := https://github.com/kmscon/kmscon
UP_KMSCON_REV := 7d3377946627ea0333eb4c39f9d6257eb1fd09c1
# libtsm v4.8.0: the release upstream's own subprojects/libtsm.wrap names, and
# the tag the fork's tier 1 goldens come from.
UP_LIBTSM_URL := https://github.com/kmscon/libtsm
UP_LIBTSM_REV := ef2365d0f9f1a370d721a82470d69c3646fe97f0

UP_KMSCON := fork/build/upstream/kmscon
UP_LIBTSM := $(UP_KMSCON)/subprojects/libtsm
UP_PKGOUT := fork/build/pkg-upstream
# Recursive, so the version is read after upstream-src has checked out the tree.
UP_PKGVER  = $(shell git -C $(UP_KMSCON) describe --tags --long | sed 's/^v//;s/-\([0-9]*\)-g/.r\1.g/')

.PHONY: upstream-src
upstream-src: ## Check out upstream kmscon and libtsm at the pins in fork/upstream.mk
	mkdir -p $(dir $(UP_KMSCON))
	fork/pkg/upstream/fetch.sh $(UP_KMSCON) $(UP_KMSCON_URL) $(UP_KMSCON_REV)
	fork/pkg/upstream/fetch.sh $(UP_LIBTSM) $(UP_LIBTSM_URL) $(UP_LIBTSM_REV)

# As `make pkg`, in its own work directory, so no object of the fork's build
# can reach upstream's (#38). The tree is mounted read-only and the fork's
# recipe beside upstream's.
.PHONY: pkg-upstream
pkg-upstream: image upstream-src ## Build the kmscon-upstream package -> fork/build/pkg-upstream
	mkdir -p $(UP_PKGOUT)/work
	docker run --rm --user $(shell id -u):$(shell id -g) -e HOME=/tmp \
		-e KMSCON_SRC=/src -e KMSCON_PKGVER=$(UP_PKGVER) \
		-e PKGDEST=/out -e BUILDDIR=/out/work -e SRCDEST=/out/work \
		-v $(abspath $(UP_KMSCON)):/src:ro -v $(CURDIR)/fork/pkg:/recipe:ro \
		-v $(CURDIR)/$(UP_PKGOUT):/out -w /out/work $(IMAGE) \
		sh -c 'cp /recipe/upstream/PKGBUILD . && cp /recipe/PKGBUILD fork.PKGBUILD && makepkg --nodeps --cleanbuild --force'

.PHONY: tier3-upstream
tier3-upstream: tier3-image upstream-src ## Tier 3 on upstream kmscon and libtsm (pins in fork/upstream.mk), for sheet-compare
	$(call T3_ENV,$(UP_KMSCON),$(UP_LIBTSM),$(UP_KMSCON_URL),$(UP_LIBTSM_URL)) \
		$(T3_PY) $(T3_DIR)/t3.py run --jobs $(JOBS) --deadline $(T3_DEADLINE) \
		--pkg-target pkg-upstream --pkg-dir $(UP_PKGOUT) --pkg-name kmscon-upstream \
		--compare-only $(VARIANTS)
