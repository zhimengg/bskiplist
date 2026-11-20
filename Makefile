CC      = gcc
CFLAGS  = -Wall -Wextra -MMD -MP -I./include
LDFLAGS = -pthread -lm

SRC_DIR   = src
INC_DIR   = include
TEST_DIR  = test
BUILD_DIR = build$(if $(LAYOUT_TAG),/$(LAYOUT_TAG))

# ------------------------------------------------------------------ #
# Build options        												 #
# ------------------------------------------------------------------ #

DEBUG           ?= 0
MEASURE_LATENCY ?= 0

ASAN			?= 0
STATS			?= 1
CHECK_STRUCTURE ?= 0
OPT             ?= 3

# LAYOUT=path/to/layout.h (default include/layout/baseline.h); builds into build/<name>/.
LAYOUT          ?=

# CONFIG=small (tests only): defines BSL_LAYOUT_SMALL, which every layout
# honours with at most about eight entries per node, and sets the promotion
# denominator to 4, so that splits, promotions and multi-leaf scans are
# common. Builds into build/<name>.small/ (build/baseline.small/ for the
# reference layout). Performance runs use CONFIG=default.
CONFIG          ?= default

ifeq ($(DEBUG),1)
    CFLAGS += -O0 -g -DDEBUG
else
    CFLAGS += -O$(OPT) -DNDEBUG -march=native
endif

ifeq ($(MEASURE_LATENCY),1)
    CFLAGS += -DMEASURE_LATENCY=1
endif

ifeq ($(ASAN),1)
    CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
endif

ifeq ($(STATS),1)
    CFLAGS += -DSTATS=1
endif

ifeq ($(CHECK_STRUCTURE),1)
    CFLAGS += -DCHECK_STRUCTURE=1
endif

ifneq ($(LAYOUT),)
    ifeq ($(wildcard $(LAYOUT)),)
        $(error LAYOUT file '$(LAYOUT)' not found)
    endif
    ifneq ($(abspath $(LAYOUT)),$(abspath $(INC_DIR)/layout/baseline.h))
        LAYOUT_TAG = $(basename $(notdir $(LAYOUT)))
        CFLAGS += -DBSL_LAYOUT_HEADER=\"$(abspath $(LAYOUT))\"
    endif
endif

ifeq ($(CONFIG),small)
    CFLAGS += -DBSL_LAYOUT_SMALL -DBSL_PROMOTE_DENOM=4
    BUILD_DIR = build/$(or $(LAYOUT_TAG),baseline).small
else ifneq ($(CONFIG),default)
    $(error unknown CONFIG '$(CONFIG)'; use CONFIG=default or CONFIG=small)
endif

# ------------------------------------------------------------------ #
# Sources and targets                                                #
# ------------------------------------------------------------------ #

SRCS      = $(wildcard $(SRC_DIR)/*.c)
OBJS      = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
TEST_SRCS = $(wildcard $(TEST_DIR)/*.c)
TEST_BINS = $(TEST_SRCS:$(TEST_DIR)/%.c=$(BUILD_DIR)/%)
STATIC_LIB  = $(BUILD_DIR)/libbskiplist.a

# ------------------------------------------------------------------ #
# Rules                                                              #
# ------------------------------------------------------------------ #

.PHONY: all clean debug

all: $(BUILD_DIR) $(OBJS) $(TEST_BINS) lib

debug:
	$(MAKE) DEBUG=1

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%: $(TEST_DIR)/%.c $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)


$(STATIC_LIB): $(OBJS)
	ar rcs $@ $^

lib: $(BUILD_DIR) $(STATIC_LIB)

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJS:.o=.d)