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
	"",
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
 * Currently we just ignore all warnings, may eventually do something
 * smarter, but this is preferable to dumping them to libpq's default
 * of dumping them to stderr.
 */
void notice_handler(void *arg, const PGresult *res) 
{
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

	/* Ignore NOTICE messages from database */
	PQsetNoticeReceiver(conn, notice_handler, NULL);

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
		do
		{
			/* 
			 * Loop through the objects, creating each in turn.
			 * If an object has dependencies that have not been created yet
			 * it will return false and we will make additional passes through
			 * the object list 
			 */  
			done  = true;
			for (olist = doc->objects; olist; olist = olist->next)
				if (!mapred_create_object(conn, olist->object))
					done = false;
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
			/* 
			 * We need to construct the returned column list for TABLE/QUERY
			 * inputs.  
			 *
			 * This is defered until mapred_create_object so that we have the
			 * view created and can interogate the catalog.
			 */
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

				rtype = malloc(sizeof(mapred_object_t));
				memset(rtype, 0, sizeof(mapred_object_t));
				rtype->kind = MAPRED_ADT;
				len = strlen(obj->name) + 7;
				rtype->name = malloc(len);
				snprintf(rtype->name, len, "%s_rtype", obj->name);
				rtype->u.adt.returns = obj->u.function.returns;
				
				obj->u.function.rtype.name = rtype->name;
				obj->u.function.rtype.object = rtype;

				/* Add the ADT to the list of document objects */
				newlist = malloc(sizeof(mapred_olist_t));
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
				obj->u.reducer.returns = malloc(sizeof(mapred_plist_t));
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
				obj->name = malloc(len);
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
						obj->u.task.parameters = sub->u.input.columns;
						break;

					case MAPRED_TASK:
						/* This objects input is the sub objects output */
						mapred_resolve_object(conn, doc, sub, exec_count);
						obj->u.task.parameters = sub->u.task.returns;
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
				 */
				if (obj->u.task.mapper.object)
					source = obj->u.task.mapper.object->u.function.returns;
				else
					source = obj->u.task.parameters;


				mapred_resolve_ref(doc->objects, &obj->u.task.reducer);
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
								obj->u.task.grouping = malloc(sizeof(mapred_plist_t));
								last = obj->u.task.grouping;
							}
							else
							{
								last->next = malloc(sizeof(mapred_plist_t));
								last = last->next;
							}
							last->name = scan->name;
							last->type = scan->type;
							last->next = NULL;
							break;
						}
					}

				}
				else if (sub->kind == MAPRED_REDUCER)
				{
					/* Validate Reducer */
					mapred_resolve_object(conn, doc, sub, exec_count);
					
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
								obj->u.task.grouping = malloc(sizeof(mapred_plist_t));
								last = obj->u.task.grouping;
							}
							else
							{
								last->next = malloc(sizeof(mapred_plist_t));
								last = last->next;
							}
							last->name = source->name;
							last->type = source->type;
							last->next = NULL;
						}
						source = source->next;
					}
				}
				else 
				{ 
					/* It's an object, but not a REDUCER */
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
				obj->u.task.returns = malloc(sizeof(mapred_plist_t));
				obj->u.task.returns->name = "value";
				obj->u.task.returns->type = "text";
				obj->u.task.returns->next = NULL;
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
		free(olist);
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
		free(clist);
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
		free(plist);
		plist = next;
	}
}


