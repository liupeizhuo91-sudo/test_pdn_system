# Makefile for the distributed PDN SystemC-AMS test
SYSTEMC_HOME=/home/zzz/systemc
SYSTEMC_AMS_HOME=/home/zzz/systemc-ams
PYTHON ?= python3
RESULTS_DIR ?= results
RUN_COMMON_ARGS ?= --no-trace
RUN_ENV = LD_LIBRARY_PATH=${SYSTEMC_AMS_HOME}/lib-linux64:${SYSTEMC_HOME}/lib-linux64:$$$$LD_LIBRARY_PATH
AI_SCENARIOS ?= dense_10pct_75pct dense_mid_30pct_62p5pct medium_50pct_50pct sparse_mid_70pct_37p5pct sparse_90pct_25pct
STEP_SCENARIO ?= load_step_116_506_330ma
SCENARIOS ?= $(AI_SCENARIOS) $(STEP_SCENARIO)
PERF_VNOM_V ?= 0.8
PERF_VTH_V ?= 0.35
PERF_ALPHA ?= 1.3
PERF_DROOP_PERCENTILE ?= 95.0
PERF_OVERSHOOT_ALLOWANCE_MV ?= 10.0
PERF_OVERSHOOT_PENALTY ?= 1.0
PERF_NOMINAL_TOPS ?= 4.2
SUMMARY_WINDOW_POLICY ?= sim_best

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
	@rm -rf $(RESULTS_DIR)/$(1)
	@mkdir -p $(RESULTS_DIR)/$(1)
	@first=1; \
	for scenario in $(SCENARIOS); do \
		echo "  scenario $$$$scenario"; \
		mkdir -p $(RESULTS_DIR)/$(1)/$$$$scenario; \
		rm -f pdn_learning_metrics.csv pdn_paper_comparison.csv waveform_pdn_system.vcd; \
		$(RUN_ENV) ./$(TARGET) $(RUN_COMMON_ARGS) $(2) --scenario=$$$$scenario; \
		cp pdn_learning_metrics.csv $(RESULTS_DIR)/$(1)/$$$$scenario/; \
		cp pdn_paper_comparison.csv $(RESULTS_DIR)/$(1)/$$$$scenario/; \
		cp pdn_paper_comparison.csv $(RESULTS_DIR)/$(1)/pdn_paper_comparison.csv; \
		if [ $$$$first -eq 1 ]; then \
			cp pdn_learning_metrics.csv $(RESULTS_DIR)/$(1)/pdn_learning_metrics.csv; \
			first=0; \
		else \
			tail -n +2 pdn_learning_metrics.csv >> $(RESULTS_DIR)/$(1)/pdn_learning_metrics.csv; \
		fi; \
		if [ -f waveform_pdn_system.vcd ]; then cp waveform_pdn_system.vcd $(RESULTS_DIR)/$(1)/$$$$scenario/; fi; \
	done
endef

$(eval $(call RUN_CASE,full,))
$(eval $(call RUN_CASE,no_learning,--disable-learning --paper-no-learning-profile))
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
		--nominal-tops $(PERF_NOMINAL_TOPS) \
		--window-policy $(SUMMARY_WINDOW_POLICY) \
		--droop-percentile $(PERF_DROOP_PERCENTILE) \
		--overshoot-allowance-mv $(PERF_OVERSHOOT_ALLOWANCE_MV) \
		--overshoot-penalty $(PERF_OVERSHOOT_PENALTY)

visualize:
	@$(PYTHON) ../scripts/visualize_pdn_results.py \
		--results-dir $(RESULTS_DIR)

full-experiment:
	@$(MAKE) clean
	@$(MAKE) clean-results
	@$(MAKE) all
	@$(MAKE) sweep
	@$(MAKE) summarize
	@$(MAKE) visualize
	@echo "Automated experiment complete. Results are under $(RESULTS_DIR)/"

clean:
	rm -f $(TARGET) *.vcd waveform_pdn_system.vcd
	rm -f ../test/*.o

clean-results:
	rm -rf $(RESULTS_DIR)

.PHONY: all run run-fast run-full run-no_learning run-no_eec run-no_balance run-baseline_all_off sweep summarize visualize full-experiment clean clean-results
