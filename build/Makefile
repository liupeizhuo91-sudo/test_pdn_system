# Makefile for the distributed PDN SystemC-AMS test
SYSTEMC_HOME=/home/zzz/systemc
SYSTEMC_AMS_HOME=/home/zzz/systemc-ams
PYTHON ?= python3
RESULTS_DIR ?= results
RUN_COMMON_ARGS ?= --no-trace
RUN_ENV = LD_LIBRARY_PATH=${SYSTEMC_AMS_HOME}/lib-linux64:${SYSTEMC_HOME}/lib-linux64:$$$$LD_LIBRARY_PATH
PERF_VNOM_V ?= 0.8
PERF_VTH_V ?= 0.35
PERF_ALPHA ?= 1.3
PERF_DROOP_PERCENTILE ?= 95.0
PERF_OVERSHOOT_ALLOWANCE_MV ?= 10.0
PERF_OVERSHOOT_PENALTY ?= 1.0

# Compiler and flags
CXX = g++
CXXFLAGS = -I${SYSTEMC_HOME}/include -I${SYSTEMC_AMS_HOME}/include -I../source
LDFLAGS = -L${SYSTEMC_AMS_HOME}/lib-linux64 -L${SYSTEMC_HOME}/lib-linux64 -lsystemc-ams -lsystemc -lm -B/usr/lib/gold-ld/

# Target executable
TARGET = pdn_system_test_simu

# Source files	
MAIN_SOURCE = ../test/test_top.cpp
SOURCES = $(MAIN_SOURCE)
OBJECTS = $(SOURCES:.cpp=.o)

.NOTPARALLEL: sweep full-experiment run-full run-no_learning run-no_eec run-no_balance run-baseline_all_off

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) -o $@ $(OBJECTS) $(LDFLAGS)

%.o: %.cpp ../source/*.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	@echo "Running distributed PDN test simulation..."
	@$(RUN_ENV) ./$(TARGET)

run-fast: $(TARGET)
	@echo "Running distributed PDN test simulation without VCD tracing..."
	@$(RUN_ENV) ./$(TARGET) --no-trace

define RUN_CASE
run-$(1): $(TARGET)
	@echo "Running case $(1): $(2)"
	@mkdir -p $(RESULTS_DIR)/$(1)
	@rm -f pdn_learning_metrics.csv pdn_paper_comparison.csv waveform_pdn_system.vcd
	@$(RUN_ENV) ./$(TARGET) $(RUN_COMMON_ARGS) $(2)
	@cp pdn_learning_metrics.csv $(RESULTS_DIR)/$(1)/
	@cp pdn_paper_comparison.csv $(RESULTS_DIR)/$(1)/
	@if [ -f waveform_pdn_system.vcd ]; then cp waveform_pdn_system.vcd $(RESULTS_DIR)/$(1)/; fi
endef

$(eval $(call RUN_CASE,full,))
$(eval $(call RUN_CASE,no_learning,--disable-learning))
$(eval $(call RUN_CASE,no_eec,--disable-eec))
$(eval $(call RUN_CASE,no_balance,--disable-balance))
$(eval $(call RUN_CASE,baseline_all_off,--disable-learning --disable-eec --disable-balance))

sweep: run-full run-no_learning run-no_eec run-no_balance run-baseline_all_off

summarize:
	@$(PYTHON) ../scripts/summarize_pdn_results.py \
		--results-dir $(RESULTS_DIR) \
		--vnom-v $(PERF_VNOM_V) \
		--vth-v $(PERF_VTH_V) \
		--alpha $(PERF_ALPHA) \
		--droop-percentile $(PERF_DROOP_PERCENTILE) \
		--overshoot-allowance-mv $(PERF_OVERSHOOT_ALLOWANCE_MV) \
		--overshoot-penalty $(PERF_OVERSHOOT_PENALTY)

full-experiment:
	@$(MAKE) clean
	@$(MAKE) clean-results
	@$(MAKE) all
	@$(MAKE) sweep
	@$(MAKE) summarize
	@echo "Automated experiment complete. Results are under $(RESULTS_DIR)/"

clean:
	rm -f $(TARGET) *.vcd waveform_pdn_system.vcd
	rm -f ../test/*.o

clean-results:
	rm -rf $(RESULTS_DIR)

.PHONY: all run run-fast run-full run-no_learning run-no_eec run-no_balance run-baseline_all_off sweep summarize full-experiment clean clean-results
