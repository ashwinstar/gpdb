#include <mapred.h>
#include <except.h>
#include <mapred_errors.h>

#include <stdio.h>
#include <errno.h>

#define error1(x,y)									\
	do {											\
		fprintf(stderr, "Error: %s %s\n", x, y);	\
		exit(1);									\
	} while (0)

#define scalarfree(x)							\
	do {										\
		if (x) {								\
			free(x);							\
			x = NULL;							\
		}										\
	} while (0)


/* instantiate the extern from mapred.h */
const char *mapred_kind_name[MAPRED_MAXKIND+1] =
{
	"<NULL>",
	"DOCUMENT",
	"INPUT",
	"OUTPUT",
	"MAP",
	"TRANSITION",
	"COMBINER",
	"FINALIZER",
	"REDUCER",
	"TASK",
	"RUN",
	"INTERNAL"
};

/* 
 * libpq Errors that we care about
 * (would be better to add <errcodes.h> to the include path)
 */
const char *IN_FAILED_SQL_TRANSACTION = "25P02";
const char *OBJ_DOES_NOT_EXIST        = "42P01";


/* Wrappers around malloc/free to handle error conditions more cleanly */
void *mapred_malloc(int size)
{
	void *m;
	XASSERT(size > 0);

	if (global_debug_flag && global_verbose_flag)
		fprintf(stderr, "Allocating %d bytes: ", size);

	m = malloc(size);
	if (!m)
		XRAISE(MEMORY_ERROR, "Memory allocation failure");

	if (global_debug_flag && global_verbose_flag)
		fprintf(stderr, "%x\n", m);

	return m;
}

void mapred_free(void *ptr)
{
	XASSERT(ptr);
	if (global_debug_flag && global_verbose_flag)
		fprintf(stderr, "Freeing memory: %x\n", ptr);
	free(ptr);
}

/* -------------------------------------------------------------------------- */
/* Functions that play with buffers
/* -------------------------------------------------------------------------- */
typedef struct 
{
	char   *buffer;
	size_t  bufsize;
	size_t  position;
	size_t  grow;
} buffer_t;

buffer_t *makebuffer(size_t bufsize, size_t grow)
{
	buffer_t *b;

	XASSERT(bufsize > 0 && grow > 0);

	b = mapred_malloc(sizeof(buffer_t) + bufsize);
	b->buffer   = (char*)(b+1);
	b->bufsize  = bufsize;
	b->grow     = grow;
	b->position = 0;
	b->buffer[0] = '\0';
	return b;
}

/* to re-use a buffer just "reset" it */
void bufreset(buffer_t *b)
{
	XASSERT(b && b->bufsize > 0 && b->grow > 0);
	b->position = 0;
	b->buffer[0] = '\0';
}

/* 
 * A simple wrapper around a strncpy that handles resizing an input buffer
 * when needed.
 */
void bufcat(buffer_t **bufp, char* str)
{
	buffer_t  *b;
	size_t     len;

	XASSERT(bufp && *bufp);
	XASSERT(str);

	b = *bufp;
	len = strlen(str);
	
	/* If the buffer is too small, grow it */
	if (b->bufsize <= b->position + len)
	{
		buffer_t *newbuf;

		/* use the minumum of "grow" and the new length for the grow amount */
		if (b->grow <= len)
			b->grow = len+1;

		newbuf = makebuffer(b->bufsize + b->grow, b->grow);
		memcpy(newbuf->buffer, b->buffer, b->position+1);
		newbuf->position = b->position;
		*bufp = newbuf;
		mapred_free(b);
		b = newbuf;
	}

	/* We are now guaranteed that we have enough space in the buffer */
	XASSERT( b->bufsize - b->position > len );
	strcpy(b->buffer+b->position, str);
	b->position += len;
	b->buffer[b->position] = '\0';
}


void mapred_setup_columns(PGconn *conn, mapred_object_t *obj);

/*
 * Currently we just ignore all warnings, may eventually do something
 * smarter, but this is preferable to dumping them to libpq's default
 * of dumping them to stderr.
 */
void ignore_notice_handler(void *arg, const PGresult *res) 
{
}

void print_notice_handler(void *arg, const PGresult *res) 
{
	fprintf(stderr, PQresultErrorMessage(res));
}


void check_version(PGconn *conn)
{
	PGresult *result;

	result = PQexec(conn, "select (regexp_matches(version(), "
					"E'\\\\(Greenplum Database ([^)]+)\\\\)'))[1]");

	if (PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) == 1)
	{
		char *version = PQgetvalue(result, 0, 0);

		if (global_verbose_flag)
			fprintf(stderr, "    VERSION: %s\n", version);

		/* Determine if this version is approved for mapreduce usage */
		if (strncmp(version, "main", 4) != 0)
		{
			/* Get major and minor release, patch level is ignored */
			char *dot   = NULL;
			int   major = strtol(version, &dot, 10);
			int   minor = strtol(dot+1, NULL, 10);
			if (major < 3 || (major == 3 && minor < 2))
			{
				char buf[100];
				snprintf(buf, sizeof(buf), 
						 "Unsupported backend version: %s", version);
				XRAISE(VERSION_ERROR, buf);	
			}
		}
	}
	else
	{
		fprintf(stderr, PQerrorMessage(conn));
		XRAISE(MAPRED_SQL_ERROR, "Unable to determine Greenplum version\n");
	}

	PQclear(result);
}



void mapred_run_document(PGconn *conn, mapred_document_t *doc)
{
	PGresult       *result;
	mapred_olist_t *olist;
	boolean         done;

	if (global_verbose_flag)
	{
		fprintf(stderr, "  - Connected Established:\n");
		fprintf(stderr, "    HOST: %s\n", 
				PQhost(conn) ? PQhost(conn) : "localhost");
		fprintf(stderr, "    PORT: %s\n", PQport(conn));
		fprintf(stderr, "    USER: %s/%s\n", PQuser(conn), PQdb(conn));
	}
	check_version(conn);


	/* Ignore NOTICE messages from database */
	PQsetNoticeReceiver(conn, ignore_notice_handler, NULL);

	/*
	 * Resolution of dependecies was defered until now so that
	 * a database connection could be available to look up any
	 * dependencies that are not defined within the YAML document.
	 */
	mapred_resolve_dependencies(conn, doc);

	XTRY
	{
		/*
		 * By running things within a transaction we can effectively
		 * obscure the mapreduce sql definitions.  They could still
		 * be exposed by a savy user via mapreduce views that access
		 * the catalog tables, but it's a cleaner method of handling
		 * things.
		 */
		result = PQexec(conn, "BEGIN TRANSACTION");
		if (PQresultStatus(result) != PGRES_COMMAND_OK)
		{
			PQclear(result);
			XRAISE(MAPRED_SQL_ERROR, NULL);
		}
		PQclear(result);

		/* With dependencies resolved we can now try creating objects */
		if (global_verbose_flag)
			fprintf(stderr, "  - Creating Objects\n");
		do
		{
			boolean progress = false;

			/* 
			 * Loop through the objects, creating each in turn.
			 * If an object has dependencies that have not been created yet
			 * it will return false and we will make additional passes through
			 * the object list 
			 */  
			done  = true;
			for (olist = doc->objects; olist; olist = olist->next)
			{
				mapred_object_t *obj = olist->object;
				if (!obj->created)
				{
					if (global_verbose_flag)
					{
						fprintf(stderr, "    - %s:\n", 
								mapred_kind_name[obj->kind]);
						fprintf(stderr, "       NAME: %s\n", obj->name);
					}

					if (!mapred_create_object(conn, obj))
						done = false;
					else
						progress = true;
				}
			}
			
			/* 
			 * If we looped through the list, we are not done, and no progress
			 * was made then we have an infinite cycle and should probably stop.
			 */
			if (!progress)
			{
				/* FIXME - produce better error message */
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Unable to make forward progress creating objects\n");
			}

		} while (!done);

		/* objects created, execute queries */
		mapred_run_queries(conn, doc);
	}
	XCATCH(MAPRED_SQL_ERROR)
	{
		fprintf(stderr, PQerrorMessage(conn));
		XRERAISE();
	}
	XFINALLY
	{
		/* Remove all the objects that we created */
		if (global_print_flag || global_debug_flag)
			printf("\n");
		for (olist = doc->objects; olist; olist = olist->next)
			mapred_remove_object(conn, olist->object);
		if (global_print_flag || global_debug_flag)
			printf("\n");

		/*
		 * We always commit the transaction, even on failure since the failure
		 * may have occured after we generated some output tables and we want
		 * to keep the partial results.
		 */
		result = PQexec(conn, "COMMIT TRANSACTION");
		if (PQresultStatus(result) != PGRES_COMMAND_OK)
			fprintf(stderr, PQerrorMessage(conn));
	}
	XTRY_END;
}


