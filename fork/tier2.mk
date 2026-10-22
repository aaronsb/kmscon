# Tier 2 (ADR-301): pixel comparison of the renderer drawing into memory.
#
#   make tier2          render the screens and compare with tests/goldens/tier2
#   make tier2-golden   regenerate the goldens (review the images, then commit
#                       them on their own)
#
# Both run in the dev image. `make test` runs the same comparison through
# meson. The report is written to fork/build/tier2-report.json.

# The libtsm the renderer is built against: the LIBTSM checkout when set, else
# the subproject. Read on the host, because a worktree's .git does not resolve
# inside the container.
T2_LIBTSM_DIR := $(or $(LIBTSM),subprojects/libtsm)
T2_LIBTSM_REV  = $(shell git -C $(T2_LIBTSM_DIR) rev-parse HEAD 2>/dev/null)$(if $(shell git -C $(T2_LIBTSM_DIR) status --porcelain --untracked-files=no 2>/dev/null),+dirty)

TIER2_ARGS := --render $(BUILD)/tests/test_pixels --fonts tests/fonts \
	--goldens tests/goldens/tier2 --out $(BUILD)/tier2 \
	--report $(BUILD)/tier2-report.json --root . \
	--libtsm-rev '$(T2_LIBTSM_REV)'

.PHONY: tier2
tier2: build ## Compare rendered screens with the tier 2 goldens
	$(RUN) python3 tests/tier2/run.py $(TIER2_ARGS)

.PHONY: tier2-golden
tier2-golden: build ## Regenerate the tier 2 goldens from the current renderer
	$(RUN) python3 tests/tier2/run.py $(TIER2_ARGS) --update
