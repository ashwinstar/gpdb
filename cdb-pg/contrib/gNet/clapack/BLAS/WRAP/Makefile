include ../../make.inc

#######################################################################
#  This is the makefile to create a wrapper for the CBLAS.
#######################################################################

all: libcblaswr.a libfblaswr.a

libcblaswr.a: cblaswr.o crotg.o zrotg.o csrot.o zdrot.o
	$(ARCH) $(ARCHFLAGS) libcblaswr.a cblaswr.o crotg.o zrotg.o csrot.o zdrot.o

libfblaswr.a: fblaswr.o crotg.o zrotg.o csrot.o zdrot.o
	$(ARCH) $(ARCHFLAGS) libfblaswr.a fblaswr.o crotg.o zrotg.o csrot.o zdrot.o

clean:
	rm -f *.o *.a

.c.o: 
	$(CC) $(CFLAGS) -c $< -o $@


