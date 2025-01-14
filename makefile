# Flexible Makefile for fillfs with temp binary in bin/
CC       = gcc
CFLAGS   = -Wall -Wextra -O2
TARGET   = fillfs
MANPAGE  = fillfs.1

# Where to install the binary
PREFIX   ?= /usr
# Where to install man pages. Typically $(PREFIX)/share/man
MANPREFIX ?= $(PREFIX)/share/man

# Temporary output directory for the binary
BUILDDIR = bin

all: $(BUILDDIR)/$(TARGET)

# Ensure the build directory exists
$(BUILDDIR)/$(TARGET): fillfs.c
	mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) $< -o $@

install: $(BUILDDIR)/$(TARGET)
	install -d $(PREFIX)/bin
	install -m 755 $(BUILDDIR)/$(TARGET) $(PREFIX)/bin/$(TARGET)

	install -d $(MANPREFIX)/man1
	install -m 644 $(MANPAGE) $(MANPREFIX)/man1/$(MANPAGE)

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)
	rm -f $(MANPREFIX)/man1/$(MANPAGE)

clean:
	rm -rf $(BUILDDIR)

.PHONY: all install uninstall clean
