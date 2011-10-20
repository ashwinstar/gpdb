top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ANT=/opt/releng/apache-ant/bin/ant

all: gphdfs protocol_formatter

protocol_formatter:
	$(MAKE) -C src/protocol_formatter

gphdfs:
	$(ANT) clean
	$(ANT) dist -Dgphdconn.version=CE_1.0.0.0 -Dhadoop.core.version=0.20.2
	$(ANT) clean
	$(ANT) dist -Dgphdconn.version=EE_1.0.0.0 -Dhadoop.core.version=0.20.2
	
gphdfsdoc:
	$(ANT) javadoc -Dgphdconn.version=CE_1.0.0.0 -Dhadoop.core.version=0.20.2

install: all installdirs
	$(MAKE) -C src/protocol_formatter install
	$(INSTALL_PROGRAM) $(top_srcdir)/src/bin/gphdfs/dist/*.jar '$(DESTDIR)$(libdir)/hadoop/'
	$(INSTALL_PROGRAM) $(top_srcdir)/src/bin/gphdfs/hadoop_env.sh$(X) '$(DESTDIR)$(libdir)/hadoop/hadoop_env.sh$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(libdir)'
	$(MKDIR_P) '$(DESTDIR)$(libdir)/hadoop'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/*.jar'
	rm -f '$(DESTDIR)$(bindir)/hadoop_env.sh$(X)'

clean:
	$(MAKE) -C src/protocol_formatter clean
	$(ANT) clean
