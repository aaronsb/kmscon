# kmscon's tests under AddressSanitizer and UndefinedBehaviorSanitizer.
#
#   make asan                       every kmscon test but ASAN_SKIP
#   make asan ASAN_TESTS='test_x'   the meson tests named
#
# A build of its own in fork/build/asan, kmscon and every test, so the plain
# build stays as it is. A leak, a use after free or undefined behaviour fails
# the test; check's suites run unforked (CK_FORK=no), so a leak in one fails
# at the process's exit. fork/lsan.supp names the leaks inside libraries that
# are not kmscon's to fix. libtsm's own suites have `make asan` in the libtsm
# repository.
#
# ASAN_SKIP: the tests that start kmscon-image-helper, the real one or the
# fake. image_host.c caps the helper's address space (RLIMIT_AS, ADR-103)
# before it runs, and an ASan binary cannot reserve its shadow memory under
# that cap, so the helper exits at start and every request fails.

ASAN_BUILD := $(BUILD)/asan
ASAN_SKIP ?= test_image_host test_image_host_race test_terminal_image
ASAN_TESTS ?=

ASAN_ENV := CK_FORK=no ASAN_OPTIONS=detect_leaks=1 \
	LSAN_OPTIONS=suppressions=/src/fork/lsan.supp:print_suppressions=0 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

.PHONY: asan
asan: image ## Run kmscon's tests built with ASan and UBSan in fork/build/asan (ASAN_TESTS='...')
	$(RUN) sh -c '[ -f $(ASAN_BUILD)/build.ninja ] || meson setup $(ASAN_BUILD) $(SETUP_ARGS) -Db_sanitize=address,undefined -Db_lundef=false; \
		meson compile -C $(ASAN_BUILD) || exit 1; \
		tests="$(ASAN_TESTS)"; \
		if [ -z "$$tests" ]; then \
			tests=$$(meson test -C $(ASAN_BUILD) --no-rebuild --list --suite kmscon | sed s/^kmscon:// | grep -vxF -e "" $(addprefix -e ,$(ASAN_SKIP))); \
			[ -z "$(strip $(ASAN_SKIP))" ] || echo "asan: skipping $(strip $(ASAN_SKIP)) (#213)"; \
		fi; \
		[ -n "$$tests" ] || { echo "asan: no tests to run"; exit 1; }; \
		$(ASAN_ENV) meson test -C $(ASAN_BUILD) --no-rebuild --print-errorlogs $$tests'
