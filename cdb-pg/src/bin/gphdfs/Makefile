top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


all: gphdfs

gphdfs:
	ant clean
	ant dist -Dgphdconn.version=CE_1.0 -Dhadoop.core.version=0.20.2
	ant clean
	ant dist -Dgphdconn.version=EE_1.0 -Dhadoop.core.version=0.20.2

install: all installdirs
	$(INSTALL_PROGRAM) $(top_srcdir)/src/bin/gphdfs/dist/*.jar '$(DESTDIR)$(bindir)'
	$(INSTALL_PROGRAM) $(top_srcdir)/src/bin/gphdfs/hadoop_env.sh$(X) '$(DESTDIR)$(bindir)/hadoop_env.sh$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/*.jar'
	rm -f '$(DESTDIR)$(bindir)/hadoop_env.sh$(X)'

clean:
	ant clean
