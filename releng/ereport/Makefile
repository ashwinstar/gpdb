all: mgmt_errors.txt

BLD_TOP=../..
PWD=$(shell pwd)
gpMgmt=$(BLD_TOP)/gpMgmt
include $(BLD_TOP)/Makefile.global

mgmt_errors.txt: 
	cd $(gpMgmt) && grep -R . --with-filename --line-number \
						-e "\[FATAL\]" \
						-e "\[ERROR\]" \
						-e "\[WARNING\]" \
						-e "FATAL:" \
						-e "ERROR:" \
						-e "WARNING:" \
						-e "fatal(.*'" \
						-e "fatal(.*\"" \
						-e "exception(.*'" \
						-e "exception(.*\"" \
						-e "critical(.*'" \
						-e "critical(.*\"" \
						-e "error(.*'" \
						-e "error(.*\"" \
						-e "warning(.*'" \
						-e "warning(.*\"" \
						-e "warn(.*'" \
						-e "warn(.*\"" \
						-e "raise.*'" \
						-e "raise.*\"" \
						-e "sys\.exit.*'" \
						-e "sys\.exit.*\"" \
						| grep -v \
							-e "TODO" \
							-e "^Binary file" \
							-e "^\.\/bin\/demo" \
							-e "^\.\/bin\/doc" \
							-e "^\.\/bin\/ext" \
							-e "^\.\/bin\/pythonSrc" \
							-e "^\.\/doc" \
							-e "^\.\/legacy" \
						> $(PWD)/$@

clean:
	rm -f mgmt_errors.txt
