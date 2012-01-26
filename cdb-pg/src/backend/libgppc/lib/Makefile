#-------------------------------------------------------------------------
#
# Makefile for src/backend/libgppc library
#
#
#-------------------------------------------------------------------------

# GPPC is ready to build against installed backend, so that we can try
# against different backend release without switching source tree.
# This means GPPC should build and work with older releases without
# source modified and we really need to keep it identical between
# different backend release branches. This makes our life easier.
# We achieve to build with another backend release by PGXS. Everything
# GPPC needs (i.e. headers and makefiles) are installed along with pg_config.
ifdef USE_PGXS
ifndef PG_CONFIG
PG_CONFIG = pg_config
endif
PGXS := $(shell $(PG_CONFIG) --pgxs)
top_builddir := $(dir $(PGXS))../..
else
subdir = src/backend/libgppc/lib
top_builddir = ../../../..
endif

include $(top_builddir)/src/Makefile.global


# shared library parameters
NAME = gppc
SO_MAJOR_VERSION = 1
SO_MINOR_VERSION = 0

override CPPFLAGS := -I$(includedir_server) -I$(includedir_internal) $(CPPFLAGS)

OBJS = gppc.o


all: gppc_gp_version_num_check gppc_config.h all-lib

doc: gppc_config.h
	doxygen

clean-doc:
	rm -rf html latex

# Fetch version number from pg_config.h, because
# GPPC should have its own config header so that
# user library can avoid to include any backend headers.
ifdef PGXS
pg_config_path := $(includedir_server)/pg_config.h
else
pg_config_path := $(top_srcdir)/src/include/pg_config.h
endif

GP_VERSION_NUM := $(shell grep 'define  *GP_VERSION_NUM' $(pg_config_path) \
		| sed 's/^[^0-9]*\([0-9]*\)/\1/')
GPPC_GP_VERSION_NUM := $(shell test -f gppc_config.h \
		&& (grep 'define  *GP_VERSION_NUM' gppc_config.h | sed 's/^[^0-9]*\([0-9]*\)/\1/'))

# Reset if we attempt to build against different version. This may happen
# when we build without PGXS after building with PGXS, vice versa.
gppc_gp_version_num_check:
ifneq ($(GP_VERSION_NUM),$(GPPC_GP_VERSION_NUM))
	rm -f gppc_config.h
endif
gppc_config.h: gppc_config.h.in
	sed 's/$$GP_VERSION_NUM_IN/$(GP_VERSION_NUM)/' $< > $@

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

# Mac OS needs dynamic_lookup for undefined symbols when using -dynamiclib
ifeq ($(PORTNAME), darwin)
  override LINK.shared += -undefined dynamic_lookup
endif


# Depend on Makefile.global to force rebuild on re-run of configure.
gppc_config.h: $(top_builddir)/src/Makefile.global


install: all installdirs install-lib
	$(INSTALL_DATA) $(srcdir)/gppc.h '$(DESTDIR)$(includedir)'
	$(INSTALL_DATA) $(srcdir)/gppc_config.h '$(DESTDIR)$(includedir)'

installdirs: installdirs-lib
	$(MKDIR_P) '$(DESTDIR)$(includedir)' '$(DESTDIR)$(includedir_internal)'

uninstall: uninstall-lib
	rm -f '$(DESTDIR)$(includedir)/gppc.h'
	rm -f '$(DESTDIR)$(includedir)/gppc_config.h'

clean distclean: clean-lib clean-doc
	rm -f $(OBJS)
	rm -f gppc_config.h

maintainer-clean: distclean
