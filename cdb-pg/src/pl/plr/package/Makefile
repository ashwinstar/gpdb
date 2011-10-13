all: gppkg

BLD_TOP=../../../../../
GPMGMT=$(BLD_TOP)/gpMgmt
include $(BLD_TOP)/releng/tools.mk
include $(BLD_TOP)/Makefile.global

OS=$(word 1,$(subst _, ,$(BLD_ARCH)))
ARCH=$(shell uname -p)

R_VER=2.13.0
R_REL=1
R_DIR=$(RELENG_TOOLS)/R-$(R_VER)
R_RPM=R-$(R_VER)-$(R_REL).$(ARCH).rpm
R_HOME=$(R_DIR)/lib64/R

PLR_VER=1.0
PLR_REL=1
PLR_RPM=plr-$(PLR_VER)-$(PLR_REL).$(ARCH).rpm
PLR_GPPKG=plr-$(PLR_VER)-$(OS)-$(ARCH).gppkg

gppkg: $(PLR_GPPKG)

# e.g. 
# postgis-1.4.2-1-x86_64.rpm
RPM_ARGS=$(subst -, ,$*)
RPM_NAME=$(word 1,$(RPM_ARGS))
%.rpm:
	mkdir RPMS SOURCES BUILD SRPMS
	rpmbuild -bb SPECS/$(RPM_NAME).spec  --define "r_ver $(R_VER)" --define "plr_ver $(PLR_VER)" \
										 --define "r_rel $(R_REL)" --define "plr_rel $(PLR_REL)" \
										 --define "r_dir $(R_DIR)"
	mv RPMS/$(ARCH)/$*.rpm .
	rm -rf RPMS SOURCES BUILD SRPMS

gppkg/gppkg_spec.yml: gppkg/gppkg_spec.yml.bk
	cat $< | sed "s/#arch/$(ARCH)/g" | sed "s/#os/$(OS)/g" > $@

$(PLR_GPPKG): gppkg/gppkg_spec.yml $(R_DIR) $(R_RPM) $(PLR_RPM)
	rm -rf gppkg/deps 
	mkdir gppkg/deps
	cp $(PLR_RPM) gppkg/ 
	cp $(R_RPM) gppkg/deps/ 
	source $(INSTLOC)/greenplum_path.sh && $(GPMGMT)/bin/gppkg -b gppkg 

clean:
	rm -rf RPMS SRPMS SOURCES BUILD
	rm -rf gppkg/deps
	rm -rf R-$(R_VER).$(R_REL) install
	rm -f gppkg/gppkg_spec.yml
	rm -f $(R_RPM) $(PLR_RPM) $(PLR_GPPKG)

.PHONY: gppkg clean
