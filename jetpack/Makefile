JETPACK_NAME = jetpack
JETPACK_SCHEMA = gp_$(JETPACK_NAME)
JETPACK_SQL = jetpack.sql
JETPACK_VERSION = '1.0.0.0'
JETPACK_OBJECT_PREFIX = gp_
JETPACK_PREFIX = $(JETPACK_SCHEMA).$(JETPACK_OBJECT_PREFIX)
JETPACK_PREFIX_AUX = $(JETPACK_SCHEMA).__$(JETPACK_OBJECT_PREFIX)
JETPACK_TEST_SCHEMA = $(JETPACK_SCHEMA)_test

all: $(JETPACK_SQL)
$(JETPACK_SQL): Makefile

%.sql: %.sql.in
	sed \
	-e 's/%%JETPACK_NAME%%/$(JETPACK_NAME)/g' \
	-e 's/%%JETPACK_SCHEMA%%/$(JETPACK_SCHEMA)/g' \
	-e 's/%%JETPACK_TEST_SCHEMA%%/$(JETPACK_TEST_SCHEMA)/g' \
	-e 's/%%JETPACK_VERSION%%/$(JETPACK_VERSION)/g' \
	-e 's/%%JETPACK_PREFIX%%/$(JETPACK_PREFIX)/g' \
	-e 's/%%JETPACK_PREFIX_AUX%%/$(JETPACK_PREFIX_AUX)/g' \
	-e 's/	/    /g' \
	$< > $@

clean:
	$(RM) $(JETPACK_SQL)


# test targets

remove:
	psql -c "drop schema if exists $(JETPACK_SCHEMA) cascade"

deploy: jetpack.sql 
	$(MAKE) remove
	psql -f $<
	
test: test.sql
	$(MAKE) deploy
	psql -c "drop owned by foouser; \
			drop role if exists foouser; \
			create role foouser; \
			alter role foouser set search_path = $(JETPACK_TEST_SCHEMA); \
			drop schema if exists $(JETPACK_TEST_SCHEMA); \
			create schema $(JETPACK_TEST_SCHEMA); \
			grant usage on schema $(JETPACK_TEST_SCHEMA) to public; \
			grant create on schema $(JETPACK_TEST_SCHEMA) to public;"			
	psql -f $<

#	psql -c "drop role foouser"
