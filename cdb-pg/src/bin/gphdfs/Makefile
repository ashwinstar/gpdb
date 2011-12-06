top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ANT=/opt/releng/apache-ant/bin/ant
gphdver = gphd-xnet-1.1.0.0
gpmrver = gpmr-xnet-1.0.0.1
gpcdh3u2ver = gpcdh3u2-xnet-1.1.0.0

all: gphdfs protocol_formatter

protocol_formatter:
	$(MAKE) -C src/protocol_formatter

gphdfs:
	$(ANT) clean
	$(ANT) dist -Dorg=emc \
                -Dname=hadoop-core-0.20.205.0-gphd \
                -Drevision=1.1.0.0 \
                -Dgphdxnet.version=$(gphdver) \
                -Dgpxnet.src=1.1
	$(ANT) clean
	$(ANT) dist -Dorg=org.apache.hadoop \
                -Dname=hadoop-core \
                -Drevision=0.20.2 \
                -Dgphdxnet.version=$(gpmrver) \
                -Dgpxnet.src=1.0
	$(ANT) clean
	$(ANT) dist -Dorg=org.apache.hadoop \
                -Dname=hadoop-core \
                -Drevision=0.20.2-cdh3u2 \
                -Dgphdxnet.version=$(gpcdh3u2ver) \
                -Dgpxnet.src=1.1

gphdfsdoc:
	$(ANT) javadoc -Dorg=emc \
                   -Dname=hadoop-core-0.20.205.0-gphd \
                   -Drevision=1.1.0.0 \
                   -Dgphdxnet.version=$(gphdver) \
                   -Dgpxnet.src=gphd-1.1.0.0

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
	rm -f $(gphdver)-javadoc.tar
