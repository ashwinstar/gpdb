all: gppkg

BLD_TOP=../../../../
GPMGMT=$(BLD_TOP)/gpMgmt
include $(BLD_TOP)/releng/tools.mk
include $(BLD_TOP)/Makefile.global

OS=$(word 1,$(subst _, ,$(BLD_ARCH)))
ARCH=$(shell uname -p)

GEOS_VER=3.2.2
GEOS_REL=1
GEOS_DIR=$(RELENG_TOOLS)/geos-$(GEOS_VER)
GEOS_CONFIG=$(GEOS_DIR)/bin/geos-config
GEOS_RPM=geos-$(GEOS_VER)-$(GEOS_REL).$(ARCH).rpm

PROJ_VER=4.7.0
PROJ_REL=1
PROJ_DIR=$(RELENG_TOOLS)/proj-$(PROJ_VER)
PROJ_RPM=proj-$(PROJ_VER)-$(PROJ_REL).$(ARCH).rpm

POSTGIS_VER=1.0
POSTGIS_REL=1
POSTGIS_RPM=postgis-$(POSTGIS_VER)-$(POSTGIS_REL).$(ARCH).rpm
POSTGIS_GPPKG=postgis-$(POSTGIS_VER)-$(OS)-$(ARCH).gppkg

gppkg: $(POSTGIS_GPPKG)

# e.g. 
# postgis-1.4.2-1-x86_64.rpm
RPM_ARGS=$(subst -, ,$*)
RPM_NAME=$(word 1,$(RPM_ARGS))
%.rpm:
	mkdir RPMS SOURCES BUILD SRPMS
	rpmbuild -bb SPECS/$(RPM_NAME).spec --define "geos_dir $(GEOS_DIR)" --define "proj_dir $(PROJ_DIR)" \
										--define "geos_ver $(GEOS_VER)" --define "proj_ver $(PROJ_VER)"
	mv RPMS/$(ARCH)/$*.rpm .
	rm -rf RPMS SOURCES BUILD SRPMS

gppkg/gppkg_spec.yml: gppkg/gppkg_spec.yml.bk
	cat $< | sed "s/#arch/$(ARCH)/g" | sed "s/#os/$(OS)/g" > $@
	
$(POSTGIS_GPPKG): gppkg/gppkg_spec.yml $(GEOS_DIR) $(PROJ_DIR) $(POSTGIS_RPM) $(PROJ_RPM) $(GEOS_RPM)
	rm -rf gppkg/deps 
	mkdir gppkg/deps 
	cp $(POSTGIS_RPM) gppkg/ 
	cp $(GEOS_RPM) gppkg/deps 
	cp $(PROJ_RPM) gppkg/deps 
	source $(INSTLOC)/greenplum_path.sh && $(GPMGMT)/bin/gppkg -b gppkg 

clean:
	rm -rf RPMS SOURCES BUILD SRPMS
	rm -rf gppkg/deps
	rm -rf geos
	rm -rf proj
	rm -f gppkg/gppkg_spec.yml
	rm -f $(POSTGIS_GPPKG) $(POSTGIS_RPM) $(GEOS_RPM) $(PROJ_RPM)

.PHONY: gppkg clean
