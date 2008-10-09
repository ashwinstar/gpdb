GPMON_DIR=../src/gpmon

all:
	if [ -d .tmp ]; then \
		rm -rf .tmp; \
	fi;
	if [ -a greenplum-perfmon.tar.gz ]; then \
		rm -f greenplum-perfmon.tar.gz; \
	fi;
	mkdir .tmp
	mkdir .tmp/bin
	mkdir .tmp/lib
	mkdir .tmp/docs
	cp $(GPMON_DIR)/gpmmon $(GPMON_DIR)/gpsmon ../src/gpperfmon .tmp/bin
	cp $(GPMON_DIR)/gpmon_catqrynow.py .tmp/lib
	cp $(GPMON_DIR)/gpperfmon.sql .tmp
	./generate-gpperfmon-path.sh > .tmp/gpperfmon_path.sh
	cd .tmp && tar cvf ../greenplum-perfmon.tar *
	gzip greenplum-perfmon.tar
	./create-gpperfmon-installer.sh
	rm -rf .tmp

clean:
	rm -rf .tmp
	rm -f greenplum-perfmon.tar.gz greenplum-perfmon.bin
