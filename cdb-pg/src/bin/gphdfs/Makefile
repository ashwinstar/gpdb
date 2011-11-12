top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ANT=/opt/releng/apache-ant/bin/ant
gphdver = gphd-xnet-1.0.0.0
gpmrver = gpmr-xnet-1.0.0.0

all: gphdfs protocol_formatter

protocol_formatter:
	$(MAKE) -C src/protocol_formatter

gphdfs:
	$(ANT) clean
	$(ANT) dist -Dgphdconn.version=$(gphdver) -Dhadoop.core.version=0.20.2
	$(ANT) clean
	$(ANT) dist -Dgphdconn.version=$(gpmrver) -Dhadoop.core.version=0.20.2
	
gphdfsdoc:
	$(ANT) javadoc -Dgphdconn.version=$(gphdver) -Dhadoop.core.version=0.20.2
	$(TAR) -cf $(gphdver)-javadoc.tar $(gphdver)-javadoc

install: all installdirs gphdfsdoc
	$(MAKE) -C src/protocol_formatter install
	$(INSTALL_PROGRAM) $(top_srcdir)/src/bin/gphdfs/dist/*.jar '$(DESTDIR)$(libdir)/hadoop/'
	$(INSTALL_PROGRAM) $(top_srcdir)/src/bin/gphdfs/hadoop_env.sh$(X) '$(DESTDIR)$(libdir)/hadoop/hadoop_env.sh$(X)'
	$(INSTALL_DATA) $(top_srcdir)/src/bin/gphdfs/$(gphdver)-javadoc.tar '$(DESTDIR)${exec_prefix}/docs/javadoc'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(libdir)'
	$(MKDIR_P) '$(DESTDIR)$(libdir)/hadoop'
	$(MKDIR_P) '$(DESTDIR)${exec_prefix}/docs/javadoc'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/*.jar'
	rm -f '$(DESTDIR)$(bindir)/hadoop_env.sh$(X)'

clean:
	$(MAKE) -C src/protocol_formatter clean
	$(ANT) clean
	rm -rf $(gphdver)-javadoc
	rm $(gphdver)-javadoc.tar
