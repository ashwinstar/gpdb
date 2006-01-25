#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_dtmdata
#
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_dtmdata - Reads the data from pg_dtm"

subdir = src/bin/pg_dtmdata
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -DFRONTEND $(CPPFLAGS)

OBJS=    pg_dtmdata.o $(WIN32RES)


all: submake-libpgport pg_dtmdata 

pg_dtmdata: $(OBJS) 
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LIBS) -o $@$(X)



install: all installdirs
	$(INSTALL_PROGRAM) pg_dtmdata $(X) $(DESTDIR)$(bindir)/pg_dtmdata$(X)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/pg_dtmdata$(X)

clean distclean maintainer-clean:
	rm -f pg_dtmdata$(X) $(OBJS)