void mapred_run_queries(PGconn *conn, mapred_document_t *doc)
{
	PGresult        *result;
	mapred_olist_t  *olist;
	mapred_plist_t  *columns;
	mapred_object_t *output;
	char            *buffer;
	int              position = 0;
	size_t           bufsize;

#define writebuffer(str)						\
	position += snprintf(buffer+position, bufsize - position, str)
	
	for (olist = doc->objects; olist; olist = olist->next)
	{
		if (olist->object->kind == MAPRED_EXECUTION)
		{
			XASSERT(olist->object->name);

			output = olist->object->u.task.output.object;

			/* "SELECT * FROM . ;\n" */
			bufsize = 18 + strlen(olist->object->name);
			if (output && output->u.output.type == MAPRED_OUTPUT_TABLE)
			{
				/* "CREATE TABLE . AS\n" */
				bufsize += 20 + strlen(output->u.output.desc);
			}
			else
			{
				/* "\nORDER BY ..." */
				bufsize += 10;
				columns = olist->object->u.task.grouping;
				while (columns)
				{
					bufsize += strlen(columns->name);
					if (columns->next || olist->object->u.task.returns)
						bufsize += 2;  /* ", " */
					columns = columns->next;				   
				}
				columns = olist->object->u.task.returns;
				while (columns)
				{
					bufsize += strlen(columns->name);
					if (columns->next)
						bufsize += 2;  /* ", " */
					columns = columns->next;				   
				}
			}

			buffer = malloc(bufsize);
			position = 0;
			if (output && output->u.output.type == MAPRED_OUTPUT_TABLE)
			{
				writebuffer("CREATE TABLE ");
				writebuffer(output->u.output.desc);
				writebuffer(" AS ");
			}
			writebuffer("SELECT * FROM ");
			writebuffer(olist->object->name);

			/* Add the ORDER BY CLAUSE */
			if (!output || output->u.output.type != MAPRED_OUTPUT_TABLE)
			{
				if (olist->object->u.task.returns ||
					olist->object->u.task.grouping)
				{
					writebuffer(" ORDER BY ");
					columns = olist->object->u.task.grouping;
					while (columns)
					{
						writebuffer(columns->name);
						if (columns->next || olist->object->u.task.returns)
							writebuffer(", ");
						columns = columns->next;				   
					}
					columns = olist->object->u.task.returns;
					while (columns)
					{
						writebuffer(columns->name);
						if (columns->next)
							writebuffer(", ");
						columns = columns->next;				   
					}
				}
			}
			writebuffer(";\n");

			/* Tell the user what job we are running */
			if (global_verbose_flag)
				fprintf(stderr, "  - RUN: ");
			if (global_print_flag)
				fprintf(stderr, buffer);
			else
				fprintf(stderr, "%s\n", olist->object->name);

			XASSERT(position < bufsize);

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
					outfile = stdout;

				result = PQexec(conn, buffer);
				switch (PQresultStatus(result))
				{
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
							options.fieldSep  = output->u.output.delimiter;
						}
						else
						{
							/* is "|" the best default delimiter? */
							options.fieldSep  = "|";
						}
						
						PQprint(outfile, result, &options);
						break;
					}
					case PGRES_COMMAND_OK:
						fprintf(stderr, "CREATED\n");
						break;

					default:
						fprintf(stderr, PQerrorMessage(conn));
				}
				PQclear(result);

				if (outfile != stdout)
					fclose(outfile);
				
			}
		}
	}

#undef writebuffer
}




