# Run page: a readable record of one conformance run (ADR-301).
#
#   make sheet-preview      run the tiers and write the page to fork/build/run/
#   make sheet NOTE="..."   write docs/conformance/runs/<date>-<kmscon>-<libtsm>/
#                           and add its row to docs/conformance/LEDGER.md
#   make sheet-selftest     unit tests of the page generator (part of make test)
#
# `make sheet` refuses a working tree with uncommitted changes, so every ledger
# row names commits that produced it. Tiers 1 and 2 run as part of the page;
# a tier 3 report is included when a tier 3 run has written one to fork/build.
# The page compares the run with the ledger's last row; golden-history.sh
# reads the goldens changed since on the host, where git works.

CONF_DIR   := subprojects/libtsm/test/conformance
REPLAY     := $(BUILD)/subprojects/libtsm/test/tsm-replay
LEDGER     := docs/conformance/LEDGER.md
HISTORY    := $(BUILD)/golden-history
REPORTS     = $(BUILD)/tier1-report.json $(BUILD)/tier2-report.json $(wildcard $(BUILD)/tier3-report.json)
SHEET_DATE := $(shell date -u +%Y-%m-%d)
KREV        = $(shell git rev-parse --short HEAD)
LREV        = $(shell git -C subprojects/libtsm rev-parse --short HEAD 2>/dev/null || echo local)
PAGE        = $(RUN) python3 -B fork/conformance/runpage.py --kmscon $(KREV) --libtsm $(LREV) \
	--date $(SHEET_DATE) --ledger $(LEDGER) --history $(HISTORY) \
	--fork-goldens $(CONF_DIR)/fork-goldens.list

.PHONY: tier1-report
tier1-report: build
	$(RUN) sh -c 'python3 $(CONF_DIR)/check.py $(REPLAY) $(CONF_DIR) --report $(BUILD)/tier1-report.json || true'

.PHONY: tier2-report
tier2-report: build
	-$(MAKE) --no-print-directory tier2

.PHONY: sheet-preview
sheet-preview: tier1-report tier2-report ## Write this run's page into fork/build/run/
	fork/conformance/golden-history.sh $(LEDGER) $(HISTORY)
	$(PAGE) --out $(BUILD)/run $(REPORTS)

.PHONY: sheet
sheet: tier1-report tier2-report ## Commit-ready run page + ledger row (NOTE="what this run marks")
	@test -n "$(NOTE)" || { echo 'sheet: give NOTE="what this run marks"' >&2; exit 1; }
	@test -z "$$(git status --porcelain --untracked-files=no)" || { echo "sheet: kmscon tree has uncommitted changes" >&2; exit 1; }
	@test -z "$$(git -C subprojects/libtsm status --porcelain --untracked-files=no 2>/dev/null)" || { echo "sheet: libtsm tree has uncommitted changes" >&2; exit 1; }
	fork/conformance/golden-history.sh $(LEDGER) $(HISTORY)
	$(PAGE) --out docs/conformance/runs/$(SHEET_DATE)-$(KREV)-$(LREV) --append --note "$(NOTE)" $(REPORTS)

.PHONY: sheet-selftest
sheet-selftest: image ## Unit tests of the run page generator
	$(RUN) python3 -B fork/conformance/test_runpage.py

# make test runs the generator's tests beside kmscon's and libtsm's suites.
test: sheet-selftest
