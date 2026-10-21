# Contact sheet: a drawn record of one conformance run (ADR-301).
#
#   make sheet-preview      run the tiers and draw fork/build/sheet.svg
#   make sheet NOTE="..."   draw docs/conformance/sheets/<date>-<kmscon>-<libtsm>.svg
#                           and add its row to docs/conformance/LEDGER.md
#
# `make sheet` refuses a working tree with uncommitted changes, so every ledger
# row names commits that produced it. Tier 2 and tier 3 reports are included
# when their runs have written them to fork/build.

CONF_DIR   := subprojects/libtsm/test/conformance
REPLAY     := $(BUILD)/subprojects/libtsm/test/tsm-replay
REPORTS    := $(BUILD)/tier1-report.json $(wildcard $(BUILD)/tier2-report.json $(BUILD)/tier3-report.json)
SHEET_DATE := $(shell date -u +%Y-%m-%d)
KREV        = $(shell git rev-parse --short HEAD)
LREV        = $(shell git -C subprojects/libtsm rev-parse --short HEAD 2>/dev/null || echo local)
SHEET_ARGS  = --kmscon $(KREV) --libtsm $(LREV) --date $(SHEET_DATE)

.PHONY: tier1-report
tier1-report: build
	$(RUN) sh -c 'python3 $(CONF_DIR)/check.py $(REPLAY) $(CONF_DIR) --report $(BUILD)/tier1-report.json || true'

.PHONY: sheet-preview
sheet-preview: tier1-report ## Draw a contact sheet of this run into fork/build/sheet.svg
	$(RUN) python3 fork/conformance/sheet.py --out $(BUILD)/sheet.svg $(SHEET_ARGS) $(REPORTS)

.PHONY: sheet
sheet: tier1-report ## Commit-ready contact sheet + ledger row (NOTE="what this run marks")
	@test -n "$(NOTE)" || { echo 'sheet: give NOTE="what this run marks"' >&2; exit 1; }
	@test -z "$$(git status --porcelain --untracked-files=no)" || { echo "sheet: kmscon tree has uncommitted changes" >&2; exit 1; }
	@test -z "$$(git -C subprojects/libtsm status --porcelain --untracked-files=no 2>/dev/null)" || { echo "sheet: libtsm tree has uncommitted changes" >&2; exit 1; }
	$(RUN) python3 fork/conformance/sheet.py \
		--out docs/conformance/sheets/$(SHEET_DATE)-$(KREV)-$(LREV).svg \
		--ledger docs/conformance/LEDGER.md --note "$(NOTE)" $(SHEET_ARGS) $(REPORTS)
