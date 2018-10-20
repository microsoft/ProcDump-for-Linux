ROOT=.
CC=gcc
CFLAGS ?= -Wall
CCFLAGS=$(CFLAGS) -I ./include -pthread -std=gnu99
LIBDIR=lib
OBJDIR=obj
SRCDIR=src
INCDIR=include
BINDIR=bin
TESTDIR=tests/integration
DEPS=$(wildcard $(INCDIR)/*.h)
SRC=$(wildcard $(SRCDIR)/*.c)
TESTSRC=$(wildcard $(TESTDIR)/*.c)
OBJS=$(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(SRC))
TESTOBJS=$(patsubst $(TESTDIR)/%.c, $(OBJDIR)/%.o, $(TESTSRC))
OUT=$(BINDIR)/procdump
TESTOUT=$(BINDIR)/ProcDumpTestApplication


# installation directory
DESTDIR ?= /
INSTALLDIR=/usr/bin
MANDIR=/usr/share/man/man1

# package creation directories
BUILDDIR := $(CURDIR)/pkgbuild

# Flags to pass to debbuild/rpmbuild
PKGBUILDFLAGS := --define "_topdir $(BUILDDIR)" -bb

# Command to create the build directory structure
PKGBUILDROOT_CREATE_CMD = mkdir -p $(BUILDDIR)/DEBS $(BUILDDIR)/SDEBS $(BUILDDIR)/RPMS $(BUILDDIR)/SRPMS \
			$(BUILDDIR)/SOURCES $(BUILDDIR)/SPECS $(BUILDDIR)/BUILD $(BUILDDIR)/BUILDROOT

# package details
PKG_VERSION=1.0.1

all: clean build

build: $(OBJDIR) $(BINDIR) $(OUT) $(TESTOUT)

install:
	mkdir -p $(DESTDIR)$(INSTALLDIR)
	cp $(BINDIR)/procdump $(DESTDIR)$(INSTALLDIR)
	mkdir -p $(DESTDIR)$(MANDIR)
	cp procdump.1 $(DESTDIR)$(MANDIR)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	$(CC) -c -g -o $@ $< $(CCFLAGS)

$(OBJDIR)/%.o: $(TESTDIR)/%.c
	$(CC) -c -g -o $@ $< $(CCFLAGS)

$(OUT): $(OBJS)
	$(CC) -o $@ $^ $(CCFLAGS)

$(TESTOUT): $(TESTOBJS)
	$(CC) -o $@ $^ $(CCFLAGS)

$(OBJDIR):
	-@mkdir -p $(OBJDIR)

$(BINDIR):
	-@mkdir -p $(BINDIR)

.PHONY: clean
clean:
	-rm -rf $(OBJDIR)
	-rm -rf $(BINDIR)
	-rm -rf $(BUILDDIR)

test: build
	./tests/integration/run.sh

release: clean tarball

.PHONY: tarball
tarball:
	$(PKGBUILDROOT_CREATE_CMD)
	tar --exclude=./pkgbuild --exclude=./.git --transform 's,^\.,procdump-$(PKG_VERSION),' -czf $(BUILDDIR)/SOURCES/procdump-$(PKG_VERSION).tar.gz .
	sed -e "s/@PKG_VERSION@/$(PKG_VERSION)/g" dist/procdump.spec.in > $(BUILDDIR)/SPECS/procdump.spec

.PHONY: deb
deb: tarball
	debbuild $(PKGBUILDFLAGS) $(BUILDDIR)/SPECS/procdump.spec

.PHONY: rpm
rpm: tarball
	rpmbuild $(PKGBUILDFLAGS) $(BUILDDIR)/SPECS/procdump.spec