boolean mapred_create_object(PGconn *conn, mapred_object_t *obj)
{
	mapred_clist_t *clist    = NULL;
	mapred_plist_t *plist    = NULL;
	char           *ckind    = NULL;
	char           *buffer   = NULL;
	int             position = 0;
	size_t          bufsize;

#define writebuffer(str)						\
	position += snprintf(buffer+position, bufsize - position, str)

	/* If the object was created in a prior pass, then do nothing */
	if (obj->created)
		return true;

	/*
	 * Initial buffersize is large enough for:
	 *   "CREATE EXTERNAL TABLE . (.) LOCATION (.) FORMAT TEXT (DELIMITER '.')"
	 * with a little wiggle room to spare.  We will extend this size below
	 * to allow for the variable length fields.
	 *
	 * TODO: it would be preferable to share a buffer between objects rather
	 * than allocating and releasing the buffer repeatedly.
	 */
	bufsize = 100;

	/* Otherwise attempt to create the object */
	switch (obj->kind)
	{
		case MAPRED_INPUT:
			XASSERT(obj->name);
			bufsize += strlen(obj->name);

			switch (obj->u.input.type)
			{
				case MAPRED_INPUT_TABLE:
					break;  /* do nothing */

				case MAPRED_INPUT_FILE:
				case MAPRED_INPUT_GPFDIST:
					XASSERT(obj->u.input.files);

					for (plist = obj->u.input.columns; plist; plist = plist->next)
					{
						bufsize += strlen(plist->name) + 1 + strlen(plist->type);
						if (plist->next)
							bufsize += 2;    /* ", " */
					}

					/* add room for file specifications */
					for (clist = obj->u.input.files; clist; clist = clist->next)
					{
						bufsize += 12;        /* "'gpfdist://'" */
						bufsize += strlen(clist->value);
						if (clist->next)
							bufsize += 13;    /* ",\n           " */
					}

					buffer = malloc(bufsize);
					writebuffer("CREATE EXTERNAL TABLE ");
					writebuffer(obj->name);
					writebuffer("(");
					for (plist = obj->u.input.columns; plist; plist = plist->next)
					{
						writebuffer(plist->name);
						writebuffer(" ");
						writebuffer(plist->type);
						if (plist->next)
							writebuffer(", ");
					}
					writebuffer(")\n");
					writebuffer("  LOCATION(");
					for (clist = obj->u.input.files; clist; clist = clist->next)
					{
						char *host, *path, *tokenizer;

						if (obj->u.input.type == MAPRED_INPUT_GPFDIST)
							writebuffer("'gpfdist://");
						else
							writebuffer("'file://");

						/* should be split by ':' */
						host = strtok_r(clist->value, ":", &tokenizer);
						path = strtok_r(NULL, ":", &tokenizer);
						if (!path)
							XRAISE(MAPRED_PARSE_ERROR,
								   "Invalid INPUT source specification");

						writebuffer(host);
						writebuffer(path);
						if (clist->next)
							writebuffer("',\n           ");
					}
					writebuffer("')\n");
					if (obj->u.input.format == MAPRED_FORMAT_CSV)
						writebuffer("  FORMAT 'CSV'");
					else
						writebuffer("  FORMAT 'TEXT'");
					if (obj->u.input.delimiter)
					{
						writebuffer(" (DELIMITER '");
						writebuffer(obj->u.input.delimiter);
						writebuffer("')");
					}
					writebuffer(";\n\n");
					break;

				case MAPRED_INPUT_EXEC:
					XASSERT(obj->u.input.desc);

					bufsize += strlen(obj->u.input.desc);
					buffer = malloc(bufsize);
					writebuffer("CREATE EXTERNAL WEB TABLE ");
					writebuffer(obj->name);
					writebuffer("(");
					for (plist = obj->u.input.columns; plist; plist = plist->next)
					{
						writebuffer(plist->name);
						writebuffer(" ");
						writebuffer(plist->type);
						if (plist->next)
							writebuffer(", ");
					}
					writebuffer(")\n");
					writebuffer("EXECUTE '");
					writebuffer(obj->u.input.desc);
					writebuffer("'\n");
					if (obj->u.input.format == MAPRED_FORMAT_CSV)
						writebuffer("  FORMAT 'CSV'");
					else
						writebuffer("  FORMAT 'TEXT'");
					if (obj->u.input.delimiter)
					{
						writebuffer(" (DELIMITER '");
						writebuffer(obj->u.input.delimiter);
						writebuffer("')");
					}
					writebuffer(";\n\n");
					break;

				case MAPRED_INPUT_QUERY:
					XASSERT(obj->u.input.desc);

					bufsize += strlen(obj->u.input.desc);
					buffer = malloc(bufsize);
					writebuffer("CREATE VIEW ");
					writebuffer(obj->name);
					writebuffer(" AS\n");
					writebuffer(obj->u.input.desc);
					writebuffer(";\n\n");
					
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
			break;

		/*
		 * The function types have different defaults and generate
		 * slightly different error messages, but basically do the
		 * same thing.  
		 */
		case MAPRED_MAPPER:
			ckind = "MAPPER";
			/* fallthrough */

		case MAPRED_TRANSITION:
			if (!ckind)
				ckind = "TRANSITION";
			/* fallthrough */

		case MAPRED_COMBINER:
			if (!ckind)
				ckind = "COMBINER";
			/* fallthrough */

		case MAPRED_FINALIZER:
			if (!ckind)
				ckind = "FINALIZER";

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
			XASSERT(obj->u.function.parameters);
			XASSERT(obj->u.function.rtype.name);

			/* How large does our buffer need to be? */
			bufsize +=
				strlen(obj->name) + 
				strlen(obj->u.function.body) +
				strlen(obj->u.function.language);

			/* Allocate space for function parameters */
			for (plist = obj->u.function.parameters; plist; plist = plist->next)
			{
				bufsize += strlen(plist->name) + 1 + strlen(plist->type);
				if (plist->next)
					bufsize += 2;    /* " ," */
			}

			/* allocate space for the "returns clause" */
			bufsize += strlen(obj->u.function.rtype.name);
		   

			/* 
			 * fill in the buffer:
			 *
			 *   [CREATE TYPE <name>_rtype as (<returns>);]
			 *
			 *    CREATE FUNCTION <name>(<parameters>)
			 *    RETURNS [SETOF] <rtype> LANGUAGE <lang> AS
			 *    $$
			 *    <body>
			 *    $$ [STRICT] [IMMUTABLE];
			 *     
			 */
			buffer = malloc(bufsize);
			writebuffer("CREATE FUNCTION ");
			writebuffer(obj->name);
			writebuffer("(");

			/* Handle parameter list */
			for (plist = obj->u.function.parameters; plist; plist = plist->next)
			{
				writebuffer(plist->name);
				writebuffer(" ");
				writebuffer(plist->type);
				if (plist->next)
					writebuffer(", ");
			}

			/* Handle Return clause */
			writebuffer(")\nRETURNS ");
			if (obj->u.function.mode == MAPRED_MULTI_MODE)
				writebuffer("SETOF ");
			writebuffer(obj->u.function.rtype.name);

			/* 
			 * Handle LANGUAGE clause, every langauge but 'C' and 'SQL'
			 * has 'pl' prefixing it
			 */
			if (!strcasecmp("C", obj->u.function.language) ||
				!strcasecmp("SQL", obj->u.function.language) ||
				!strncasecmp("PL", obj->u.function.language, 2))
			{
				writebuffer(" LANGUAGE ");
				writebuffer(obj->u.function.language);
			}
			else
			{
				writebuffer(" LANGUAGE pl");
				writebuffer(obj->u.function.language);
			}
			
			/* python only has an untrusted form */
			if (!strcasecmp("python", obj->u.function.language))
				writebuffer("u");


			/* Handle the body of the function */
			writebuffer(" AS\n$$\n");
			writebuffer(obj->u.function.body);
			if (buffer[position-1] != '\n')
				writebuffer("\n$$");
			else
				writebuffer("$$");

			/* Handle options */
			if (obj->u.function.flags & mapred_function_strict)
				writebuffer(" STRICT");
			if (obj->u.function.flags & mapred_function_immutable)
				writebuffer(" IMMUTABLE");
			
			/* All done */
			writebuffer(";\n\n");
			break;
			

		case MAPRED_REDUCER:
		{
			mapred_object_t *transition = obj->u.reducer.transition.object;
			char *state;

			XASSERT(obj->name);
			XASSERT(transition);
			XASSERT(transition->name);
			plist = transition->u.function.parameters;
			XASSERT(plist);          /* state */
			XASSERT(plist->next);    /* parameters */
			
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
				return false;
			}
			if (global_print_flag || global_debug_flag)
				printf("-- REDUCER %s\n", obj->name);

			bufsize += 
				strlen(obj->name) + 
				strlen(transition->name);
			for (plist = transition->u.function.parameters; plist; plist = plist->next)
			{
				bufsize += strlen(plist->type);
				if (plist->next)
					bufsize += 2;  /* ", " */
			}
			if (obj->u.reducer.initialize)
				bufsize += strlen(obj->u.reducer.initialize);
			if (obj->u.reducer.combiner.name)
				bufsize += strlen(obj->u.reducer.combiner.name);
			if (obj->u.reducer.finalizer.name)
				bufsize += strlen(obj->u.reducer.finalizer.name);

			buffer = malloc(bufsize);
			writebuffer("CREATE AGGREGATE ");
			writebuffer(obj->name);
			writebuffer(" (");

			/* 
			 * Get the state type, and write out the aggregate parameters based
			 * on the parameter list of the transition function.
			 */
			plist = transition->u.function.parameters;
			state = plist->type;
			for (plist = plist->next; plist; plist = plist->next)
			{
				writebuffer(plist->type);
				if (plist->next)
					writebuffer(", ");
			}
			writebuffer(") (\n");
			writebuffer("  stype = ");
			writebuffer(state);
			if (obj->u.reducer.initialize)
			{
				writebuffer(",\n  initcond = '");
				writebuffer(obj->u.reducer.initialize);
				writebuffer("'");
			}
			writebuffer(",\n  sfunc = ");
			writebuffer(transition->name);
			if (obj->u.reducer.combiner.name)
			{
				writebuffer(",\n  prefunc = ");
				writebuffer(obj->u.reducer.combiner.name);
			}
			if (obj->u.reducer.finalizer.name)
			{
				writebuffer(",\n  finalfunc = ");
				writebuffer(obj->u.reducer.finalizer.name);
			}
			writebuffer("\n);\n\n");
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
			char            *qbuffer  = NULL;
			size_t           qlen     = 0;
			size_t           qneed    = 0;
			size_t           paramlen = 0;

			if (!obj->u.task.execute)
				XASSERT(obj->name);
			XASSERT(obj->u.task.input.name);

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
				return false;
			}

			if (global_print_flag || global_debug_flag)
			{
				if (obj->u.task.execute)
					printf("-- EXECUTION\n");
				else
					printf("-- TASK %s\n", obj->name);
			}


			/* 
			 * If there are map functions or reduce functions which make
			 * use of the command line arguments then we need to make
			 * sure that we have enough space in the buffers for them.
			 *
			 * The easiest method is just to add up all their lengths and
			 * leave enough space for all of them if needed.
			 *
			 * (*HACK*)
			 * Note that the global_plist overloaded the "plist" structure
			 * and used the "type" field for "value". 
			 * (*HACK*)
			 */
			for (plist = global_plist; plist; plist = plist->next)
				paramlen += 2 + strlen(plist->type);

			/* 
			 * It is easiest to build the query starting from the
			 * input and working back towards the output.  This requires
			 * multiple buffers and several memcpys back and forth to 
			 * build the query, but results in more maintainable code.
			 */
			
			/* 
			 * 1) Handle the INPUT, two cases:
			 *   1a) There is no MAPPER/REDUCER:  "SELECT * FROM <input>"
			 *   1b) There is a MAPPER and/or REDUCER:  "<input>"
			 */
			if (!obj->u.task.mapper.name && !obj->u.task.reducer.name)
			{
				/* Allocate the buffer for the input. */
				if (input->u.input.type == MAPRED_INPUT_TABLE)
				{
					qlen = 15 + strlen(input->u.input.desc);
					qbuffer = malloc(qlen);
					snprintf(qbuffer, qlen, "SELECT * FROM %s", input->u.input.desc);
				}
				else
				{
					qlen = 15 + strlen(input->name);
					qbuffer = malloc(qlen);
					snprintf(qbuffer, qlen, "SELECT * FROM %s", input->name);
				}
			}
			else
			{
				/* 
				 * Point directly into input name - no actual buffer allocation
				 * qneed and qlen both remain == 0
				 */
				if (input->u.input.type == MAPRED_INPUT_TABLE)
					qbuffer = input->u.input.desc;
				else
					qbuffer = input->name;

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
			XASSERT(columns);   /* FIXME: this can fail! */

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
				XASSERT(!buffer);

				/*
				 * Input was written into qbuffer
				 * Mapper is written into buffer
				 *    buffer has not yet been allocated.
				 */
				bufsize = 25 +    /* SELECT .() as . FROM . */
					paramlen + 
					strlen(qbuffer) + 
					strlen(mapper->name);

				plist = mapper->u.function.parameters;
				for (plist = columns; plist; plist = plist->next)
				{
					bufsize += strlen(plist->name);
					if (plist->next)
						bufsize += 2;  /* ", " */
				}

				plist = mapper->u.function.returns;
				XASSERT(plist);
				if (plist->next)
				{  /* 2a */
					/*  "SELECT . FROM (.) mapxq WHERE m is not null" */
					bufsize += 50;
					for (; plist; plist = plist->next)
					{
						bufsize += 3 + strlen(plist->name);  /* ".(m)" */
						if (plist->next)
							bufsize += 2;  /* ", " */
					}
					buffer = malloc(bufsize);
					position = 0;

					writebuffer("SELECT ");
					plist = mapper->u.function.returns;
					for (; plist; plist = plist->next)
					{
						writebuffer(plist->name);
						writebuffer("(m)");
						if (plist->next)
							writebuffer(", ");

					}
					writebuffer(" FROM (");
				}
				else
				{  /* 2b */
					/* "SELECT .(.) as . FROM ." */
					bufsize += strlen(plist->name);
					buffer = malloc(bufsize);
					position = 0;
				}

				/* shared code */
				writebuffer("SELECT ");
				writebuffer(mapper->name);
				writebuffer("(");
				plist = mapper->u.function.parameters;
				for (; plist; plist = plist->next)
				{
					/* Check if this parameter is one of the input columns */
					for (scan = columns; scan; scan = scan->next)
						if (!strcasecmp(plist->name, scan->name))
						{
							writebuffer(plist->name);
							break;
						}

					/* Task inputs also need to scan the grouping columns */
					if (!scan)
						for (scan = ingrouping; scan; scan = scan->next)
							if (!strcasecmp(plist->name, scan->name))
							{
								writebuffer(plist->name);
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
								writebuffer("'");
								writebuffer(scan->type);
								writebuffer("'::");
								writebuffer(plist->type);
								break;
							}


					/* If we couldn't find it issue a warning and set to NULL */
					if (!scan)
					{
						fprintf(stderr, 
								"WARNING: unset parameter - %s(%s => NULL)\n",
								mapper->name, plist->name);
						writebuffer("NULL::");
						writebuffer(plist->type);
					}

					/* Add a comma if there is another parameter */
					if (plist->next)
						writebuffer(", ");
				}
				writebuffer(") as ");

				/* break into cases again */
				plist = mapper->u.function.returns;
				if (plist->next)
				{  /* 2a */
					writebuffer("m FROM ");
					writebuffer(qbuffer);
					writebuffer(") mapxq\n");
					writebuffer("WHERE m is not null");
				}
				else
				{
					writebuffer(plist->name);
					writebuffer(" FROM ");
					writebuffer(qbuffer);
				}

				/* If an allocation was made for the qbuffer, release it */
				if (qlen)
				{
					free(qbuffer);
					qlen = 0;
				}

				/* 
				 * Swap the buffer into the qbuffer for input as the next stage
				 * of the query pipeline. 
				 */
				qbuffer = buffer;
				qlen = bufsize;
				buffer = NULL;
				bufsize = 0;

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
								last->next = malloc(sizeof(mapred_plist_t));
								last = last->next;
							}
							else
							{
								grouping = malloc(sizeof(mapred_plist_t));
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
					for (plist = columns; plist; plist = plist->next)
					{
						scan = reducer->u.reducer.parameters;
						for (; scan; scan = scan->next)
						{
							if (!strcasecmp(plist->name, scan->name))
								break;
						}

						/* not a parameter, add it to the list */
						if (!scan)
						{
							/* allocate new entry on the end */
							if (grouping)
							{
								last->next = malloc(sizeof(mapred_plist_t));
								last = last->next;
							}
							else
							{
								grouping = malloc(sizeof(mapred_plist_t));
								last = grouping;
							}
							last->name = plist->name;
							last->type = plist->type;
							last->next = NULL;
						}
					}
				}
				
				/* SELECT ., .(.) as . FROM . GROUP BY . */
				bufsize = 50 + 
					paramlen + 
					strlen(obj->u.task.reducer.name) +
					strlen(qbuffer);
				
				/* 
				 * The grouping columns occur both in the select list and in
				 * the GROUP BY clause, so we double count the size 
				 */
				for (plist = grouping; plist; plist = plist->next)
				{
					bufsize += 2 * strlen(plist->name);
					if (plist->next)
						bufsize += 2 * 2;  /* ", " */
				}

				/* Parameters to the reducer */
				if (reducer)
				{
					plist = reducer->u.reducer.parameters;
					for (; plist; plist = plist->next)
					{
						bufsize += strlen(plist->name);
						if (plist->next)
							bufsize += 2;  /* ", " */
					}

					plist = reducer->u.reducer.returns;
					if (!plist || plist->next)
						XASSERT(false); /* FIXME: multi-column returning reducers? */
					bufsize += strlen(plist->name);
				}
				else
				{
					/* non-yaml reducers always take "value" column as input */
					bufsize += 5;  /* value */
				}
								
				/* 
				 * How we handle the from clause depends on if there was a
				 * map function.
				 */
				if (obj->u.task.mapper.name)
					bufsize += 10;  /* "FROM (.) mapsubq" */

				/* Write it out */
				buffer = malloc(bufsize);
				position = 0;
				writebuffer("SELECT ");
				for (plist = grouping; plist; plist = plist->next)
				{
					writebuffer(plist->name);
					writebuffer(", ");
				}
				writebuffer(obj->u.task.reducer.name);
				writebuffer("(");

				if (reducer)
				{
					plist = reducer->u.reducer.parameters;
					for (; plist; plist = plist->next)
					{
						/* Check if this parameter is one of the input columns */
						for (scan = columns; scan; scan = scan->next)
							if (!strcasecmp(plist->name, scan->name))
							{
								writebuffer(plist->name);
								break;
							}

						/* Task inputs also need to scan the grouping columns */
						if (!scan)
							for (scan = ingrouping; scan; scan = scan->next)
								if (!strcasecmp(plist->name, scan->name))
								{
									writebuffer(plist->name);
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
									writebuffer("'");
									writebuffer(scan->type);
									writebuffer("'::");
									writebuffer(plist->type);
									break;
								}


						/* If we couldn't find it issue a warning and set to NULL */
						if (!scan)
						{
							fprintf(stderr, 
									"WARNING: unset parameter - %s(%s => NULL)\n",
									reducer->name, plist->name);
							writebuffer("NULL::");
							writebuffer(plist->type);
						}

						/* Write out the comma if there is another input parameter */
						if (plist->next)
							writebuffer(", ");
					}
				}
				else
				{
					/* non-yaml reducer always takes "value" as it's input column */

					/* Check if "value" is one of the input columns */
					for (scan = columns; scan; scan = scan->next)
						if (!strcasecmp(scan->name, "value"))
						{
							writebuffer("value");
							break;
						}

					/* Task inputs also need to scan the grouping columns */
					if (!scan)
						for (scan = ingrouping; scan; scan = scan->next)
							if (!strcasecmp(plist->name, scan->name))
							{
								writebuffer("value");
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
								writebuffer("'");
								writebuffer(scan->type);
								writebuffer("'::");
								writebuffer(plist->type);
								break;
							}

					if (!scan)
					{
						fprintf(stderr, 
								"WARNING: unset parameter - %s(value => NULL)\n",
								obj->u.task.reducer.name);
						writebuffer("NULL");
					}
				}

				writebuffer(") as ");

				if (reducer)
				{
					plist = reducer->u.reducer.returns;
					if (!plist || plist->next)
						XASSERT(false); /* FIXME: multi-column returning reducers? */
					writebuffer(plist->name);
				}
				else
				{
					/* non-yaml reducer always returns a single column named "value" */
					writebuffer("value");
				}
				writebuffer("\nFROM ");
				if (obj->u.task.mapper.name)
				{
					writebuffer("(");
					writebuffer(qbuffer);
					writebuffer(") mapsubq");
				}
				else
				{
					writebuffer(qbuffer);
				}
				if (grouping)
				{
					writebuffer("\nGROUP BY ");
					for (plist = grouping; plist; plist = plist->next)
					{
						writebuffer(plist->name);
						if (plist->next)
							writebuffer(", ");
					}
				}


				/* If an allocation was made for the qbuffer, release it */
				if (qlen)
				{
					free(qbuffer);
					qlen = 0;
				}
				
				/* 
				 * Swap the buffer into the qbuffer for input as the next stage
				 * of the query pipeline. 
				 */
				qbuffer = buffer;
				qlen = bufsize;
				buffer = NULL;
				bufsize = 0;

				/* 
				 * Add the return columns to the grouping columns and set
				 * it to the current columns.  
				 *
				 * Note that unlike the columns set by the mapper or the
				 * input this is a list that must be de-allocated.
				 */
				if (grouping)
				{
					last->next = malloc(sizeof(mapred_plist_t));
					last = last->next;
				}
				else
				{
					grouping = malloc(sizeof(mapred_plist_t));
					last = grouping;
				}
				last->name = "value";  /* FIXME */
				last->type = "text";   /* FIXME */
				last->next = NULL;
				columns = grouping;
			}


			/*
			 * 4) Handle the final transform into the view definition:
			 *        "CREATE VIEW . AS . ORDER BY .;"
			 */
			bufsize = 50 + strlen(obj->name) + strlen(qbuffer);
			for (plist = columns; plist; plist = plist->next)
			{
				bufsize += strlen(plist->name);
				if (plist->next)
					bufsize += 2;  /* ", " */
			}
			
			buffer = malloc(bufsize);
			position = 0;
			writebuffer("CREATE VIEW ");
			writebuffer(obj->name);
			writebuffer(" AS\n");
			writebuffer(qbuffer);
			writebuffer(";\n\n");

			/* 
			 * If there was a reducer then we have to release the columns
			 * list, otherwise it is a pointer to an existing list and can
			 * be ignored.
			 */
			if (obj->u.task.reducer.name)
			{
				plist = columns;
				while (plist)
				{
					newitem = plist;
					plist = plist->next;
					free(newitem);
				}
			}
			break;
		}

		case MAPRED_ADT:
			XASSERT(obj->name);

			bufsize += strlen(obj->name);
			for (plist = obj->u.adt.returns; plist; plist = plist->next)
			{
				bufsize += strlen(plist->name) + 1 + strlen(plist->type);
				if (plist->next)
					bufsize += 2;  /* ", " */
			}
			
			buffer = malloc(bufsize);
			writebuffer("CREATE TYPE ");
			writebuffer(obj->name);
			writebuffer(" as (");
			for (plist = obj->u.adt.returns; plist; plist = plist->next)
			{
				writebuffer(plist->name);
				writebuffer(" ");
				writebuffer(plist->type);
				if (plist->next)
					writebuffer(", ");
			}
			writebuffer(");\n\n");
			break;

		default:
			XASSERT(false);

	}

	if (buffer)
	{
		PGresult   *result;

		if (global_print_flag)
		{
			printf("%s", buffer);
			free(buffer);
			XASSERT(position < bufsize);
		}
		else
		{
			if (global_debug_flag)
				printf("%s", buffer);				

			/* There should be room to spare, but if not - spill the beans */
			if (position >= bufsize)
			{
				free(buffer);
				XASSERT(false);
			}

			/* Try to create the object */
			result = PQexec(conn, buffer);
			free(buffer);

			if (PQresultStatus(result) != PGRES_COMMAND_OK)
				XRAISE(MAPRED_SQL_ERROR, "Object creation Failure");
		}
	}

	/* set the created flag on the object and return true */
	obj->created = true;
	return true;

