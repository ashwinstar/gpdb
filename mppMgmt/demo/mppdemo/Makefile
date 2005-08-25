all: clean prereqsOK 
	@echo "Building cluster, this may take a few minutes."
	@./demo_cluster.sh
	@echo "Probing cluster configuration."
	@./probe_config.sh
	@echo "Cluster build successful."

.PHONY : clean prereqsOK

prereqsOK:
	@if [ `netstat -an | grep 5432 | wc -l` -ne 0 ]; then echo "Port 5432 appears to be in use, this port is needed by the QD and must be free to proceed. Check to see the port is free using 'netstat -an | grep 5432'."; exit 1; fi
	@if [ `netstat -an | grep 10000 | wc -l` -ne 0 ]; then echo "Port 10000 appears to be in use, this port is needed by the QD and must be free to proceed. Check to see the port is free using 'netstat -an | grep 10000'."; exit 1; fi
	@if [ `netstat -an | grep 10001 | wc -l` -ne 0 ]; then echo "Port 10001 appears to be in use, this port is needed by the QD and must be free to proceed. Check to see the port is free using 'netstat -an | grep 10001'."; exit 1; fi
	@if [ `netstat -an | grep 10002 | wc -l` -ne 0 ]; then echo "Port 10002 appears to be in use, this port is needed by the QD and must be free to proceed. Check to see the port is free using 'netstat -an | grep 10002'."; exit 1; fi
	@echo "Ports 5432,10000,10001,10002 appears to be free, good."


clean:
	@-killall postmaster
	@rm -f hostfile
	@rm -f clusterConfigFile
	@rm -rf datadirs
	@rm -rf logs
	@sleep 2
