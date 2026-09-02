# Build and run random pick strategy benchmarks.
#
#   make          build the benchmark binaries (default)
#   make setup    clone SHISHUA if it is not already here
#   make run      measure, write CSVs
#   make plot     measure, write CSVs, draw the plots
#   make clean    remove binaries, CSVs and plots (keeps the shishua clone)

CXX     := clang++
# -DNDEBUG strips the assert inside prng_gen's hot loop.
CXXFLAGS := -O3 -DNDEBUG -std=c++11 -Wall -Wextra
# Pick by architecture, not by probing which flag compiles: on aarch64 both are
# accepted but -march=native resolves to a baseline (apple-m1) instead of the
# real CPU, and on x86_64 -mcpu only sets tuning, leaving the ISA at baseline -
# which would cost SHISHUA its AVX2 path. Both clang and gcc agree on these two.
CXXFLAGS += $(if $(filter x86_64,$(shell uname -m)),-march=native,-mcpu=native)

BINS    := speed select
HEADERS := os.h rng.h
REPO    := https://github.com/espadrine/shishua.git

.PHONY: all setup build run clean
all: build

setup: shishua
build: $(BINS)

shishua:
	git clone --depth 1 --revision 28ab108f9532bf35b2f988357c9a06434fd36291 $(REPO) $@

# Every binary is one translation unit, so the headers are the whole dependency
# set. shishua is order-only: the clone has to exist, but touching it should not
# force a rebuild.
$(BINS): %: %.cpp $(HEADERS) shishua
	mkdir -p bin
	$(CXX) $(CXXFLAGS) $< -o ./$@

# Data goes to the CSV, progress and verdicts to the terminal.
run: build
	@for b in $(BINS); do echo; ./$$b | tee $$b.csv; done

plot: run
	./plot.py

clean:
	rm -rf $(BINS) *.csv plots