#undef writebuffer
}


void mapred_remove_object(PGconn *conn, mapred_object_t *obj)
{
	mapred_clist_t *clist    = NULL;
	mapred_plist_t *plist    = NULL;
	char           *ckind    = NULL;
	char           *buffer   = NULL;
	int             position = 0;
	size_t          bufsize;

#define writebuffer(str)						\
	position += snprintf(buffer+position, bufsize - position, str)

	/* If the object wasn't created, then do nothing */
//	if (!obj->created)
//		return;

	/*
	 * Initial buffersize is large enough for:
	 *   "CREATE EXTERNAL TABLE . (.) LOCATION (.) FORMAT TEXT (DELIMITER .)"
	 * with a little wiggle room to spare.  We will extend this size below
	 * to allow for the variable length fields.
	 *
	 * TODO: it would be preferable to share a buffer between objects rather
	 * than allocating and releasing the buffer repeatedly.
	 */
	bufsize = 100;

	if (obj->name)
		bufsize += strlen(obj->name);

	/* Otherwise attempt to create the object */
	switch (obj->kind)
	{
		case MAPRED_INPUT:
			XASSERT(obj->name);

			switch (obj->u.input.type)
			{
				case MAPRED_INPUT_TABLE:
					break;  /* do nothing */

				case MAPRED_INPUT_FILE:
				case MAPRED_INPUT_GPFDIST:
				case MAPRED_INPUT_EXEC:
					buffer = malloc(bufsize);
					writebuffer("DROP EXTERNAL TABLE IF EXISTS ");
					writebuffer(obj->name);
					writebuffer(" CASCADE;\n");
					break;

				case MAPRED_INPUT_QUERY:
					buffer = malloc(bufsize);
					writebuffer("DROP VIEW IF EXISTS ");
					writebuffer(obj->name);
					writebuffer(" CASCADE;\n");					
					break;

				case MAPRED_INPUT_NONE:
				default:
					XASSERT(false);
			}
			break;

		case MAPRED_OUTPUT:
			/* nothing to do for outputs */
			break;

		/*
		 * The function types have different defaults and generate
		 * slightly different error messages, but basically do the
		 * same thing.  
		 */
		case MAPRED_MAPPER:
			ckind = "MAPPER";
			/* fallthrough */

		case MAPRED_TRANSITION:
			if (!ckind)
				ckind = "TRANSITION";
			/* fallthrough */

		case MAPRED_COMBINER:
			if (!ckind)
				ckind = "COMBINER";
			/* fallthrough */

		case MAPRED_FINALIZER:
			if (!ckind)
				ckind = "FINALIZER";

			/*
			 * 'kind' specific initialization accomplished above, now handle
			 * the generic function creation.
			 */
			
			/* It should have already been verified */
			XASSERT(obj->name);
			for (plist = obj->u.function.parameters; plist; plist = plist->next)
			{
				bufsize += strlen(plist->type);
				if (plist->next)
					bufsize += 2;  /* ', ' */
			}

			/* 
			 * Allocate space for dropping the return type, if present:
			 *     "DROP TYPE <name>_rtype CASCADE;\n"
			 */
			plist = obj->u.function.returns;
			if (plist->next)
				bufsize += 30 + strlen(obj->name);

			/* Fill in the buffer */
			buffer = malloc(bufsize);
			writebuffer("DROP FUNCTION IF EXISTS ");
			writebuffer(obj->name);
			writebuffer("(");

			/* Handle parameter list */
			for (plist = obj->u.function.parameters; plist; plist = plist->next)
			{
				writebuffer(plist->type);
				if (plist->next)
					writebuffer(", ");
			}

			/* All done */
			writebuffer(") CASCADE;\n");
			break;
			

		case MAPRED_REDUCER:
		{
			mapred_object_t  *transition = obj->u.reducer.transition.object;

			XASSERT(obj->name);
			XASSERT(transition);   /* is this a good assumption? */

			if (!transition->u.function.parameters)
				break;  /* FIXME */
			XASSERT(transition->u.function.parameters);

			/* 
			 * The first parameter of the transition function is the 'state' and is
			 * not listed as a parameter of the reducer, but all the rest of the
			 * parameters are 
			 */
			plist = transition->u.function.parameters;
			for (plist = plist->next; plist; plist = plist->next)
			{
				bufsize += strlen(plist->type);
				if (plist->next)
					bufsize += 2;  /* ', ' */
			}

			buffer = malloc(bufsize);
			writebuffer("DROP AGGREGATE IF EXISTS ");
			writebuffer(obj->name);
			writebuffer("(");
			plist = transition->u.function.parameters;
			for (plist = plist->next; plist; plist = plist->next)
			{
				writebuffer(plist->type);
				if (plist->next)
					writebuffer(", ");
			}
			writebuffer(") CASCADE;\n");
			break;
		}

		case MAPRED_TASK:
		case MAPRED_EXECUTION:
			XASSERT(obj->name);

			buffer = malloc(bufsize);
			writebuffer("DROP VIEW IF EXISTS ");
			writebuffer(obj->name);
			writebuffer(" CASCADE;\n");
			break;

		case MAPRED_ADT:
			buffer = malloc(bufsize);
			writebuffer("DROP TYPE IF EXISTS ");
			writebuffer(obj->name);
			writebuffer(" CASCADE;\n");
			break;

		default:
			XASSERT(false);

	}

	if (buffer)
	{
		PGresult   *result;

		if (global_print_flag)
		{
			printf("%s", buffer);
			free(buffer);
			XASSERT(position < bufsize);
		}
		else
		{
			if (global_debug_flag)
				printf("%s", buffer);

			/* There should be room to spare, but if not - spill the beans */
			if (position >= bufsize)
			{
				free(buffer);
				XASSERT(false);
			}

			/* Try to create the object */
			result = PQexec(conn, buffer);
			free(buffer);

			if (PQresultStatus(result) != PGRES_COMMAND_OK)
				XRAISE(MAPRED_SQL_ERROR, "Object creation Failure");
		}
	}

	/* clear the created flag */
	obj->created = false;

#undef writebuffer
}
