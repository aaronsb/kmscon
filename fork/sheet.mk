# Run pages and comparison pages: readable records of conformance runs (ADR-301).
#
#   make sheet-preview      run the tiers and write the page to fork/build/run/
#   make sheet NOTE="..."   write docs/conformance/runs/<date>-<kmscon>-<libtsm>/
#                           and add its row to docs/conformance/LEDGER.md
#   make sheet-save SAVE=DIR [TIERS="2 3"]
#                           copy the last run's reports and screens to DIR;
#                           TIERS=3 keeps tier 3 only, as for upstream's run
#   make sheet-compare A=DIR B=DIR [A2=DIR] [B2=DIR] [LABEL_A=..] [LABEL_B=..]
#                       [VERDICTS=FILE] [OUT=DIR]
#                           pair the screens of two saved runs in OUT
#                           (default fork/build/compare/)
#   make sheet-selftest     unit tests of the page generators (also run by make test, through meson)
#
# A committed run (`make sheet`) is a milestone record: it refuses a working
# tree with uncommitted changes, and a HEAD that is not origin/main after a
# fetch (SHEET_ANY_COMMIT=1 overrides), so every ledger row names commits on
# main. Tiers 1 and 2 run as part of the page; a tier 3 report is included
# when a tier 3 run has written one to fork/build. The page compares the run
# with the ledger's last row; golden-history.sh reads the goldens changed
# since on the host, where git works. Saved runs live inside the checkout,
# since the generators run in the dev image with only the checkout mounted.

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

# The checks a committed run must pass, made before any tier runs, so a
# refused run costs nothing.
.PHONY: sheet-check
sheet-check:
	@test -n "$(NOTE)" || { echo 'sheet: give NOTE="what this run marks"' >&2; exit 1; }
	@test -z "$$(git status --porcelain --untracked-files=no)" || { echo "sheet: kmscon tree has uncommitted changes" >&2; exit 1; }
	@test -z "$$(git -C subprojects/libtsm status --porcelain --untracked-files=no 2>/dev/null)" || { echo "sheet: libtsm tree has uncommitted changes" >&2; exit 1; }
	@test "$(SHEET_ANY_COMMIT)" = 1 || { \
		git fetch -q origin main || { echo "sheet: git fetch origin main failed, so HEAD cannot be checked against origin/main" >&2; exit 1; }; \
		test "$$(git rev-parse HEAD)" = "$$(git rev-parse origin/main)" || { \
		echo "sheet: HEAD is not origin/main; a ledger run is made on main once the stage's branches have merged" >&2; \
		echo "sheet: SHEET_ANY_COMMIT=1 overrides" >&2; exit 1; }; }

.PHONY: sheet
sheet: sheet-check ## Commit-ready run page + ledger row (NOTE="what this run marks")
	$(MAKE) --no-print-directory tier1-report tier2-report
	fork/conformance/golden-history.sh $(LEDGER) $(HISTORY)
	$(PAGE) --out docs/conformance/runs/$(SHEET_DATE)-$(KREV)-$(LREV) --append --note "$(NOTE)" $(REPORTS)

.PHONY: sheet-save
sheet-save: ## Save the last run's reports and screens to SAVE=<dir in the checkout>
	@test -n "$(SAVE)" || { echo 'sheet-save: give SAVE=<dir>, e.g. fork/build/saved/fork-1' >&2; exit 1; }
	$(RUN) python3 -B fork/conformance/compare.py save --out $(SAVE) \
		$(wildcard $(foreach t,$(TIERS),$(BUILD)/tier$(t)-report.json))

COMPARE_OUT ?= $(or $(OUT),$(BUILD)/compare)
COMPARE_CMD  = make sheet-compare A=$(A) B=$(B)$(if $(A2), A2=$(A2))$(if $(B2), B2=$(B2)) \
	LABEL_A='$(LABEL_A)' LABEL_B='$(LABEL_B)'$(if $(VERDICTS), VERDICTS=$(VERDICTS))
LABEL_A     ?= A
TIERS       ?= 2 3
LABEL_B     ?= B

.PHONY: sheet-compare
sheet-compare: ## Page pairing the screens of saved runs A= and B= (A2=, B2= second runs; LABEL_A=, LABEL_B=)
	@test -n "$(A)" -a -n "$(B)" || { echo 'sheet-compare: give A=<saved run> B=<saved run>' >&2; exit 1; }
	$(RUN) python3 -B fork/conformance/compare.py page --out $(COMPARE_OUT) --a $(A) --b $(B) \
		$(if $(A2),--a2 $(A2)) $(if $(B2),--b2 $(B2)) $(if $(VERDICTS),--verdicts $(VERDICTS)) \
		--label-a "$(LABEL_A)" --label-b "$(LABEL_B)" --harness $(KREV) --command "$(COMPARE_CMD)"

.PHONY: sheet-selftest
sheet-selftest: image ## Unit tests of the run and comparison page generators
	$(RUN) sh -c 'python3 -B fork/conformance/test_runpage.py && python3 -B fork/conformance/test_compare.py'

