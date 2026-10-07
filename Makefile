# FULL STORE — PS5 homebrew store (etaHEN target)
# Toolchain: ps5-payload-sdk (https://github.com/ps5-payload-dev/sdk)
#
# Expects PS5_PAYLOAD_SDK env var pointing at installed SDK root.
# Produces: full-store.elf — load via payload loader (etaHEN / BinLoader / Itemzflow).

ifndef PS5_PAYLOAD_SDK
    $(error PS5_PAYLOAD_SDK not set — install ps5-payload-sdk and export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk)
endif

include $(PS5_PAYLOAD_SDK)/make/elf.mk

APP        := full-store
ELF        := $(APP).elf

SRCDIR     := src
INCDIR     := include
BUILDDIR   := build
ASSETS     := assets

# Source discovery
CPP_SRC    := $(shell find $(SRCDIR) -name '*.cpp')
C_SRC      := $(shell find $(SRCDIR) -name '*.c')
OBJS       := $(CPP_SRC:$(SRCDIR)/%.cpp=$(BUILDDIR)/%.o) \
              $(C_SRC:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)

# Dependencies pulled from SDK ports (install via sdk's port manager):
#   curl, mbedtls, zlib, libarchive, SDL2, SDL2_ttf, SDL2_image, freetype
PORT_LIBS  := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto \
              -larchive -lz -lbz2 -llzma -lzstd \
              -lSDL2 -lSDL2_ttf -lSDL2_image -lfreetype -lpng -ljpeg

CPPFLAGS   += -I$(INCDIR) -I$(PS5_PAYLOAD_SDK)/include \
              -DAPP_NAME='"FULL STORE"' -DAPP_VERSION='"1.0.0"'

CXXFLAGS   += -std=c++17 -Wall -Wextra -Wno-unused-parameter \
              -fno-rtti -fno-exceptions -O2 -g

CFLAGS     += -std=c11 -Wall -Wextra -O2 -g

LDFLAGS    += -lkernel -lc++ -lpthread $(PORT_LIBS)

# Build rules
all: $(ELF)

$(ELF): $(OBJS)
	@echo "  LD    $@"
	@$(LD) $(LDFLAGS) -o $@ $^

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILDDIR) $(ELF)

# Convenience: push ELF to running PS5 via ps5-payload loader
# usage: make send IP=192.168.1.x PORT=9020
send: $(ELF)
	@test -n "$(IP)"   || (echo "need IP=x.x.x.x"; exit 1)
	@test -n "$(PORT)" || (echo "need PORT=9020"; exit 1)
	nc -q0 $(IP) $(PORT) < $(ELF)

.PHONY: all clean send
