OS=`uname -s`

line_delim.o: line_delim.c
	if [ $(OS) = 'Darwin' ]; then \
		gcc -c line_delim.c -Wall -Werror -I$(GPHOME)/include/postgresql/internal -I$(GPHOME)/include/postgresql/server; \
		gcc -bundle -flat_namespace -undefined suppress -o line_delim.so line_delim.o; \
	elif [ $(OS) = 'Linux' ]; then \
		gcc -fpic -c line_delim.c -Wall -I$(GPHOME)/include/postgresql/internal -I$(GPHOME)/include/postgresql/server; \
		gcc -shared -o line_delim.so line_delim.o; \
	else \
		echo "OS $(OS) is not supported"; \
	fi

.PHONY: install
install:
	cp -f line_delim.so ${GPHOME}/lib/postgresql/
	
.PHONY: clean
clean:
	rm *.o