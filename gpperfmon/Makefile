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
	p4 client -o
	p4 files *
	p4 files ./src/webui/flexGUI/bin-release/GPPerfMon_2.0.0.0.swf
	p4 edit ./src/webui/flexGUI/bin-release/GPPerfMon_2.0.0.0.swf

p4_submit:
	p4 submit -d "Update of flexGUI" src/webui/flexGUI/bin-release/GPPerfMon_2.0.0.0.swf

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
