# The Bad Apple!! showcase (#75) on a free VT, run the way make vt-demo runs
# the demo: the same build, stage, VT choice and restore (fork/demo/vt-demo.sh
# and run-vt.sh), with fork/showcase/bad-apple.sh as the session. See
# fork/showcase/README.md.
#
#   make showcase BAD_APPLE=/path/to/your/copy [FROM=N] [AUDIO=1] [VT=N] [DEMO_ARGS='...']
#
# The video's frame caches are rendered first, as the invoking user, so the
# show starts at once on the VT and a missing video or tool fails before the
# build. Every variable reaches the scripts through the environment, never as
# recipe text.

.PHONY: showcase
showcase: export VT_DEMO_VT = $(value VT)
showcase: export VT_DEMO_ARGS = $(value DEMO_ARGS)
showcase: export VT_DEMO_SHOWCASE = $(value BAD_APPLE)
showcase: export VT_DEMO_SHOWCASE_FROM = $(value FROM)
showcase: export VT_DEMO_SHOWCASE_AUDIO = $(value AUDIO)
showcase: ## Build, stage and run the Bad Apple!! showcase on a free VT (BAD_APPLE=video, FROM=N, AUDIO=1, VT=N, DEMO_ARGS='...')
	BAD_APPLE="$$VT_DEMO_SHOWCASE" fork/showcase/bad-apple.sh --prepare
	$(MAKE) --no-print-directory demo
	fork/demo/vt-demo.sh
