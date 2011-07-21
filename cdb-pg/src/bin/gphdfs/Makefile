top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

#
# Because it's hard to build these class files, due to not having the Hadoop files on the build machine,
# the class files are in perforce, and we don't do anything here on a make all or make clean.
#
all:

gphdfs: HDFSReader.class HDFSWriter.class

HDFSReader.class:
	javac HDFSReader.java

HDFSWriter.class:
	javac HDFSWriter.java

install: all installdirs
	$(INSTALL_PROGRAM) $(top_srcdir)/src/bin/gphdfs/HDFSReader.class$(X) '$(DESTDIR)$(bindir)/HDFSReader.class$(X)'
	$(INSTALL_PROGRAM) $(top_srcdir)/src/bin/gphdfs/HDFSWriter.class$(X) '$(DESTDIR)$(bindir)/HDFSWriter.class$(X)'
	$(INSTALL_PROGRAM) $(top_srcdir)/src/bin/gphdfs/hadoop_env.sh$(X) '$(DESTDIR)$(bindir)/hadoop_env.sh$(X)'
	$(INSTALL_PROGRAM) $(top_srcdir)/src/bin/gphdfs/HDFSReader.class.0.20.1$(X) '$(DESTDIR)$(bindir)/HDFSReader.class.0.20.1$(X)'
	$(INSTALL_PROGRAM) $(top_srcdir)/src/bin/gphdfs/HDFSWriter.class.0.20.1$(X) '$(DESTDIR)$(bindir)/HDFSWriter.class.0.20.1$(X)'


installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/HDFSReader.class$(X)'
	rm -f '$(DESTDIR)$(bindir)/HDFSWriter.class$(X)'
	rm -f '$(DESTDIR)$(bindir)/hadoop_env.sh$(X)'


cleanjava:
	rm -f *.class

