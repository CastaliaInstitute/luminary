CC ?= cc
CFLAGS ?= -O2 -std=gnu99 -Wall -Wextra -Wno-unused-parameter
LDLIBS = -lm

ocean-sim: host_main.c ocean_sim.c ocean_sim.h
	$(CC) $(CFLAGS) -o $@ host_main.c ocean_sim.c $(LDLIBS)

.PHONY: run clean
run: ocean-sim
	mkdir -p ../../output/ocean-sim
	./ocean-sim --steps 900 --frames ../../output/ocean-sim

clean:
	rm -f ocean-sim
