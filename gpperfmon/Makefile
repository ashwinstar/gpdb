.PHONY: installer

all: gpsmon gpmmon flexGUI installer

gpsmon:
	$(MAKE) -C src/gpmon gpsmon

gpmmon:
	$(MAKE) -C src/gpmon gpmmon

webapi:
	@echo webapi

FLEXSDK=$(HOME)/flex_sdk_3.4.0.9271_mpl

flexGUI:
	@echo flexGUI
	[ -d "$(FLEXSDK)" ] && $(MAKE) MXMLC=$(FLEXSDK)/bin/mxmlc -C src/webui/flexGUI/src release

installer: gpsmon gpmmon
	$(MAKE) -C installer all

clean:
	$(MAKE) -C src/gpmon clean
	$(MAKE) -C installer clean
