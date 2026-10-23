# ADR-103's synthetic scenarios S1 to S6, drawn by bbulk into memory
# (tests/bench_compose.c) at the sizes ADR-103 names, on one CPU.
#
#   make bench-compose                  1366x768, 1920x1080 and 7680x2160
#   make bench-compose BENCH_SIZES=1366x768 BENCH_FRAMES=120 BENCH_CPU=3
#
# One JSON line per scenario and size in fork/build/bench-compose.jsonl. The
# container is pinned to the host CPU BENCH_CPU, a core like the small tier 3
# machine's single vCPU; the figures move with the host's load and clock.

BENCH_SIZES  ?= 1366x768 1920x1080 7680x2160
BENCH_FRAMES ?= 60
BENCH_CPU    ?= 2
BENCH_RUN     = $(subst docker run ,docker run --cpuset-cpus=$(BENCH_CPU) ,$(RUN))

.PHONY: bench-compose
bench-compose: build ## Time ADR-103's S1 to S6 through bbulk in memory, on one CPU
	rm -f $(BUILD)/bench-compose.jsonl
	for s in $(BENCH_SIZES); do \
		$(BENCH_RUN) $(BUILD)/tests/bench_compose tests/fonts $${s%x*} $${s#*x} \
			$(BENCH_FRAMES) >> $(BUILD)/bench-compose.jsonl || exit 1; \
	done
	cat $(BUILD)/bench-compose.jsonl
