ROOT        = ..
INC_DIR     = $(ROOT)/include

# LAYOUT=path/to/layout.h tests that layout's library, as ../Makefile builds it.
LAYOUT      ?=
ifneq ($(LAYOUT),)
    LAYOUT_ABS := $(abspath $(LAYOUT))
    ifneq ($(LAYOUT_ABS),$(abspath $(INC_DIR)/layout/baseline.h))
        LAYOUT_TAG = $(basename $(notdir $(LAYOUT)))
    endif
endif
BUILD_DIR   = $(ROOT)/build$(if $(LAYOUT_TAG),/$(LAYOUT_TAG))

CGO_CFLAGS_VAL  = -I$(abspath $(INC_DIR))
CGO_LDFLAGS_VAL = -L$(abspath $(BUILD_DIR)) -lbskiplist -lpthread -lm

GO := $(shell command -v go 2>/dev/null)

.PHONY: all check clean tidy lib

all: check

lib:
	$(MAKE) -C $(ROOT) $(if $(LAYOUT),LAYOUT=$(LAYOUT_ABS))

tidy:
ifndef GO
	$(error "go not found, please install Go first")
endif
	@echo "--- Running go mod tidy ---"
	go mod tidy


check: tidy lib
	@echo "--- Running Linearizability Tests ---"
	CGO_CFLAGS="$(CGO_CFLAGS_VAL)" \
	CGO_LDFLAGS="$(CGO_LDFLAGS_VAL)" \
	$(GO) test -v -count=1 -timeout 120s ./...


clean:
ifndef GO
	@echo "go not found, skipping clean-testcache"
else
	$(GO) clean -testcache
endif