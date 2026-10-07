# FULL STORE — PS5 homebrew store (etaHEN target)
# Toolchain: ps5-payload-sdk (https://github.com/ps5-payload-dev/sdk)

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK not set)
endif

APP        := full-store
ELF        := $(APP).elf

SRCDIR     := src
INCDIR     := include
BUILDDIR   := build

# Source discovery
CPP_SRC    := $(shell find $(SRCDIR) -name '*.cpp')
C_SRC      := $(shell find $(SRCDIR) -name '*.c')
OBJS       := $(CPP_SRC:$(SRCDIR)/%.cpp=$(BUILDDIR)/%.o) \
              $(C_SRC:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)

PORT_LIBS  := -L$(PS5_SYSROOT)/user/homebrew/lib \
              -lcurl \
              -larchive -lz -lbz2 -llzma -lzstd \
              -lSDL2 -lSDL2_ttf -lSDL2_image -lfreetype -lpng -ljpeg

CPPFLAGS   += -I$(INCDIR) -I$(SRCDIR) \
              -I$(PS5_SYSROOT)/user/homebrew/include \
              -I$(PS5_SYSROOT)/user/homebrew/include/SDL2 \
              -DAPP_NAME='"FULL STORE"' -DAPP_VERSION='"1.0.0"'

CXXFLAGS   += -std=c++17 -Wall -Wextra -Wno-unused-parameter \
              -O2 -g

CFLAGS     += -std=c11 -Wall -Wextra -O2 -g

LDFLAGS    += $(PORT_LIBS) -Wl,--allow-undefined

all: $(ELF)

$(ELF): $(OBJS)
	@echo "  LD    $@"
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILDDIR) $(ELF)

.PHONY: all clean