void mapred_resolve_dependencies(PGconn *conn, mapred_document_t *doc)
{
	mapred_olist_t     *olist;
	int                 exec_count = 0;

	/* Walk the list of objects */
	for (olist = doc->objects; olist; olist = olist->next)
		mapred_resolve_object(conn, doc, olist->object, &exec_count);

}

void mapred_resolve_object(PGconn *conn, mapred_document_t *doc, 
						   mapred_object_t *obj, int *exec_count)
{
	mapred_object_t *sub;   /* sub-object */

	switch (obj->kind)
	{
		/* Objects with no dependencies */
		case MAPRED_INPUT:
		case MAPRED_OUTPUT:
			break;

		case MAPRED_MAPPER:
		case MAPRED_TRANSITION:
		case MAPRED_COMBINER:
		case MAPRED_FINALIZER:
			XASSERT(obj->u.function.returns);

			/* 
			 * The function types may manufacture a dependency on an adt, 
			 * but have no other dependencies.
			 */
			if (obj->u.function.returns->next)
			{
				mapred_olist_t  *newlist;
				mapred_object_t *rtype;
				size_t           len;

				rtype = mapred_malloc(sizeof(mapred_object_t));
				memset(rtype, 0, sizeof(mapred_object_t));
				rtype->kind = MAPRED_ADT;
				len = strlen(obj->name) + 7;
				rtype->name = mapred_malloc(len);
				snprintf(rtype->name, len, "%s_rtype", obj->name);
				rtype->u.adt.returns = obj->u.function.returns;
				
				obj->u.function.rtype.name = rtype->name;
				obj->u.function.rtype.object = rtype;

				/* Add the ADT to the list of document objects */
				newlist = mapred_malloc(sizeof(mapred_olist_t));
				newlist->object = rtype;
				newlist->next = doc->objects;
				doc->objects = newlist;
			}
			else
			{
				obj->u.function.rtype.name = obj->u.function.returns->type;
				obj->u.function.rtype.object = NULL;
			}
			break;


		case MAPRED_REDUCER:
		{
			mapred_resolve_ref(doc->objects, &obj->u.reducer.transition);
			mapred_resolve_ref(doc->objects, &obj->u.reducer.combiner);
			mapred_resolve_ref(doc->objects, &obj->u.reducer.finalizer);
			/* FIXME: validate dependencies */
			break;
		}


		case MAPRED_TASK:
		case MAPRED_EXECUTION:
		{
			/* 
			 * Resolving a task may require recursion to resolve other
			 * tasks to work out parameter lists.  We keep track of 
			 * our resolution state in order to detect potential 
			 * infinite recursion issues.
			 */
			if (obj->u.task.flags & mapred_task_resolved)
				return;

			/* Assign a name to anonymous executions */
			if (!obj->name)
			{
				int        pid = PQbackendPID(conn);
				size_t     len;

				XASSERT(obj->u.task.execute);

				/* 10 characters for max int digits */
				len = 10 + 10 + strlen("mapreduce__run_") + 1;
				obj->name = mapred_malloc(len);
				snprintf(obj->name, len, "mapreduce_%d_run_%d",
						 pid, ++(*exec_count));
			}

			/* Check for infinite recursion */
			if (obj->u.task.flags & mapred_task_resolving)
			{
				fprintf(stderr, 
						"Error: Infinite recursion detected trying to resolve TASK '%s'.\n",
						obj->name);
				XRAISE(MAPRED_PARSE_ERROR, NULL);
			}
			obj->u.task.flags |= mapred_task_resolving;

			/* Validate object types */
			if (obj->u.task.input.name)
			{
				mapred_resolve_ref(doc->objects, &obj->u.task.input);
				sub = obj->u.task.input.object;
				
				/* If we can't find the input, throw an error */
				if (!sub)
				{
					/* Can't find INPUT object */
					if (obj->u.task.execute)
						fprintf(stderr, "Error: RUN '%s'", obj->name);
					else
						fprintf(stderr, "Error: TASK '%s'", obj->name);
					fprintf(stderr, " SOURCE '%s' not found in document\n",
							obj->u.task.input.name);
					XRAISE(MAPRED_PARSE_ERROR, NULL);
				}

				/* 
				 * The input must either be an INPUT or a TASK 
				 */
				switch (sub->kind)
				{
					case MAPRED_INPUT:
						break;

					case MAPRED_TASK:
						/* This objects input is the sub objects output */
						mapred_resolve_object(conn, doc, sub, exec_count);
						break;
							
						/* Otherwise generate an error */
					default:

						/* SOURCE wasn't an INPUT */
						if (obj->u.task.execute)
							fprintf(stderr, "Error: RUN '%s'", obj->name);
						else
							fprintf(stderr, "Error: TASK '%s'", obj->name);
						fprintf(stderr, 
								" SOURCE '%s' is neither an INPUT nor a TASK\n",
								obj->u.task.input.name);
						XRAISE(MAPRED_PARSE_ERROR, NULL);
				}
			}
			
			if (obj->u.task.mapper.name)
			{
				mapred_resolve_ref(doc->objects, &obj->u.task.mapper);
				sub = obj->u.task.mapper.object;
				
				if (!sub)
				{
					/* FIXME: Lookup function in database */
					/* for now... do nothing */
				}
				else
				{
					/* Allow any function type */
					switch (sub->kind)
					{
						case MAPRED_MAPPER:
						case MAPRED_TRANSITION:
						case MAPRED_COMBINER:
						case MAPRED_FINALIZER:
							break;

						default:
							if (obj->u.task.execute)
								fprintf(stderr, "Error: RUN '%s' ", obj->name);
							else
								fprintf(stderr, "Error: TASK '%s' ", obj->name);
							fprintf(stderr, "MAPPER '%s' is not a MAP object\n",
									obj->u.task.mapper.name);
							XRAISE(MAPRED_PARSE_ERROR, NULL);
					}
				}
			}

			if (obj->u.task.reducer.name)
			{
				mapred_resolve_ref(doc->objects, &obj->u.task.reducer);
				sub = obj->u.task.reducer.object;
				
				if (!sub)
				{
					/* FIXME: non-yaml reducers */
				}
				else if (sub->kind == MAPRED_REDUCER)
				{   /* Validate Reducer */
					mapred_resolve_object(conn, doc, sub, exec_count);					
				}
				else 
				{   /* It's an object, but not a REDUCER */
					if (obj->u.task.execute)
						fprintf(stderr, "Error: RUN '%s' ", obj->name);
					else
						fprintf(stderr, "Error: TASK '%s' ", obj->name);
					fprintf(stderr, " REDUCER '%s' is not a REDUCER object.\n",
							obj->u.task.reducer.name);
					XRAISE(MAPRED_PARSE_ERROR, NULL);
				}
			}

			if (obj->u.task.output.name)
			{
				mapred_resolve_ref(doc->objects, &obj->u.task.output);

				sub = obj->u.task.output.object;
				if (!sub)
				{
					if (obj->u.task.execute)
						fprintf(stderr, "Error: RUN '%s' ", obj->name);
					else
						fprintf(stderr, "Error: TASK '%s' ", obj->name);
					fprintf(stderr, "OUTPUT '%s' is not defined in document.\n",
							obj->u.task.reducer.name);
					XRAISE(MAPRED_PARSE_ERROR, NULL);
				}
				else if (sub->kind == MAPRED_OUTPUT)
				{
					/* nothin' special */
				}
				else
				{
					if (obj->u.task.execute)
						fprintf(stderr, "Error: RUN '%s' ", obj->name);
					else
						fprintf(stderr, "Error: TASK '%s' ", obj->name);
					fprintf(stderr, "OUTPUT '%s' is not an OUTPUT object.\n",
							obj->u.task.output.name);
					XRAISE(MAPRED_PARSE_ERROR, NULL);
				}
			}
	
			/* clear resolving bit and set resolved bit */
			obj->u.task.flags &= !mapred_task_resolving;
			obj->u.task.flags |= mapred_task_resolved;
			break;
		}

		default:
			XASSERT(false);
	}
}

/* 
 * mapred_setup_columns -
 *   setup column lists (input, output, grouping, etc)
 *
 *   This is usually able to be determined directly from the YAML,
 *   but for some things (defined in the database rather than in
 *   the YAML, eg QUERY INPUTS) we can not determine the columns
 *   until the object has been created.  Which can trickle down to
 *   any object that depends on it.  
 *
 *   For this reason we don't setup the columns during the parse phase,
 *   but rather just before or just after we actually create the object
 *   once we know that all the dependencies have already been created.
 */
