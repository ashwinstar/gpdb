.PHONY: installer

all: gpsmon gpmmon flexGUI installer

gpsmon:
	$(MAKE) -C src/gpmon gpsmon

gpmmon:
	$(MAKE) -C src/gpmon gpmmon

webapi:
	@echo webapi

FLEXSDK=$(HOME)/flex_sdk_3.4.0.9271_mpl

p4_edit:
	cd src/webui/flexGUI/bin-release; \
	p4 edit GPPerfMon_2.0.0.0.swf

p4_submit:
	cd src/webui/flexGUI/bin-release; \
	p4 submit -d "Update of flexGUI" GPPerfMon_2.0.0.0.swf

flexGUI:
	@echo flexGUI
	if [ -d "$(FLEXSDK)" ]; then \
       $(MAKE) MXMLC=$(FLEXSDK)/bin/mxmlc -C src/webui/flexGUI/src release; \
    fi

installer: gpsmon gpmmon
	$(MAKE) -C installer all

clean:
	$(MAKE) -C src/gpmon clean
	$(MAKE) -C installer clean
