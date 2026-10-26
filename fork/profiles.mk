# Build profiles (ADR-103, the minimal build): scripts/profiles/*.ini, meson
# machine files holding a reduced build's options. The Makefile reads PROFILE.
#
#   make test PROFILE=minimal            build fork/build-minimal and run its tests
#   make test PROFILE=zlib-free          the same, minimal.ini then no-zlib.ini
#   make profile-check PROFILE=...       then install it and check what it links
#                                        (tests/check_profile.py), the nm check's
#                                        control being kmscon from fork/build
#
# PROFILE moves BUILD and SETUP_ARGS for build, test and profile-check. asan
# and tier 2 run on the default build and refuse it: asan's fork/build/asan
# would be configured from the profile and reused later by a plain make asan,
# and tier 2's goldens are the default build's. Tier 3 builds its own package.

PROFILE_REFUSED := asan tier2 tier2-golden
ifneq ($(PROFILE),)
ifneq ($(filter $(PROFILE_REFUSED),$(MAKECMDGOALS)),)
$(error $(filter $(PROFILE_REFUSED),$(MAKECMDGOALS)) runs on the default build only; drop PROFILE=$(PROFILE))
endif
endif

.PHONY: profile-check
profile-check: test ## Build PROFILE=minimal or zlib-free, test it, install it and check what it links
	@[ -n "$(PROFILE)" ] || { echo "profile-check: set PROFILE=minimal or PROFILE=zlib-free"; exit 2; }
	$(RUN) sh -c 'rm -rf $(BUILD)/stage && meson install -C $(BUILD) --destdir stage --quiet && \
		{ [ -f fork/build/build.ninja ] || meson setup fork/build --force-fallback-for=libtsm \
			-Dlibtsm:default_library=static; } && meson compile -C fork/build kmscon && \
		python3 -I -B tests/check_profile.py $(PROFILE) $(BUILD) $(BUILD)/stage fork/build/src/kmscon'