void mapred_setup_columns(PGconn *conn, mapred_object_t *obj)
{
	mapred_object_t *sub;
	PGresult        *result;

	/* switch based on object type */
	switch (obj->kind)
	{
		case MAPRED_ADT:
			break;

		case MAPRED_INPUT:

			/* Should be called after creation, otherwise catalog queries could fail */
			XASSERT(obj->created);

			/* setup the column list for database defined inputs */
			if (obj->u.input.type == MAPRED_INPUT_TABLE ||
				obj->u.input.type == MAPRED_INPUT_QUERY)
			{
				/* 
				 * This gets the ordered list of columns for the first
				 * input of the given name in the user's search path.
				 */
				buffer_t *buffer = makebuffer(1024, 1024);
				bufcat(&buffer, "SELECT      attname, format_type \n");
				bufcat(&buffer, "FROM \n");
				bufcat(&buffer, "  (SELECT   rank() over (order by i),\n");
				bufcat(&buffer, "            a.attnum, a.attname, \n");
				bufcat(&buffer, "            pg_catalog.format_type(a.atttypid, a.atttypmod)\n");
				bufcat(&buffer, "   FROM     pg_catalog.pg_attribute a, \n");
				bufcat(&buffer, "            pg_catalog.pg_class c, \n");
				bufcat(&buffer, "            pg_catalog.pg_namespace n, \n");
				bufcat(&buffer, "            generate_series(1, array_upper(current_schemas(true), 1)) x(i) \n");
				bufcat(&buffer, "   WHERE    a.attrelid = c.oid \n");
				bufcat(&buffer, "     AND    a.attnum > 0 \n");
				bufcat(&buffer, "     AND    c.relkind in ('t', 'v', 'a') \n");
				bufcat(&buffer, "     AND    c.relnamespace = n.oid \n");
				bufcat(&buffer, "     AND    c.relname = '");

				if (obj->u.input.type == MAPRED_INPUT_TABLE)
					bufcat(&buffer, obj->u.input.desc);
				else
					bufcat(&buffer, obj->name);

				bufcat(&buffer, "' \n");
				bufcat(&buffer, "     AND    n.nspname = (current_schemas(true))[i]) subq \n");
				bufcat(&buffer, "WHERE rank = 1 \n");
				bufcat(&buffer, "ORDER BY -attnum;\n\n");
			
				if (global_debug_flag)
					printf("%s", buffer->buffer);			
						
				result = PQexec(conn, buffer->buffer);
				mapred_free(buffer);
				if (PQresultStatus(result) == PGRES_TUPLES_OK &&
					PQntuples(result) > 0)
				{
					mapred_plist_t *newitem;
					int i, ntup;

					/* Destroy any previous default values we setup */
					mapred_destroy_plist(&obj->u.input.columns);

					/* 
					 * The columns were sorted reverse order above so
					 * the list can be generated back -> front
					 */
					for (i = 0; i < PQntuples(result); i++)
					{
						char *name = PQgetvalue(result, i, 0);
						char *type = PQgetvalue(result, i, 1);
					
						/* Add the column to the list */
						newitem = mapred_malloc(sizeof(mapred_plist_t));
						newitem->name = mapred_malloc(strlen(name)+1);
						strncpy(newitem->name, name, strlen(name)+1);
						newitem->type = mapred_malloc(strlen(type)+1);
						strncpy(newitem->type, type, strlen(type)+1);
						newitem->next = obj->u.input.columns;
						obj->u.input.columns = newitem;
					}
				}
				else
				{
					XRAISE(MAPRED_SQL_ERROR, "Object creation Failure");
				}
			}
			break;


		case MAPRED_OUTPUT:
			break;

		case MAPRED_MAPPER:
		case MAPRED_TRANSITION:
		case MAPRED_COMBINER:
		case MAPRED_FINALIZER:
			XASSERT(obj->u.function.parameters);
			XASSERT(obj->u.function.returns);
			break;


		case MAPRED_REDUCER:
		{
			mapred_object_t *transition = obj->u.reducer.transition.object;

			if (transition && transition->u.function.parameters)
			{
				obj->u.reducer.parameters = 
					transition->u.function.parameters->next;
			}
			else
			{
				fprintf(stderr, "FIXME - bad reducer parameters\n");
			}

			/* 
			 * Use the return result of:
			 *   1) The finalizer
			 *   2) The combiner, or
			 *   3) The transition
			 *
			 * in that order, if the return is not derivable then
			 * fall into the default value of a single text column
			 * named "value"
			 */
			if (obj->u.reducer.finalizer.name)
				sub = obj->u.reducer.finalizer.object;
			else if (obj->u.reducer.combiner.name)
				sub = obj->u.reducer.combiner.object;
			else
				sub = obj->u.reducer.transition.object;					 
					
			if (sub)
				obj->u.reducer.returns = sub->u.function.returns;
			else
			{
				/* 
				 * If unable to determine the returns based on the reducer 
				 * components (generally due to use of SQL functions) then 
				 * use the default of a single text column named "value".
				 */
				obj->u.reducer.returns = mapred_malloc(sizeof(mapred_plist_t));
				obj->u.reducer.returns->name = "value";
				obj->u.reducer.returns->type = "text";
				obj->u.reducer.returns->next = NULL;
			}
			break;
		}


		case MAPRED_TASK:
		case MAPRED_EXECUTION:
		{
			/* 
			 * The input must either be an INPUT or a TASK 
			 */
			sub = obj->u.task.input.object;
			switch (sub->kind)
			{
				case MAPRED_INPUT:
					obj->u.task.parameters = sub->u.input.columns;
					break;

				case MAPRED_TASK:
					obj->u.task.parameters = sub->u.task.returns;
					break;
							
				default:
					XASSERT(false);  /* Should have already been validated */
			}
			
			if (obj->u.task.mapper.name)
			{
				sub = obj->u.task.mapper.object;				
				if (!sub)
				{
					/* FIXME: Lookup function in database */
					/* for now... do nothing */
				}
				else
				{
					/* Allow any function type */
					switch (sub->kind)
					{
						case MAPRED_MAPPER:
						case MAPRED_TRANSITION:
						case MAPRED_COMBINER:
						case MAPRED_FINALIZER:
							break;

						default:
							XASSERT(false); /* Should have already been validated */
					}
				}
			}

			if (obj->u.task.reducer.name)
			{
				mapred_plist_t *source;
				mapred_plist_t *scan;
				mapred_plist_t *last = NULL;

				/* 
				 * The grouping columns for a task are the columns produced
				 * by the input/mapper that are not consumed by the reducer.
				 * 
				 * A special exception is made for a column named "key" which
				 * is always a grouping column. 
				 *
				 * FIXME: deal with non-yaml map functions
				 *
				 * FIXME: deal with KEY specifications
				 */
				if (obj->u.task.mapper.object)
					source = obj->u.task.mapper.object->u.function.returns;
				else
					source = obj->u.task.parameters;

				sub = obj->u.task.reducer.object;				
				if (!sub)
				{
					/*
					 * The output of a built in function is defined to be 
					 * "value", with an input of "value", everything else
					 * is defined to be a grouping column.
					 */
					for (scan = source; scan; scan = scan->next)
					{
						if (strcasecmp(scan->name, "value"))
						{
							if (!last)
							{
								obj->u.task.grouping = mapred_malloc(sizeof(mapred_plist_t));
								last = obj->u.task.grouping;
							}
							else
							{
								last->next = mapred_malloc(sizeof(mapred_plist_t));
								last = last->next;
							}
							last->name = scan->name;
							last->type = scan->type;
							last->next = NULL;
						}
					}
				}
				else
				{
					/* Validate Reducer */					
					XASSERT(sub->kind == MAPRED_REDUCER);
					while (source)
					{
						for (scan = sub->u.reducer.parameters; scan; scan = scan->next)
						{
							if (!strcasecmp(source->name, scan->name))
								break;
						}
						if (!scan || !strcasecmp(source->name, "key"))
						{
							if (!last)
							{
								obj->u.task.grouping = mapred_malloc(sizeof(mapred_plist_t));
								last = obj->u.task.grouping;
							}
							else
							{
								last->next = mapred_malloc(sizeof(mapred_plist_t));
								last = last->next;
							}
							last->name = source->name;
							last->type = source->type;
							last->next = NULL;
						}
						source = source->next;
					}
				}
			}
			
			/*
			 * If there is a reducer then the "returns" columns are the 
			 * output of the reducer, and must be unioned with the grouping 
			 * columns for final output.
			 *
			 * If there is no reducer then the returns columns are the
			 * returns columns of the mapper or the input
			 */
			if (obj->u.task.reducer.name)
			{
				/*
				 * If it is a built in function then we'll just fall into the
				 * default of a single text column named "value". 
				 */
				sub = obj->u.task.reducer.object;
				if (sub)
					obj->u.task.returns = sub->u.reducer.returns;

			}
			else if (obj->u.task.mapper.name)
			{
				sub = obj->u.task.mapper.object;
				if (sub)
					obj->u.task.returns = sub->u.function.returns;
			}
			else
			{
				obj->u.task.returns = obj->u.task.parameters;
			}

			if (!obj->u.task.returns)
			{
				/* 
				 * If unable to determine the returns based on the reducer 
				 * components (generally due to use of SQL functions) then 
				 * use the default of a single text column named "value".
				 */
				obj->u.task.returns = mapred_malloc(sizeof(mapred_plist_t));
				obj->u.task.returns->name = "value";
				obj->u.task.returns->type = "text";
				obj->u.task.returns->next = NULL;
			}
			break;
		}

		default:
			XASSERT(false);
	}
}

