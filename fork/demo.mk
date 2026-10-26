# Local VT demo (#36): the package built from this checkout, extracted into a
# staging root that fork/demo/run-vt.sh runs on one VT. See docs/fork/demo.md.
#
#   make demo           build the package as `make pkg` does, then stage it in
#                       fork/build/demo-root; nothing is installed system-wide
#   make vt-demo        make demo, then run it on a free VT through sudo and
#                       print the run's answers when it ends
#                       (VT=N, DEMO_ARGS='kmscon options')
#   make vt-login       make demo, then run its kmscon with a login prompt on
#                       a free VT through sudo, for the demo (or the
#                       showcase, aaronsb/kmscon-showcase) run by hand in a
#                       logind session (VT=N, DEMO_ARGS=...)
#
# Or by hand, as root: sudo fork/demo/run-vt.sh [N]

DEMO_ROOT := fork/build/demo-root
# Recursive: fork/tier3.mk, which defines T3_*, is included after this file.
DEMO_PKGS  = $(T3_PKGOUT)/kmscon-aaronsb-$(T3_PKGVER)-1-x86_64.pkg.tar.zst \
	$(T3_PKGOUT)/kmscon-aaronsb-terminfo-$(T3_PKGVER)-1-x86_64.pkg.tar.zst

# The packages are extracted in the dev image, so the host needs no zstd. BUILD
# records what was staged; the demo's title page shows it.
.PHONY: demo
demo: pkg ## Build the package and stage it in fork/build/demo-root for fork/demo/run-vt.sh
	rm -rf $(DEMO_ROOT)
	mkdir -p $(DEMO_ROOT)
	$(RUN) sh -c 'for p in $(DEMO_PKGS); do tar -C $(DEMO_ROOT) --zstd -xf "$$p" || exit 1; done'
	printf '%s\n' "$$(git describe --tags --long --dirty) kmscon-aaronsb $(T3_PKGVER)" > $(DEMO_ROOT)/BUILD
	@echo "staged in $(DEMO_ROOT): $$(cat $(DEMO_ROOT)/BUILD)"
	@echo "run: make vt-demo, or sudo fork/demo/run-vt.sh 3   (see docs/fork/demo.md)"

# Blocks until the demo ends. VT and DEMO_ARGS reach the script through the
# environment, unexpanded, never as recipe text. DEMO_ARGS is split into words
# with quotes honoured, so a value with spaces is quoted inside it:
# DEMO_ARGS='--font-name="Monoid Nerd Font Mono"'.
.PHONY: vt-demo
vt-demo: export VT_DEMO_VT = $(value VT)
vt-demo: export VT_DEMO_ARGS = $(value DEMO_ARGS)
vt-demo: export VT_DEMO_KIT = $(value KIT)
vt-demo: demo ## Build, stage and run the demo on a free VT, then print its answers (VT=N, DEMO_ARGS='...')
	fork/demo/vt-demo.sh

# The same with kmscon's own login prompt (run-vt.sh --login) in place of the
# demo: a real login on the VT, a logind session (#207). Blocks until the
# first logout, or Ctrl-C.
.PHONY: vt-login
vt-login: export VT_DEMO_VT = $(value VT)
vt-login: export VT_DEMO_ARGS = $(value DEMO_ARGS)
# Passed on to be refused: the demo is run by hand once logged in.
vt-login: export VT_DEMO_KIT = $(value KIT)
vt-login: export VT_DEMO_LOGIN = 1
vt-login: demo ## Build, stage and run its kmscon with a login prompt on a free VT, until the logout (VT=N, DEMO_ARGS='...')
	fork/demo/vt-demo.sh
