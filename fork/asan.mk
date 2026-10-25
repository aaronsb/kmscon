# kmscon's tests under AddressSanitizer and UndefinedBehaviorSanitizer.
#
#   make asan                       test_shl and test_gltex
#   make asan ASAN_TESTS='test_x'   the meson tests named
#
# A build of its own in fork/build/asan, so the plain build stays as it is;
# only the tests named are compiled (kmscon itself does not build with
# -Werror under the sanitizers: src/config.c's strncpy). A leak, a use after
# free or undefined behaviour fails the test; check's suites run unforked
# (CK_FORK=no), so a leak in one fails at the process's exit. libtsm's own
# suites have `make asan` in the libtsm repository.

ASAN_BUILD := $(BUILD)/asan
ASAN_TESTS ?= test_shl test_gltex

.PHONY: asan
asan: image ## Run tests built with ASan and UBSan in fork/build/asan (ASAN_TESTS='...')
	$(RUN) sh -c '[ -f $(ASAN_BUILD)/build.ninja ] || meson setup $(ASAN_BUILD) $(SETUP_ARGS) -Db_sanitize=address,undefined -Db_lundef=false; meson compile -C $(ASAN_BUILD) $(addprefix tests/,$(ASAN_TESTS)) && CK_FORK=no ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 meson test -C $(ASAN_BUILD) --no-rebuild --print-errorlogs $(ASAN_TESTS)'