void mapred_resolve_ref(mapred_olist_t *olist, mapred_reference_t *ref)
{
	XASSERT(ref);
	if (!ref->name)
		return;

	/* Scan the list of objects until we find one with a matching name */
	for (; olist; olist = olist->next)
	{
		if (olist->object->name && !strcasecmp(ref->name, olist->object->name))
		{
			ref->object = olist->object;
			return;
		}
	}
}

/* Some basic destructors */
void mapred_destroy_object(mapred_object_t **objh)
{
	mapred_object_t *obj;

	/* 
	 * We are passed a handle to the object, get the actual pointer and point
	 * the handle to NULL so that it is not stale once we free the list below.
	 */
	if (!objh || !*objh)
		return;
	obj = *objh;
	*objh = (mapred_object_t *) NULL;
	
	/* What fields are valid is dependent on what kind of object it is */
	scalarfree(obj->name);
	switch (obj->kind)
	{
		case MAPRED_NO_KIND:
			break;

		case MAPRED_DOCUMENT:
			scalarfree(obj->u.document.version);
			scalarfree(obj->u.document.database);
			scalarfree(obj->u.document.user);
			scalarfree(obj->u.document.host);
			mapred_destroy_olist(&obj->u.document.objects);
			mapred_destroy_olist(&obj->u.document.execute);
			break;

		case MAPRED_INPUT:
			scalarfree(obj->u.input.desc);
			scalarfree(obj->u.input.delimiter);
			scalarfree(obj->u.input.encoding);
			mapred_destroy_clist(&obj->u.input.files);
			mapred_destroy_plist(&obj->u.input.columns);
			break;

		case MAPRED_OUTPUT:
			scalarfree(obj->u.output.desc);
			break;

		case MAPRED_MAPPER:
		case MAPRED_TRANSITION:
		case MAPRED_COMBINER:
		case MAPRED_FINALIZER:
			scalarfree(obj->u.function.body);
			scalarfree(obj->u.function.language);
			mapred_destroy_plist(&obj->u.function.parameters);
			mapred_destroy_plist(&obj->u.function.returns);
			break;

		case MAPRED_REDUCER:
			scalarfree(obj->u.reducer.transition.name);
			scalarfree(obj->u.reducer.combiner.name);
			scalarfree(obj->u.reducer.finalizer.name);
			scalarfree(obj->u.reducer.initialize);
			break;

		case MAPRED_TASK:
		case MAPRED_EXECUTION:
			scalarfree(obj->u.task.input.name);
			scalarfree(obj->u.task.mapper.name);
			scalarfree(obj->u.task.reducer.name);
			scalarfree(obj->u.task.output.name);
			break;

		/*
		 * ADT just borrowed the parameter list from the owning function,
		 * so it has nothing else to delete.
		 */
		case MAPRED_ADT:
			break;

		default:
			XASSERT(false);
	}
}

void mapred_destroy_olist(mapred_olist_t **olisth)
{
	mapred_olist_t *olist;
	mapred_olist_t *next;

	/* 
	 * We are passed a handle to the olist, get the actual pointer and point
	 * the handle to NULL so that it is not stale once we free the list below.
	 */
	if (!olisth || !*olisth)
		return;
	olist = *olisth;
	*olisth = (mapred_olist_t *) NULL;

	/* Walk the list destroying each item as we come to it. */
	while (olist)
	{
		mapred_destroy_object(&olist->object);
		next = olist->next;
		mapred_free(olist);
		olist = next;
	}
}


void mapred_destroy_clist(mapred_clist_t **clisth)
{
	mapred_clist_t *clist;
	mapred_clist_t *next;

	/* 
	 * We are passed a handle to the olist, get the actual pointer and point
	 * the handle to NULL so that it is not stale once we free the list below.
	 */
	if (!clisth || !*clisth)
		return;
	clist = *clisth;
	*clisth = (mapred_clist_t *) NULL;

	/* Walk the list destroying each item as we come to it. */
	while (clist)
	{
		scalarfree(clist->value);
		next = clist->next;
		mapred_free(clist);
		clist = next;
	}
}

void mapred_destroy_plist(mapred_plist_t **plisth)
{
	mapred_plist_t *plist;
	mapred_plist_t *next;

	/* 
	 * We are passed a handle to the olist, get the actual pointer and point
	 * the handle to NULL so that it is not stale once we free the list below.
	 */
	if (!plisth || !*plisth)
		return;
	plist = *plisth;
	*plisth = (mapred_plist_t *) NULL;

	/* Walk the list destroying each item as we come to it. */
	while (plist)
	{
		scalarfree(plist->name);
		scalarfree(plist->type);
		next = plist->next;
		mapred_free(plist);
		plist = next;
	}
}




/* -------------------------------------------------------------------------- */
/* Functions that get things done
/* -------------------------------------------------------------------------- */
void mapred_run_queries(PGconn *conn, mapred_document_t *doc)
{
	PGresult        *result;
	mapred_olist_t  *olist;
	mapred_plist_t  *columns;
	mapred_object_t *output;
	buffer_t        *buffer = NULL;

	XTRY 
	{
		/* allocates 512 bytes, extending by 512 bytes if we run out. */
		buffer = makebuffer(512, 512);
		
		/* Loop through all objects */
		for (olist = doc->objects; olist; olist = olist->next)
		{
			if (olist->object->kind == MAPRED_EXECUTION)
			{
				XASSERT(olist->object->name);

				/* Reset the buffer from any previous executions */
				bufreset(buffer);
				
				output = olist->object->u.task.output.object;

				/*
				 *  [CREATE TABLE <name> AS ]
				 *    SELECT * FROM <name>
				 *    ORDER BY <column-list>
				 */
				if (output && output->u.output.type == MAPRED_OUTPUT_TABLE)
				{
					bufcat(&buffer, "CREATE TABLE");
					bufcat(&buffer, output->u.output.desc);
					bufcat(&buffer, " AS ");
				}
				bufcat(&buffer, "SELECT * FROM ");
				bufcat(&buffer, olist->object->name);

				/* Add the ORDER BY CLAUSE */
				if (!output || output->u.output.type != MAPRED_OUTPUT_TABLE)
				{
					if (olist->object->u.task.returns ||
						olist->object->u.task.grouping)
					{
						bufcat(&buffer, " ORDER BY ");
						columns = olist->object->u.task.grouping;
						while (columns)
						{
							bufcat(&buffer, columns->name);
							if (columns->next || olist->object->u.task.returns)
								bufcat(&buffer, ", ");
							columns = columns->next;				   
						}
						columns = olist->object->u.task.returns;
						while (columns)
						{
							bufcat(&buffer, columns->name);
							if (columns->next)
								bufcat(&buffer, ", ");
							columns = columns->next;				   
						}
					}
				}
				bufcat(&buffer, ";\n");

				/* Tell the user what job we are running */
				if (global_verbose_flag)
					fprintf(stderr, "  - RUN: ");
				if (global_print_flag || global_debug_flag)
					fprintf(stderr, buffer->buffer);
				else
					fprintf(stderr, "%s\n", olist->object->name);

				/* But we only execute it if we are not in "print-only" mode */
				if (!global_print_flag)
				{
					FILE *outfile;

					/* If we have an output file, open it for write now */
					if (output && output->u.output.type == MAPRED_OUTPUT_FILE)
					{
						outfile = fopen(output->u.output.desc, "wb");
						if (!outfile)
						{
							fprintf(stderr, "Error (%d): could not open file '%s' for write.\n", 
									errno, output->u.output.desc);
							continue;
						}
					}
					else
					{
						outfile = stdout;
					}

					/* 
					 * Enable notices for user queries since they may contain 
					 * debugging info. 
					 */
					PQsetNoticeReceiver(conn, print_notice_handler, NULL);
					result = PQexec(conn, buffer->buffer);
					PQsetNoticeReceiver(conn, ignore_notice_handler, NULL);
					switch (PQresultStatus(result))
					{
						/* Output is STDOUT or FILE */
						case  PGRES_TUPLES_OK:
						{
							PQprintOpt options;
							memset(&options, 0, sizeof(options));

							/* Fancy formating for stdout, plain for flat file */
							if (outfile == stdout)
							{
								options.header = true;
								options.align  = true;
								options.fieldSep  = "|";
							}
							else if (output->u.output.delimiter)
							{
								options.fieldSep = output->u.output.delimiter;
							}
							else
							{
								/* is "|" the best default delimiter? */
								options.fieldSep  = "|";
							}
						
							PQprint(outfile, result, &options);
							break;
						}
						
						/* OUTPUT is a table */
						case PGRES_COMMAND_OK:
							fprintf(stderr, "CREATED\n");
							break;

						/* An error of some kind */
						default:
							fprintf(stderr, PQerrorMessage(conn));
					}
					PQclear(result);
					
					if (outfile != stdout)
						fclose(outfile);
				}
			}
		}
	}
	XFINALLY
	{
		if (buffer)
			mapred_free(buffer);
	}
	XTRY_END;
}




