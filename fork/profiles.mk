# Build profiles (ADR-103, the minimal build): scripts/profiles/*.ini, meson
# machine files holding a reduced build's options.
#
#   make test PROFILE=minimal            build fork/build-minimal and run its tests
#   make test PROFILE=zlib-free          the same, minimal.ini then no-zlib.ini
#   make profile-check PROFILE=...       then install it and check what it links
#                                        (tests/check_profile.py), the nm check's
#                                        control being kmscon from fork/build
#
# PROFILE moves BUILD and SETUP_ARGS, so every target that builds uses that
# profile's build directory; tier 2 and tier 3 run on the default build.

PROFILE ?=
PROFILE_INI_minimal := scripts/profiles/minimal.ini
PROFILE_INI_zlib-free := scripts/profiles/minimal.ini scripts/profiles/no-zlib.ini

ifneq ($(PROFILE),)
ifeq ($(PROFILE_INI_$(PROFILE)),)
$(error PROFILE=$(PROFILE): want minimal or zlib-free)
endif
BUILD := fork/build-$(PROFILE)
SETUP_ARGS := $(addprefix --native-file ,$(PROFILE_INI_$(PROFILE)))
endif

.PHONY: profile-check
profile-check: test ## Build PROFILE=minimal or zlib-free, test it, install it and check what it links
	@[ -n "$(PROFILE)" ] || { echo "profile-check: set PROFILE=minimal or PROFILE=zlib-free"; exit 2; }
	$(RUN) sh -c 'rm -rf $(BUILD)/stage && meson install -C $(BUILD) --destdir stage --quiet && \
		{ [ -f fork/build/build.ninja ] || meson setup fork/build --force-fallback-for=libtsm \
			-Dlibtsm:default_library=static; } && meson compile -C fork/build kmscon && \
		python3 -I -B tests/check_profile.py $(PROFILE) $(BUILD) $(BUILD)/stage fork/build/src/kmscon'
