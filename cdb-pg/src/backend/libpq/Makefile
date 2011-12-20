#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for libpq subsystem (backend half of libpq interface)
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/libpq/Makefile,v 1.39 2008/02/19 10:30:07 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/libpq
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SYSTEM_NAME=$(shell uname -s)
PLATFORM_Darwin=PLATFORM_MACOSX
PLATFORM_Linux=PLATFORM_LINUX
PLATFORM_SunOS=PLATFORM_SOLARIS

RSA_PLATFORM=$(PLATFORM_$(SYSTEM_NAME))

# Flags for RSA BSAFE. Yes, this quantity of flags is totally gross
# but we cannot do anything about it.
RSACFLAGS = $(CFLAGS) -DCRYPTOCME_FIPSPROV -DR_OPT_FIPS140 \
	-DCRYPTOC_ME -DNO_PK_CERT -DCPU_X86 -DL_ENDIAN -DTERMIOS \
	-DRSA_PLATFORM=RSA_AMD_LINUX -DPTHREAD -D_REENTRANT -D$(RSA_PLATFORM) \
	-DFLAT_INC -DNO_PKCS12 -DNO_CRL -DNO_CERT -DNO_NTR -DNO_ERR -DNO_WTLS \
	-DNO_EX_DATA -DNO_PKI_SIZE -DNO_OCSP -DNO_PKCS7 -DNO_EXT -DNO_SOCK \
	-DNO_HDW_CERT -DNO_RC6 -DNO_X509CERT -DNO_CERTIMPL -DNO_CM -DNO_R_DIAG \
	-DNO_WTLSCERT -DNO_PROXY

RSA_FILE = rsa_bsafe_sha2

# be-fsstubs is here for historical reasons, probably belongs elsewhere

OBJS = be-fsstubs.o be-secure.o auth.o crypt.o hba.o ip.o md5.o pqcomm.o \
       pqformat.o pqsignal.o sha2.o pg_sha2.o $(RSA_FILE).o

$(RSA_FILE).o : $(RSA_FILE).c
	$(CC) $(CFLAGS) $(RSACFLAGS) $(CPPFLAGS) -c -o $@ $<

include $(top_srcdir)/src/backend/common.mk