boolean mapred_create_object(PGconn *conn, mapred_object_t *obj)
{
	mapred_clist_t *clist    = NULL;
	mapred_plist_t *plist    = NULL;
	const char     *ckind    = NULL;
	buffer_t       *buffer   = NULL;
	buffer_t       *qbuffer  = NULL;
	PGresult       *result   = NULL;

	/* If the object was created in a prior pass, then do nothing */
	if (obj->created)
		return true;

	/* Otherwise attempt to create the object */
	XTRY
	{
		/* allocates 1024 bytes, extending by 1024 bytes if we run out */
		buffer = makebuffer(1024, 1024);

		switch (obj->kind)
		{
			case MAPRED_INPUT:
				XASSERT(obj->name);

				switch (obj->u.input.type)
				{
					case MAPRED_INPUT_TABLE:
						/* Nothing to actually create */
						obj->created = true;
						break;  

					case MAPRED_INPUT_FILE:
					case MAPRED_INPUT_GPFDIST:
						XASSERT(obj->u.input.files);

						/* Allocate and produce buffer */
						bufcat(&buffer, "CREATE EXTERNAL TABLE ");
						bufcat(&buffer, obj->name);
						bufcat(&buffer, "(");
						for (plist = obj->u.input.columns; plist; plist = plist->next)
						{
							bufcat(&buffer, plist->name);
							bufcat(&buffer, " ");
							bufcat(&buffer, plist->type);
							if (plist->next)
								bufcat(&buffer, ", ");
						}
						bufcat(&buffer, ")\n");
						bufcat(&buffer, "  LOCATION(");
						for (clist = obj->u.input.files; clist; clist = clist->next)
						{
							char *host, *path, *tokenizer;

							if (obj->u.input.type == MAPRED_INPUT_GPFDIST)
								bufcat(&buffer, "'gpfdist://");
							else
								bufcat(&buffer, "'file://");

							/* should be split by ':' */
							host = strtok_r(clist->value, ":", &tokenizer);
							path = strtok_r(NULL, ":", &tokenizer);
							if (!path)
								XRAISE(MAPRED_PARSE_ERROR,
									   "Invalid INPUT source specification");

							bufcat(&buffer, host);
							bufcat(&buffer, path);
							if (clist->next)
								bufcat(&buffer, "',\n           ");
						}
						bufcat(&buffer, "')\n");
						if (obj->u.input.format == MAPRED_FORMAT_CSV)
							bufcat(&buffer, "  FORMAT 'CSV'");
						else
							bufcat(&buffer, "  FORMAT 'TEXT'");
						if (obj->u.input.delimiter || obj->u.input.quote || obj->u.input.null)
						{
							bufcat(&buffer, " ( ");
							if (obj->u.input.delimiter)
							{
								bufcat(&buffer, "DELIMITER '");
								bufcat(&buffer, obj->u.input.delimiter);
								bufcat(&buffer, "' ");
							}
							if (obj->u.input.quote)
							{
								bufcat(&buffer, "QUOTE '");
								bufcat(&buffer, obj->u.input.quote);
								bufcat(&buffer, "' ");
							}
							if (obj->u.input.null)
							{
								bufcat(&buffer, "NULL '");
								bufcat(&buffer, obj->u.input.null);
								bufcat(&buffer, "' ");
							}
							bufcat(&buffer, ")");
						}
						
						if (obj->u.input.error_limit > 0)
						{
							char intbuf[11];
							snprintf(intbuf, 11, "%d", obj->u.input.error_limit);

							bufcat(&buffer, "\n  SEGMENT REJECT LIMIT ");
							bufcat(&buffer, intbuf);
						}

						bufcat(&buffer, ";\n\n");
						break;

					case MAPRED_INPUT_EXEC:
						XASSERT(obj->u.input.desc);

						bufcat(&buffer, "CREATE EXTERNAL WEB TABLE ");
						bufcat(&buffer, obj->name);
						bufcat(&buffer, "(");
						for (plist = obj->u.input.columns; plist; plist = plist->next)
						{
							bufcat(&buffer, plist->name);
							bufcat(&buffer, " ");
							bufcat(&buffer, plist->type);
							if (plist->next)
								bufcat(&buffer, ", ");
						}
						bufcat(&buffer, ")\n");
						bufcat(&buffer, "EXECUTE '");
						bufcat(&buffer, obj->u.input.desc);
						bufcat(&buffer, "'\n");
						if (obj->u.input.format == MAPRED_FORMAT_CSV)
							bufcat(&buffer, "  FORMAT 'CSV'");
						else
							bufcat(&buffer, "  FORMAT 'TEXT'");
						if (obj->u.input.delimiter || obj->u.input.quote || obj->u.input.null)
						{
							bufcat(&buffer, " ( ");
							if (obj->u.input.delimiter)
							{
								bufcat(&buffer, "DELIMITER '");
								bufcat(&buffer, obj->u.input.delimiter);
								bufcat(&buffer, "' ");
							}
							if (obj->u.input.quote)
							{
								bufcat(&buffer, "QUOTE '");
								bufcat(&buffer, obj->u.input.quote);
								bufcat(&buffer, "' ");
							}
							if (obj->u.input.null)
							{
								bufcat(&buffer, "NULL '");
								bufcat(&buffer, obj->u.input.null);
								bufcat(&buffer, "' ");
							}
							bufcat(&buffer, ")");
						}

						if (obj->u.input.error_limit > 0)
						{
							char intbuf[11];
							snprintf(intbuf, 11, "%d", obj->u.input.error_limit);

							bufcat(&buffer, "\n  SEGMENT REJECT LIMIT ");
							bufcat(&buffer, intbuf);
						}
						bufcat(&buffer, ";\n\n");
						break;

					case MAPRED_INPUT_QUERY:
						XASSERT(obj->u.input.desc);

						/* 
						 *  CREATE VIEW <name> AS 
						 *  <desc>;
						 */
						bufcat(&buffer, "CREATE VIEW ");
						bufcat(&buffer, obj->name);
						bufcat(&buffer, " AS\n");
						bufcat(&buffer, obj->u.input.desc);
						bufcat(&buffer, ";\n\n");
						break;

					case MAPRED_INPUT_NONE:
					default:
						XASSERT(false);
				}
				if (global_print_flag || global_debug_flag)
					printf("-- INPUT %s\n", obj->name);
				break;

			case MAPRED_OUTPUT:
				/* 
				 * Outputs have no backend objects created directly.
				 * For output tables we may issue a create table as
				 * select, but that occurs at run-time.
				 */
				obj->created = true;
				mapred_setup_columns(conn, obj);
				break;

				/*
				 * The function types have different defaults and generate
				 * slightly different error messages, but basically do the
				 * same thing.  
				 */
			case MAPRED_MAPPER:
			case MAPRED_TRANSITION:
			case MAPRED_COMBINER:
			case MAPRED_FINALIZER:
				ckind = mapred_kind_name[obj->kind];

				/*
				 * 'kind' specific initialization accomplished above, now handle
				 * the generic function creation.
				 */
				if (global_print_flag || global_debug_flag)
					printf("-- %s %s\n", ckind, obj->name);
			
				/* We should have already been verified */
				XASSERT(obj->name);
				XASSERT(obj->u.function.body);
				XASSERT(obj->u.function.language);

				mapred_setup_columns(conn, obj);
				XASSERT(obj->u.function.parameters);
				XASSERT(obj->u.function.rtype.name);

				/* 
				 * fill in the buffer:
				 *
				 *    CREATE FUNCTION <name>(<parameters>)
				 *    RETURNS [SETOF] <rtype> LANGUAGE <lang> AS
				 *    $$
				 *    <body>
				 *    $$ [STRICT] [IMMUTABLE];
				 *     
				 */
				bufcat(&buffer, "CREATE FUNCTION ");
				bufcat(&buffer, obj->name);
				bufcat(&buffer, "(");

				/* Handle parameter list */
				for (plist = obj->u.function.parameters; plist; plist = plist->next)
				{
					bufcat(&buffer, plist->name);
					bufcat(&buffer, " ");
					bufcat(&buffer, plist->type);
					if (plist->next)
						bufcat(&buffer, ", ");
				}

				/* Handle Return clause */
				bufcat(&buffer, ")\nRETURNS ");
				if (obj->u.function.mode == MAPRED_MULTI_MODE)
					bufcat(&buffer, "SETOF ");
				bufcat(&buffer, obj->u.function.rtype.name);

				/* 
				 * Handle LANGUAGE clause, every langauge but 'C' and 'SQL'
				 * has 'pl' prefixing it
				 */
				if (!strcasecmp("C", obj->u.function.language) ||
					!strcasecmp("SQL", obj->u.function.language) ||
					!strncasecmp("PL", obj->u.function.language, 2))
				{
					bufcat(&buffer, " LANGUAGE ");
					bufcat(&buffer, obj->u.function.language);
				}
				else
				{
					bufcat(&buffer, " LANGUAGE pl");
					bufcat(&buffer, obj->u.function.language);
				}
			
				/* python only has an untrusted form */
				if (!strcasecmp("python", obj->u.function.language))
					bufcat(&buffer, "u");


				/* Handle the body of the function */
				bufcat(&buffer, " AS\n$$\n");
				bufcat(&buffer, obj->u.function.body);
				if (buffer->buffer[buffer->position-1] != '\n')
					bufcat(&buffer, "\n$$");
				else
					bufcat(&buffer, "$$");

				/* Handle options */
				if (obj->u.function.flags & mapred_function_strict)
					bufcat(&buffer, " STRICT");
				if (obj->u.function.flags & mapred_function_immutable)
					bufcat(&buffer, " IMMUTABLE");
				
				/* All done */
				bufcat(&buffer, ";\n\n");
				break;
			

			case MAPRED_REDUCER:
			{
				mapred_object_t *transition = obj->u.reducer.transition.object;
				char *state;

				XASSERT(obj->name);
				XASSERT(transition);
				XASSERT(transition->name);
			
				/* 
				 * If the reducer depends on an object that hasn't be created then
				 * return false, it will be resolved during a second pass
				 */
				if ((obj->u.reducer.transition.object && 
					 !obj->u.reducer.transition.object->created) ||
					(obj->u.reducer.combiner.object && 
					 !obj->u.reducer.combiner.object->created) ||
					(obj->u.reducer.finalizer.object && 
					 !obj->u.reducer.finalizer.object->created))
				{
					if (global_print_flag && global_debug_flag)
						printf("-- defering REDUCER %s\n", obj->name);
					break;
				}
				if (global_print_flag || global_debug_flag)
					printf("-- REDUCER %s\n", obj->name);

				/* Now, set things up to create the thing */
				mapred_setup_columns(conn, obj);
				plist = transition->u.function.parameters;
				XASSERT(plist);          /* state */
				XASSERT(plist->next);    /* parameters */


				bufcat(&buffer, "CREATE AGGREGATE ");
				bufcat(&buffer, obj->name);
				bufcat(&buffer, " (");

				/* 
				 * Get the state type, and write out the aggregate parameters based
				 * on the parameter list of the transition function.
				 */
				plist = transition->u.function.parameters;
				state = plist->type;
				for (plist = plist->next; plist; plist = plist->next)
				{
					bufcat(&buffer, plist->type);
					if (plist->next)
						bufcat(&buffer, ", ");
				}
				bufcat(&buffer, ") (\n");
				bufcat(&buffer, "  stype = ");
				bufcat(&buffer, state);
				if (obj->u.reducer.initialize)
				{
					bufcat(&buffer, ",\n  initcond = '");
					bufcat(&buffer, obj->u.reducer.initialize);
					bufcat(&buffer, "'");
				}
				bufcat(&buffer, ",\n  sfunc = ");
				bufcat(&buffer, transition->name);
				if (obj->u.reducer.combiner.name)
				{
					bufcat(&buffer, ",\n  prefunc = ");
					bufcat(&buffer, obj->u.reducer.combiner.name);
				}

				/*
				 * To handle set returning finalizers the finalizer is pushed
				 * into the task definition rather than being placed in the
				 * uda where it belongs.
				 */
				/*
				 if (obj->u.reducer.finalizer.name)
				 {
				    bufcat(&buffer, ",\n  finalfunc = ");
				    bufcat(&buffer, obj->u.reducer.finalizer.name);
				 }
				*/

				bufcat(&buffer, "\n);\n\n");
				break;
			}

			case MAPRED_TASK:
			case MAPRED_EXECUTION:
			{
				mapred_object_t *input    = obj->u.task.input.object;
				mapred_object_t *mapper   = obj->u.task.mapper.object;
				mapred_object_t *reducer  = obj->u.task.reducer.object;
				mapred_object_t *output   = obj->u.task.output.object;
				mapred_plist_t  *columns  = NULL;
				mapred_plist_t  *ingrouping = NULL;
				mapred_plist_t  *newitem  = NULL;
				mapred_plist_t  *grouping = NULL;
				mapred_plist_t  *last     = NULL;
				mapred_plist_t  *scan     = NULL;
				buffer_t        *swap;

				if (!obj->u.task.execute)
					XASSERT(obj->name);
				XASSERT(obj->u.task.input.name);

				if (!qbuffer)
					qbuffer = makebuffer(1024, 1024);
				else
					bufreset(qbuffer);
				
				/* 
				 * If the task depends on an object that hasn't be created then
				 * return false, it will be resolved during a second pass
				 */
				if ((obj->u.task.input.object && 
					 !obj->u.task.input.object->created) ||
					(obj->u.task.mapper.object && 
					 !obj->u.task.mapper.object->created) ||
					(obj->u.task.reducer.object && 
					 !obj->u.task.reducer.object->created))
				{
					if (global_print_flag && global_debug_flag)
					{
						if (obj->u.task.execute)
							printf("-- defering EXECUTION\n");
						else
							printf("-- defering TASK %s\n", obj->name);
					}
					break;
				}

				if (global_print_flag || global_debug_flag)
				{
					if (obj->u.task.execute)
						printf("-- EXECUTION\n");
					else
						printf("-- TASK %s\n", obj->name);
				}

				/* 
				 * 1) Handle the INPUT, two cases:
				 *   1a) There is no MAPPER/REDUCER:  "SELECT * FROM <input>"
				 *   1b) There is a MAPPER and/or REDUCER:  "<input>"
				 */
				mapred_setup_columns(conn, obj);
				if (!obj->u.task.mapper.name && !obj->u.task.reducer.name)
				{
					/* Allocate the buffer for the input. */
					if (input->u.input.type == MAPRED_INPUT_TABLE)
					{
						bufcat(&qbuffer, "SELECT * FROM ");
						bufcat(&qbuffer, input->u.input.desc);
					}
					else
					{
						bufcat(&qbuffer, "SELECT * FROM ");
						bufcat(&qbuffer, input->name);
					}
				}
				else
				{
					/* Input is just the name or description of the input */
					if (input->u.input.type == MAPRED_INPUT_TABLE)
						bufcat(&qbuffer, input->u.input.desc);
					else
						bufcat(&qbuffer, input->name);
				}

				/* 
				 * How we get the columns depends a bit on the input.
				 * Is the input actually an "MAPRED_INPUT" object, or is it
				 * a "MAPRED_TASK" object?
				 */
				switch (input->kind)
				{
					case MAPRED_INPUT:
						columns = input->u.input.columns;
						break;

					case MAPRED_TASK:
						columns    = input->u.task.returns;
						ingrouping = input->u.task.grouping;
					
						if (!columns)
						{
							fprintf(stderr, 
									"Error: Unable to determine return columns for TASK '%s'\n",
									obj->u.task.input.name);
							XRAISE(MAPRED_PARSE_INTERNAL, NULL);
						}
						break;

					default:
						XRAISE(MAPRED_PARSE_ERROR,
							   "Bad SOURCE object, not an INPUT");
						break;
				}
				XASSERT(columns);
				
				/*
				 * 2) Handle the MAPPER, two cases
				 *  2a) The Mapper returns an generated ADT that needs extraction
				 *           "SELECT key(m), ... 
				 *            FROM (SELECT <map(...) as m FROM <input>) mapsubq
				 *  2b) The Mapper returns a single column:
				 *           "SELECT <map>(...) FROM <input>"
				 */
				if (obj->u.task.mapper.name)
				{
					XASSERT(mapper);  /* FIXME */


					plist = mapper->u.function.returns;
					XASSERT(plist);

					if (plist->next)
					{ /* 2a */
						bufcat(&buffer, "SELECT ");
						for (; plist; plist = plist->next)
						{
							bufcat(&buffer, plist->name);
							bufcat(&buffer, "(m)");
							if (plist->next)
								bufcat(&buffer, ", ");
						}
						bufcat(&buffer, " FROM (");
					}

					/* shared code */
					bufcat(&buffer, "SELECT ");
					bufcat(&buffer, mapper->name);
					bufcat(&buffer, "(");
					plist = mapper->u.function.parameters;
					for (; plist; plist = plist->next)
					{
						/* Check if this parameter is one of the input columns */
						for (scan = columns; scan; scan = scan->next)
							if (!strcasecmp(plist->name, scan->name))
							{
								bufcat(&buffer, plist->name);
								break;
							}

						/* Task inputs also need to scan the grouping columns */
						if (!scan)
							for (scan = ingrouping; scan; scan = scan->next)
								if (!strcasecmp(plist->name, scan->name))
								{
									bufcat(&buffer, plist->name);
									break;
								}
					
						/* Check if this parameter is in global_plist */
						if (!scan)
							for (scan = global_plist; scan; scan = scan->next)
								if (!strcasecmp(plist->name, scan->name))
								{
									/*
									 * (HACK)
									 * Note that global_plist overloads the plist 
									 * structure using the "type" field to store 
									 * "value".
									 * (HACK)
									 */
									bufcat(&buffer, "'");
									bufcat(&buffer, scan->type);
									bufcat(&buffer, "'::");
									bufcat(&buffer, plist->type);
									break;
								}


						/* If we couldn't find it issue a warning and set to NULL */
						if (!scan)
						{
							if (global_verbose_flag)
								fprintf(stderr, "       ");
							fprintf(stderr, 
									"WARNING: unset parameter - %s(%s => NULL)\n",
									mapper->name, plist->name);
							bufcat(&buffer, "NULL::");
							bufcat(&buffer, plist->type);
						}

						/* Add a comma if there is another parameter */
						if (plist->next)
							bufcat(&buffer, ", ");
					}
					bufcat(&buffer, ") as ");

					/* break into cases again */
					plist = mapper->u.function.returns;
					if (plist->next)
					{  /* 2a */
						bufcat(&buffer, "m FROM ");
						bufcat(&buffer, qbuffer->buffer);
						bufcat(&buffer, ") mapxq\n");
						/*
						 * Need to work this through, it seems that m is true whenever a 
						 * column is null, which is not the desired behavior.
						 *
						 * Look more closely at "grep" code for why we want it, and
						 * "oreilly" code for why we don't.
						 *
						 * For the moment the compromise is that we do it only for
						 * SINGLE mode functions, since MULTI mode can control it's
						 * own filtering without this.
						 */
						if (mapper->u.function.mode != MAPRED_MULTI_MODE)
							bufcat(&buffer, "WHERE m is not null");   
					}
					else
					{
						bufcat(&buffer, plist->name);
						bufcat(&buffer, " FROM ");
						bufcat(&buffer, qbuffer->buffer);
					}

					/* 
					 * Swap the buffer into the qbuffer for input as the next stage
					 * of the query pipeline. 
					 */
					swap = qbuffer;
					qbuffer = buffer;
					buffer = swap;
					bufreset(buffer);

					/* Columns are now the output of the mapper */
					columns  = mapper->u.function.returns;
					ingrouping = NULL;
				}
				
				/*
				 * 3) Handle the Reducer, several sub-cases:
				 */
				if (obj->u.task.reducer.name)
				{
					/*
					 * Step 1:   Determine grouping columns
					 *    Find which columns are returned from the previous
					 *    stage that are NOT parameters to the reducer.  
					 */
					grouping = last = NULL;
					if (!reducer)
					{
						/* 
						 * We have a reducer, but it isn't listed in the YAML.
						 * How to work out parameter handling still needs to
						 * be worked out.  For now we just assume that this 
						 * sort of function always takes a single "value" column
						 * and returns a "value" column.
						 */
						for (plist = columns; plist; plist = plist->next)
						{
							if (strcasecmp(plist->name, "value"))
							{
								if (grouping)
								{
									last->next = mapred_malloc(sizeof(mapred_plist_t));
									last = last->next;
								}
								else
								{
									grouping = mapred_malloc(sizeof(mapred_plist_t));
									last = grouping;
								}
								last->name = plist->name;
								last->type = plist->type;
								last->next = NULL;
							}
						}
					} 
					else
					{
						/* The reducer exists in the YAML */
					
						/* 
						 * If the reducer has a finalizer we push the finalizer out
						 * to the task so that we can handle set returning aggregates.
						 */

						/* FIXME */

						/* We precalculated the grouping columns */
						grouping = obj->u.task.grouping;
					}
				
					/* Fill in the buffer */
					bufcat(&buffer, "SELECT ");
					for (plist = grouping; plist; plist = plist->next)
					{
						bufcat(&buffer, plist->name);
						bufcat(&buffer, ", ");
					}
				
					/* 
					 * Since "finalfunc" can't return a set we instead push the finalizer
					 * outside the scope of the reducer!
					 */
					if (reducer && reducer->u.reducer.finalizer.name)
					{
						bufcat(&buffer, reducer->u.reducer.finalizer.name);
						bufcat(&buffer, "(");
					}

					/* Call the aggregation function */
					bufcat(&buffer, obj->u.task.reducer.name);
					bufcat(&buffer, "(");

					if (reducer)
					{
						plist = reducer->u.reducer.parameters;
						for (; plist; plist = plist->next)
						{
							/* Check if this parameter is one of the input columns */
							for (scan = columns; scan; scan = scan->next)
								if (!strcasecmp(plist->name, scan->name))
								{
									bufcat(&buffer, plist->name);
									break;
								}

							/* Task inputs also need to scan the grouping columns */
							if (!scan)
								for (scan = ingrouping; scan; scan = scan->next)
									if (!strcasecmp(plist->name, scan->name))
									{
										bufcat(&buffer, plist->name);
										break;
									}
					
							/* Check if this parameter is in global_plist */
							if (!scan)
								for (scan = global_plist; scan; scan = scan->next)
									if (!strcasecmp(plist->name, scan->name))
									{
										/*
										 * (HACK)
										 * Note that global_plist overloads the plist 
										 * structure using the "type" field to store 
										 * "value".
										 * (HACK)
										 */
										bufcat(&buffer, "'");
										bufcat(&buffer, scan->type);
										bufcat(&buffer, "'::");
										bufcat(&buffer, plist->type);
										break;
									}


							/* If we couldn't find it issue a warning and set to NULL */
							if (!scan)
							{
								if (global_verbose_flag)
									fprintf(stderr, "       ");
								fprintf(stderr, 
										"WARNING: unset parameter - %s(%s => NULL)\n",
										reducer->name, plist->name);
								bufcat(&buffer, "NULL::");
								bufcat(&buffer, plist->type);
							}

							/* Write out the comma if there is another input parameter */
							if (plist->next)
								bufcat(&buffer, ", ");
						}
						if (reducer->u.reducer.finalizer.name)
							bufcat(&buffer, ")");
					}
					else
					{
						/* non-yaml reducer always takes "value" as it's input column */

						/* Check if "value" is one of the input columns */
						for (scan = columns; scan; scan = scan->next)
							if (!strcasecmp(scan->name, "value"))
							{
								bufcat(&buffer, "value");
								break;
							}

						/* Task inputs also need to scan the grouping columns */
						if (!scan)
							for (scan = ingrouping; scan; scan = scan->next)
								if (!strcasecmp(plist->name, scan->name))
								{
									bufcat(&buffer, "value");
									break;
								}
					
						/* Check if this parameter is in global_plist */
						if (!scan)
							for (scan = global_plist; scan; scan = scan->next)
								if (!strcasecmp(scan->name, "value"))
								{
									/*
									 * (HACK)
									 * Note that global_plist overloads the plist 
									 * structure using the "type" field to store 
									 * "value".
									 * (HACK)
									 */
									bufcat(&buffer, "'");
									bufcat(&buffer, scan->type);
									bufcat(&buffer, "'::");
									bufcat(&buffer, plist->type);
									break;
								}
						
						if (!scan)
						{
							if (global_verbose_flag)
								fprintf(stderr, "       ");
							fprintf(stderr, 
									"WARNING: unset parameter - %s(value => NULL)\n",
									obj->u.task.reducer.name);
							bufcat(&buffer, "NULL");
						}
					}

					bufcat(&buffer, ") as ");

					if (reducer)
					{
						plist = reducer->u.reducer.returns;
						if (!plist || plist->next)
							XASSERT(false); /* FIXME: multi-column returning reducers? */
						bufcat(&buffer, plist->name);
					}
					else
					{
						/* non-yaml reducer always returns a single column named "value" */
						bufcat(&buffer, "value");
					}
					bufcat(&buffer, "\nFROM ");
					if (obj->u.task.mapper.name)
					{
						bufcat(&buffer, "(");
						bufcat(&buffer, qbuffer->buffer);
						bufcat(&buffer, ") mapsubq");
					}
					else
					{
						bufcat(&buffer, qbuffer->buffer);
					}
					if (grouping)
					{
						bufcat(&buffer, "\nGROUP BY ");
						for (plist = grouping; plist; plist = plist->next)
						{
							bufcat(&buffer, plist->name);
							if (plist->next)
								bufcat(&buffer, ", ");
						}
					}


					/* 
					 * Swap the buffer into the qbuffer for input as the next stage
					 * of the query pipeline. 
					 */
					swap = qbuffer;
					qbuffer = buffer;
					buffer = swap;
					bufreset(buffer);


					/* 
					 * Add the return columns to the grouping columns and set
					 * it to the current columns.  
					 *
					 * Note that unlike the columns set by the mapper or the
					 * input this is a list that must be de-allocated.
					 */
					columns = mapred_malloc(sizeof(mapred_plist_t));
					columns->name = "value";  /* FIXME */
					columns->type = "text";   /* FIXME */
					columns->next = grouping;
					columns = last;

					/* 
					 * If the reducer had a finalizer we push it into another nested
					 * subquery since user defined aggregates aren't allowed to return
					 * sets.
					 *
					 * FIXME: this code is a duplicate of the MAP code above
					 */

				}


				/*
				 * 4) Handle the final transform into the view definition:
				 *        "CREATE VIEW . AS .;"
				 */
				bufcat(&buffer, "CREATE VIEW ");
				bufcat(&buffer, obj->name);
				bufcat(&buffer, " AS\n");
				bufcat(&buffer, qbuffer->buffer);
				bufcat(&buffer, ";\n\n");

				/* 
				 * If there was a reducer then we have to release the columns
				 * list, otherwise it is a pointer to an existing list and can
				 * be ignored.
				 */
				if (obj->u.task.reducer.name)
				{
					plist = columns;
					while (plist && plist != grouping)
					{
						newitem = plist;
						plist = plist->next;
						mapred_free(newitem);
					}
				}
				break;
			}

			case MAPRED_ADT:
				XASSERT(obj->name);
				mapred_setup_columns(conn, obj);

				bufcat(&buffer, "CREATE TYPE ");
				bufcat(&buffer, obj->name);
				bufcat(&buffer, " as (");
				for (plist = obj->u.adt.returns; plist; plist = plist->next)
				{
					bufcat(&buffer, plist->name);
					bufcat(&buffer, " ");
					bufcat(&buffer, plist->type);
					if (plist->next)
						bufcat(&buffer, ", ");
				}
				bufcat(&buffer, ");\n\n");
				break;

			default:
				XASSERT(false);
		}

		if (buffer->position > 0)
		{

			/* 
			 * In print-only mode we do everything but run the queries
			 * ie, we still create and destroy objects.
			 */
			if (global_print_flag || global_debug_flag)
				printf("%s", buffer->buffer);			

			/*
			 * Try to create the object, but failure should not terminate
			 * the transaction, so wrap it in a savepoint.
			 */
			PQexec(conn, "SAVEPOINT mapreduce_save");
			result = PQexec(conn, buffer->buffer);
			if (PQresultStatus(result) == PGRES_COMMAND_OK)
			{
				obj->created = true;
				PQexec(conn, "RELEASE SAVEPOINT mapreduce_save");
			}
			else
			{
				char *error = PQresultErrorField(result, PG_DIAG_SQLSTATE);
				
				if (global_verbose_flag)
					fprintf(stderr, "       %s", PQerrorMessage(conn));

				/* rollback to savepoint */
				PQexec(conn, "ROLLBACK TO SAVEPOINT mapreduce_save");
				PQexec(conn, "RELEASE SAVEPOINT mapreduce_save");

				/* 
				 * If we have an "object does not exist" error from a SQL input
				 * then it may just be a dependency issue, so we don't error
				 * right away.
				 */
				if (obj->kind != MAPRED_INPUT || 
					obj->u.input.type != MAPRED_INPUT_QUERY ||
					strcmp(error, OBJ_DOES_NOT_EXIST))
				{
					XRAISE(MAPRED_SQL_ERROR, "Object creation Failure");
				}
			}
		}

		/* 
		 * INPUTS setup columns AFTER creation.
		 * All other objects handle it above prior to creation.
		 */
		if (obj->kind == MAPRED_INPUT && obj->created)
			mapred_setup_columns(conn, obj);
	}
	XFINALLY
	{
		if (buffer)
			mapred_free(buffer);
		if (qbuffer)
			mapred_free(qbuffer);
	}
	XTRY_END;

	return obj->created;
}


