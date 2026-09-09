# lucid-wayland: the Wayland client code shared by LucidOS shell components.
#
# Built as a static library on purpose, and it stays one now that it is
# packaged. The original reason was that it shipped as a submodule versioned
# with its consumers; the reason now is that a shared object is a promise about
# an ABI, and this interface is still moving -- window_count() and activate()
# were both added on 7 September. It becomes a .so when that stops.
CXX      ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra
CXXSTD   := -std=c++20
CPPFLAGS += -Iinclude -Igenerated
PREFIX   ?= $(HOME)/.local

WAYLAND_SCANNER ?= wayland-scanner
GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0)
WL_CFLAGS   := $(shell pkg-config --cflags wayland-client)

# Vendored rather than build-depended on: wlr-protocols is not packaged on
# Debian or Ubuntu at all, and pinning the XML means the generated marshalling
# cannot change underneath a build.
PROTOCOLS := ext-foreign-toplevel-list-v1 wlr-foreign-toplevel-management-unstable-v1 \
             ext-image-capture-source-v1 ext-image-copy-capture-v1
GEN_H := $(addprefix generated/,$(addsuffix -client-protocol.h,$(PROTOCOLS)))
GEN_C := $(addprefix generated/,$(addsuffix -protocol.c,$(PROTOCOLS)))

all: liblucidwayland.a

generated/%-client-protocol.h: protocols/%.xml
	@mkdir -p generated
	$(WAYLAND_SCANNER) client-header $< $@

generated/%-protocol.c: protocols/%.xml
	@mkdir -p generated
	$(WAYLAND_SCANNER) private-code $< $@

generated/%-protocol.o: generated/%-protocol.c
	$(CC) $(CFLAGS) $(WL_CFLAGS) -c $< -o $@

toplevel_source.o: src/toplevel_source.cpp $(GEN_H)
	$(CXX) $(CXXSTD) $(CPPFLAGS) $(CXXFLAGS) $(GLIB_CFLAGS) $(WL_CFLAGS) -c $< -o $@

screen_capture.o: src/screen_capture.cpp $(GEN_H)
	$(CXX) $(CXXSTD) $(CPPFLAGS) $(CXXFLAGS) $(GLIB_CFLAGS) $(WL_CFLAGS) -c $< -o $@

liblucidwayland.a: toplevel_source.o screen_capture.o $(GEN_C:.c=.o)
	$(AR) rcs $@ $^

install: liblucidwayland.a
	install -Dm644 liblucidwayland.a $(DESTDIR)$(PREFIX)/lib/liblucidwayland.a
	install -Dm644 include/lucid/toplevel_source.h \
		$(DESTDIR)$(PREFIX)/include/lucid/toplevel_source.h

clean:
	rm -rf generated *.o liblucidwayland.a

.PHONY: all install clean
