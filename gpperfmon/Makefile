.PHONY: installer

all: gpsmon gpmmon installer

gpsmon:
	$(MAKE) -C src/gpmon gpsmon

gpmmon:
	$(MAKE) -C src/gpmon gpmmon

webapi:
	@echo webapi

flexGUI:
	@echo flexGUI

installer: all
	$(MAKE) -C installer all

clean:
	$(MAKE) -C src/gpmon clean
	$(MAKE) -C installer clean