void mapred_remove_object(PGconn *conn, mapred_object_t *obj)
{
	mapred_clist_t *clist    = NULL;
	mapred_plist_t *plist    = NULL;
	const char     *ckind    = NULL;
	buffer_t       *buffer   = NULL;


	/* If the object wasn't created, then do nothing */
//	if (!obj->created)
//		return;

	XTRY
	{
		buffer = makebuffer(100, 100);

		/* Otherwise attempt to create the object */
		switch (obj->kind)
		{
			case MAPRED_INPUT:
				XASSERT(obj->name);

				switch (obj->u.input.type)
				{
					case MAPRED_INPUT_TABLE:
						obj->created = false;
						break;  /* do nothing */

					case MAPRED_INPUT_FILE:
					case MAPRED_INPUT_GPFDIST:
					case MAPRED_INPUT_EXEC:
						bufcat(&buffer, "DROP EXTERNAL TABLE IF EXISTS ");
						bufcat(&buffer, obj->name);
						bufcat(&buffer, " CASCADE;\n");
						break;

					case MAPRED_INPUT_QUERY:
						bufcat(&buffer, "DROP VIEW IF EXISTS ");
						bufcat(&buffer, obj->name);
						bufcat(&buffer, " CASCADE;\n");					
						break;

					case MAPRED_INPUT_NONE:
					default:
						XASSERT(false);
				}
				break;

			case MAPRED_OUTPUT:
				/* nothing to do for outputs */
				obj->created = false;
				break;

				/*
				 * The function types have different defaults and generate
				 * slightly different error messages, but basically do the
				 * same thing.  
				 */
			case MAPRED_MAPPER:
			case MAPRED_TRANSITION:
			case MAPRED_COMBINER:
			case MAPRED_FINALIZER:
				ckind = mapred_kind_name[obj->kind];

				bufcat(&buffer, "DROP FUNCTION IF EXISTS ");
				bufcat(&buffer, obj->name);
				bufcat(&buffer, "(");
				for (plist = obj->u.function.parameters; plist; plist = plist->next)
				{  /* Handle parameter list */
					bufcat(&buffer, plist->type);
					if (plist->next)
						bufcat(&buffer, ", ");
				}
				bufcat(&buffer, ") CASCADE;\n");
				break;
			

			case MAPRED_REDUCER:
			{
				mapred_object_t  *transition = obj->u.reducer.transition.object;

				XASSERT(obj->name);
				XASSERT(transition);   /* is this a good assumption? */

				if (!transition->u.function.parameters)
					break;  /* FIXME */
				XASSERT(transition->u.function.parameters);

				bufcat(&buffer, "DROP AGGREGATE IF EXISTS ");
				bufcat(&buffer, obj->name);
				bufcat(&buffer, "(");

				/* 
				 * The first parameter of the transition function is the 'state' and is
				 * not listed as a parameter of the reducer, but all the rest of the
				 * parameters are 
				 */
				plist = transition->u.function.parameters;
				for (plist = plist->next; plist; plist = plist->next)
				{
					bufcat(&buffer, plist->type);
					if (plist->next)
						bufcat(&buffer, ", ");
				}
				bufcat(&buffer, ") CASCADE;\n");
				break;
			}

			case MAPRED_TASK:
			case MAPRED_EXECUTION:
				XASSERT(obj->name);

				bufcat(&buffer, "DROP VIEW IF EXISTS ");
				bufcat(&buffer, obj->name);
				bufcat(&buffer, " CASCADE;\n");
				break;

			case MAPRED_ADT:
				bufcat(&buffer, "DROP TYPE IF EXISTS ");
				bufcat(&buffer, obj->name);
				bufcat(&buffer, " CASCADE;\n");
				break;

			default:
				XASSERT(false);
		}

		if (buffer->position > 0)
		{
			PGresult   *result;

			/* 
			 * In print-only mode we do everything but run the queries
			 * ie, we still create and destroy objects.
			 */
			if (global_print_flag || global_debug_flag)
				printf("%s", buffer->buffer);

			/* Try to delete the object, but don't raise exception on error */
			result = PQexec(conn, buffer->buffer);
			if (PQresultStatus(result) == PGRES_COMMAND_OK)
				obj->created = false;
			else
			{
				char *error = PQresultErrorField(result, PG_DIAG_SQLSTATE);
				
				/* 
				 * Errors that we can expect/ignore:
				 *
				 *     IN_FAILED_SQL_TRANSACTION -
				 *        another error has occured and the transaction was 
				 *        aborted
				 *
				 */
				if (strcmp(error, IN_FAILED_SQL_TRANSACTION))
				{
					if (obj->name)
						fprintf(stderr, 
								"[WARNING] Error dropping '%s'\n", obj->name);
					else
						fprintf(stderr, 
								"[WARNING] Error dropping unnamed object\n");
					fprintf(stderr, PQerrorMessage(conn));
				}
			}
			PQclear(result);
		}
	}
	XFINALLY
	{
		if (buffer)
			mapred_free(buffer);
	}
	XTRY_END;
}
