SHELL=/bin/bash

# This is left here for future reference. Ideally we should be generating
# the version string or a build string dynamically and use it in the
# python code as well as flex code such that there is only one global
# reference to the version and build#.
# Currently the python code also hard codes the version which is a duplication.
#GPPERFMON_VERSION=$$(< $(BLD_TOP)/gpperfmon/VERSION)

RELEASE_DIR=../bin-release
DEBUG_DIR=../bin-debug
GPMON_SWF=GPPerfMon.swf
GPMON_MXML=GPPerfMon.mxml
DEBUG_OPTS=-debug
RELEASE_OPTS=


# if Build Platform is MacOS, then find the MXML compiler at a location similar
# to the one below. Or better yet, download the standalone Flex SDKs from Adobe
# for Linux and MacOS platforms and install them in similar locations (e.g.,
# /opt/FlexSDK/3.3.0/....) and set the MXMLC path accordingly.
MXMLC=/Applications/Adobe\ Flex\ Builder\ 3/sdks/3.2.0/bin/mxmlc

default: release

all: release debug

release: 
	$(MXMLC) $(RELEASE_OPTS) $(GPMON_MXML) -output $(RELEASE_DIR)/$(GPMON_SWF)

debug: 
	$(MXMLC) $(DEBUG_OPTS) $(GPMON_MXML) -output $(DEBUG_DIR)/$(GPMON_SWF)

clean:
	rm -rf $(DEBUG_DIR)/$(GPMON_SWF) $(RELEASE_DIR)/$(GPMON_SWF)
