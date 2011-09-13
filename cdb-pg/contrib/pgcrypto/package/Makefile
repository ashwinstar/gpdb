## Build a Gppkg

all: gppkg

MACHINE_ARCH=$(shell uname -p)
top_builddir=../../..
OS=$(shell uname -s)
GPMGMT=$(top_builddir)/../gpMgmt/

gppkg:  
	sed -i".bk" "s/#arch/$(MACHINE_ARCH)/g" gppkg/gppkg_spec.yml
	sed -i".bk" "s/#os/$(OS)/g" gppkg/gppkg_spec.yml
	rm -f gppkg/*.bk
	rm -rf RPMS SOURCES SRPMS BUILD
	mkdir RPMS SOURCES SRPMS BUILD
	rpmbuild -bb SPECS/pgcrypto.spec 
	cp RPMS/$(MACHINE_ARCH)/pgcrypto* gppkg/
	$(GPMGMT)/bin/gppkg -b gppkg/
						
clean:
	rm -rf RPMS SOURCES SRPMS BUILD
	rm gppkg/*.rpm

.PHONY: gppkg clean
