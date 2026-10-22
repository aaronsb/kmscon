# Local VT demo (#36): the package built from this checkout, extracted into a
# staging root that fork/demo/run-vt.sh runs on one VT. See docs/fork/demo.md.
#
#   make demo           build the package as `make pkg` does, then stage it in
#                       fork/build/demo-root; nothing is installed system-wide
#
# Then, as root: sudo fork/demo/run-vt.sh [N]

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
	@echo "run: sudo fork/demo/run-vt.sh 3   (see docs/fork/demo.md)"
