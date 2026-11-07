PLUGIN_NAME = retuner
SOURCES = src/retuner.cpp
TEST_SOURCES = tests/retuner_core_test.cpp tests/retuner_adapter_test.cpp

UNAME_S := $(shell uname -s)
TARGET ?= hardware
ARM_CXX ?= arm-none-eabi-g++
ARM_NM ?= arm-none-eabi-nm
ARM_READELF ?= arm-none-eabi-readelf
ARM_SIZE ?= arm-none-eabi-size

ifeq ($(TARGET),hardware)
    CXX = $(ARM_CXX)
    CFLAGS = -std=c++11 \
             -mcpu=cortex-m7 \
             -mfpu=fpv5-d16 \
             -mfloat-abi=hard \
             -mthumb \
             -Os \
             -fPIC \
             -ffunction-sections \
             -fdata-sections \
             -fno-rtti \
             -fno-exceptions \
             -fno-unwind-tables \
             -fno-asynchronous-unwind-tables \
             -Wall \
             -Wextra \
             -Werror \
             -Wno-missing-field-initializers
    INCLUDES = -I. -I./src -I./distingNT_API/include
    LDFLAGS = -Wl,--relocatable -nostdlib
    OUTPUT_DIR = plugins
    BUILD_DIR = build
    OUTPUT = $(OUTPUT_DIR)/$(PLUGIN_NAME).o
    OBJECTS = $(patsubst %.cpp, $(BUILD_DIR)/%.o, $(SOURCES))
    CHECK_CMD = $(ARM_NM) $(OUTPUT) | grep ' U ' || true
    SIZE_CMD = $(ARM_SIZE) -A $(OUTPUT)
else ifeq ($(TARGET),test)
    ifeq ($(UNAME_S),Darwin)
        CXX = clang++
    else
        CXX = g++
    endif
    CFLAGS = -std=c++11 -O2 -g -Wall -Wextra -Werror -fno-rtti -fno-exceptions
    INCLUDES = -I. -I./src -I./distingNT_API/include
    BUILD_DIR = build
    TEST_CORE = $(BUILD_DIR)/retuner_core_test
    TEST_ADAPTER = $(BUILD_DIR)/retuner_adapter_test
endif

all: $(OUTPUT)

ifeq ($(TARGET),hardware)
$(OUTPUT): $(OBJECTS)
	@mkdir -p $(OUTPUT_DIR)
	$(CXX) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@echo "Built: $@"
	@$(SIZE_CMD)

$(BUILD_DIR)/%.o: %.cpp src/retuner_core.h | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)
else ifeq ($(TARGET),test)
$(TEST_CORE): tests/retuner_core_test.cpp src/retuner_core.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CFLAGS) $(INCLUDES) -o $@ tests/retuner_core_test.cpp -lm
	@echo "Built: $@"

$(TEST_ADAPTER): tests/retuner_adapter_test.cpp src/retuner.cpp src/retuner_core.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CFLAGS) $(INCLUDES) -o $@ tests/retuner_adapter_test.cpp src/retuner.cpp -lm
	@echo "Built: $@"
endif

hardware:
	@$(MAKE) TARGET=hardware

test:
	@$(MAKE) TARGET=test build/retuner_core_test build/retuner_adapter_test
	@./build/retuner_core_test
	@./build/retuner_adapter_test

verify: test
	@$(MAKE) TARGET=hardware
	@$(MAKE) check TARGET=hardware
	@$(MAKE) inspect TARGET=hardware

check: $(OUTPUT)
	@$(CHECK_CMD)

size: $(OUTPUT)
	@$(SIZE_CMD)

ifeq ($(TARGET),hardware)
inspect: $(OUTPUT)
	@$(ARM_READELF) -h $(OUTPUT) | grep -Eq 'Class:[[:space:]]+ELF32' \
		|| (echo "Plugin is not ELF32" >&2; exit 1)
	@$(ARM_READELF) -h $(OUTPUT) | grep -Eq 'Data:[[:space:]]+2.s complement, little endian' \
		|| (echo "Plugin is not little-endian" >&2; exit 1)
	@$(ARM_READELF) -h $(OUTPUT) | grep -Eq 'Type:[[:space:]]+REL' \
		|| (echo "Plugin is not a relocatable object" >&2; exit 1)
	@$(ARM_READELF) -h $(OUTPUT) | grep -Eq 'Machine:[[:space:]]+ARM' \
		|| (echo "Plugin is not built for ARM" >&2; exit 1)
	@$(ARM_NM) --defined-only $(OUTPUT) | grep -Eq '[[:space:]]pluginEntry$$' \
		|| (echo "pluginEntry export is missing" >&2; exit 1)
	@echo "PASS: $(OUTPUT) is a little-endian relocatable ARM plugin with pluginEntry"
	@$(ARM_NM) -u $(OUTPUT) | awk '{print $$2}' | sort -u | grep -Ev '^(NT_algorithmIndex|NT_drawText|NT_getNumScl|NT_getSclInfo|NT_intToString|NT_isSdCardMounted|NT_parameterOffset|NT_readScl|NT_sendMidi3ByteMessage|NT_setParameterFromUi|NT_updateParameterDefinition|memcpy|memset)$$' \
		&& { echo "Unexpected undefined symbol(s) in $(OUTPUT)" >&2; exit 1; } || true
	@echo "Review undefined symbols against the pinned API/runtime:"
	@$(ARM_NM) -u $(OUTPUT) || true
else
inspect:
	@echo "inspect is available only for TARGET=hardware" >&2
	@exit 1
endif

clean:
	rm -rf $(BUILD_DIR) $(OUTPUT_DIR)

.PHONY: all hardware test verify check size inspect clean
