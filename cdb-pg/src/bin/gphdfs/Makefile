top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

all:

gphdfs: HDFSReader.class HDFSWriter.class

HDFSReader.class:
	javac HDFSReader.java

HDFSWriter.class:
	javac HDFSWriter.java

install: all installdirs
	$(INSTALL_PROGRAM) HDFSReader.class$(X) '$(DESTDIR)$(bindir)/HDFSReader.class$(X)'
	$(INSTALL_PROGRAM) HDFSWriter.class$(X) '$(DESTDIR)$(bindir)/HDFSWriter.class$(X)'
	$(INSTALL_PROGRAM) hadoop_env.sh$(X) '$(DESTDIR)$(bindir)/hadoop_env.sh$(X)'


installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/HDFSReader.class$(X)'
	rm -f '$(DESTDIR)$(bindir)/HDFSWriter.class$(X)'
	rm -f '$(DESTDIR)$(bindir)/hadoop_env.sh$(X)'


cleanjava:
	rm -f *.class

