# Offline equivalence tool for the harvest-based collision export (bridge #33). Host build (64-bit), no game needed.
#   make                       builds ./colharvest
#   make run MAP=pc_pit_flings diffs the harvest of maps/build/<MAP>.bsp against cache/bridge/colcache/<MAP>.bin
CXX ?= g++
ROOT := $(abspath ../../..)
MAP ?= pc_pit_flings
colharvest: main.cpp ../../src/bridge/colharvest.cpp ../../src/bridge/colharvest.hpp
	$(CXX) -std=c++17 -O2 -Wall -I../../protocol -I../../src/bridge main.cpp ../../src/bridge/colharvest.cpp -o $@
run: colharvest
	PORTAL2_DIR="$${PORTAL2_DIR:-$$(../portal2_dir.sh)}" ./colharvest --bsp $(ROOT)/maps/build/$(MAP).bsp --cache $(ROOT)/cache/bridge/colcache/$(MAP).bin $(ARGS)
clean:
	rm -f colharvest
.PHONY: run clean
