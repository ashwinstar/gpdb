#include <parser.h>
#include <except.h>
#include <mapred_errors.h>

#include <stdio.h>
#include <yaml_parse.h>
#include <yaml.h>


int mapred_verify_object(mapred_parser_t *parser, mapred_object_t *obj);

static int 
error_missing(const char *type, const char *name, int line, 
			  const char *missing)
{
	if (global_verbose_flag)
		fprintf(stderr, "    - ");
	fprintf(stderr, "Error: %s '%s' - Missing %s, at line %d\n",
			type, name, missing, line);
	return MAPRED_PARSE_ERROR;
}

#define error1(x,y)								\
	do {										\
		printf("Error: %s %s\n", x, y);			\
		exit(1);								\
	} while (0)

#define copyscalar(s)							\
	strcpy(malloc(strlen(s)+1), s)

mapred_olist_t* mapred_parse_string(unsigned char *yaml)
{
	mapred_olist_t  *documents;
	yaml_parser_t    parser;
	
	XASSERT(yaml);
	if (!yaml_parser_initialize(&parser)) 
		XRAISE(MAPRED_PARSE_INTERNAL, 
			   "YAML parser initialization failed");

	yaml_parser_set_input_string(&parser, yaml, strlen((char*) yaml));
	documents = mapred_parse_yaml(&parser);
	yaml_parser_delete(&parser);
	return documents;
}

mapred_olist_t* mapred_parse_file(FILE *file)
{
	mapred_olist_t  *documents;
	yaml_parser_t parser;

	XASSERT(file);
	if (!yaml_parser_initialize(&parser))
		XRAISE(MAPRED_PARSE_INTERNAL, 
			   "YAML parser initialization failed");

	yaml_parser_set_input_file(&parser, file);
	documents = mapred_parse_yaml(&parser);
	yaml_parser_delete(&parser);
	return documents;
}

mapred_olist_t* mapred_parse_yaml(yaml_parser_t *yparser)
{
	mapred_parser_t  parser;
	int              i;
	int              error = 0;

	/* Give us a clean slate */
	memset(&parser, 0, sizeof(parser));

	/* Initialize what must be initialized */
#if USE_FLEX_REENTRANT
	yaml_scalar_yylex_init (&parser.yscanner);
#endif
	parser.yparser = yparser;
	parser.state   = STATE_YAML_PARSE;
	parser.frame   = -1;
	for (i = 0; i < MAX_CONTEXT_DEPTH; i++)
		parser.context[i] = CONTEXT_NONE;

	/* Call into the parser */
	error = yaml_yyparse(&parser);

	if (error)
	{
		mapred_destroy_object(&parser.current_obj);
		mapred_destroy_object(&parser.current_doc);

		if (global_verbose_flag)
			fprintf(stderr, "  - ");
		fprintf(stderr, "Error parsing file\n");
	}
	else
	{
		/* finalize final document */
		parser_add_document(&parser);
	}

	/* Cleanup and return */
#if USE_FLEX_REENTRANT
	yaml_scalar_yylex_destroy(parser.yscanner);
#endif
		
	return parser.doclist;
}


void parser_add_document(mapred_parser_t *parser)
{
	mapred_olist_t     *newitem;
	mapred_olist_t     *doclist;
	int                 error;

	if (!parser->current_doc)
		return;

	/* Add the last of the documents objects into the document */
	parser_add_object(parser, MAPRED_NO_KIND);

	/* Verify the completed document */
	error = mapred_verify_object(parser, parser->current_doc);
	if (error != NO_ERROR)
		parser->current_doc->u.document.flags |= mapred_document_error;
	
	if (parser->current_doc->u.document.flags & mapred_document_error)
	{
		mapred_destroy_object(&parser->current_doc);
		return;
	}

	/* Allocate the new list item */
	newitem = malloc(sizeof(mapred_olist_t));
	newitem->object = parser->current_doc;
	newitem->next   = (mapred_olist_t *) NULL;

	/* Insert it into the last slot of the existing list */
	doclist = parser->doclist;
	while (doclist && doclist->next)
		doclist = doclist->next;
	if (doclist)
		doclist->next = newitem;
	else
		parser->doclist = newitem;
}

void parser_begin_document(mapred_parser_t *parser)
{
	/* If there is a current document add it first */
	parser_add_document(parser);

	/* Allocate an object for the new document and return */
	parser->current_doc = malloc(sizeof(mapred_object_t));
	memset(parser->current_doc, 0, sizeof(mapred_object_t));
	parser->current_doc->kind = MAPRED_DOCUMENT;
	parser->current_doc->u.document.id = ++parser->doc_number;
	parser->current_doc->line = (int) parser->event.start_mark.line+1;

	if (global_verbose_flag)
		fprintf(stderr, "  - Parsing YAML Document %d:\n", parser->doc_number);
}

void parser_begin_define(mapred_parser_t *parser)
{
	XASSERT(parser->current_doc);

	/* 
	 * The only thing we have to do is ensure that this isn't a duplicate
	 * define list.
	 */
	if (parser->current_doc->u.document.flags & mapred_document_defines)
		XRAISE(MAPRED_PARSE_ERROR, 
			   "Duplicate EXECUTE list in DOCUMENT");
	parser->current_doc->u.document.flags |= mapred_document_defines;
}

void parser_begin_execute(mapred_parser_t *parser)
{
	XASSERT(parser->current_doc);

	/* 
	 * The only thing we have to do is ensure that this isn't a duplicate
	 * execution list.
	 */
	if (parser->current_doc->u.document.flags & mapred_document_executes)
		XRAISE(MAPRED_PARSE_ERROR, 
			   "Duplicate EXECUTE list in DOCUMENT");
	parser->current_doc->u.document.flags |= mapred_document_executes;
}

void parser_set_version(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);

	if (parser->current_doc->u.document.version)
		error1("Duplicate Version:", value);

	/* 
	 * We have already assured that the value matches a good regex,
	 * but we must still validate that the version itself is supported.
	 */
	/* FIXME */
	parser->current_doc->u.document.version = copyscalar(value);
}

void parser_set_database(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	if (parser->current_doc->u.document.database)
		error1("Duplicate Database:", value);
	parser->current_doc->u.document.database = copyscalar(value);
}

void parser_set_user(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	if (parser->current_doc->u.document.user)
		error1("Duplicate User:", value);
	parser->current_doc->u.document.user = copyscalar(value);
}

void parser_set_host(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	if (parser->current_doc->u.document.host)
		error1("Duplicate Host:", value);
	parser->current_doc->u.document.host = copyscalar(value);
}

void parser_set_port(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	if (parser->current_doc->u.document.port > 0)
		error1("Duplicate Port:", value);

	/* 
	 * The parse has already assured that the value consists of a sequence
	 * of digits, so strtol should convert successfully. 
	 */
	parser->current_doc->u.document.port = (int) strtol(value, NULL, 10);
}


/*
 * parser_add_object - Create a new empty object for the current document.
 */
void parser_add_object(mapred_parser_t *parser, mapred_kind_t kind)
{
	int error;

	XASSERT(parser->current_doc);

	/* 
	 * If we have a current object then verify it and add it into the
	 * document's object list. 
	 */
	if (parser->current_obj)
	{
		mapred_olist_t *newitem;
		mapred_olist_t *objlist;

		/* Validate the finished object */
		error = mapred_verify_object(parser, parser->current_obj);
		if (error != NO_ERROR)
		{
			mapred_destroy_object(&parser->current_obj);
			parser->current_doc->u.document.flags |= 
				mapred_document_error;
		}
		else
		{

			/* Allocate the new list item */
			newitem = malloc(sizeof(mapred_olist_t));
			newitem->object = parser->current_obj;
			newitem->next   = (mapred_olist_t *) NULL;

			/* Insert it into the last slot of the existing list */
			objlist = parser->current_doc->u.document.objects;
			while (objlist && objlist->next)
				objlist = objlist->next;
			if (objlist)
				objlist->next = newitem;
			else
				parser->current_doc->u.document.objects = newitem;

			if (global_verbose_flag)
			{
				const char *type, *name;
				XASSERT (newitem->object->kind > 0 && 
						 newitem->object->kind <= MAPRED_MAXKIND);


				type = mapred_kind_name[newitem->object->kind];
				name = newitem->object->name; 
				if (name)
					fprintf(stderr, "    - %s: %s\n", type, name);
				else
					fprintf(stderr, "    - %s\n", type);
			}
		}
	}

	/* 
	 * If 'kind' is 'NO_KIND' then we just add in the current object
	 * (above) and do not create a new one.  We call it this way once
	 * at the end to add the last object into the current document.
	 */
	if (kind == MAPRED_NO_KIND)
	{
		parser->current_obj = (mapred_object_t *) NULL;
		return;
	}

	/* Allocate a new empyt object of the correct kind and return. */
	parser->current_obj = malloc(sizeof(mapred_object_t));
	memset(parser->current_obj, 0, sizeof(mapred_object_t));
	parser->current_obj->kind = kind;
	parser->current_obj->line = (int) parser->event.start_mark.line+1;
}



void parser_add_run(mapred_parser_t *parser)
{
	/*
	 * Execution objects just re-use the 'task' structure.  The only
	 * differences are that:
	 *    Execution objects get RUN
	 *    Execution objects do not require (or support) a NAME
	 */
	parser_add_object(parser, MAPRED_EXECUTION);
	parser->current_obj->u.task.execute = true;
}



void parser_set_name(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_INPUT      ||
		   parser->current_obj->kind == MAPRED_OUTPUT     ||
		   parser->current_obj->kind == MAPRED_MAPPER     ||
		   parser->current_obj->kind == MAPRED_TRANSITION ||
		   parser->current_obj->kind == MAPRED_COMBINER   ||
		   parser->current_obj->kind == MAPRED_FINALIZER  ||
		   parser->current_obj->kind == MAPRED_REDUCER    ||
		   parser->current_obj->kind == MAPRED_TASK);

	/* If the object already has a name throw an error */
	if (parser->current_obj->name)
	{
		switch (parser->current_obj->kind)
		{
			case MAPRED_INPUT:
				error1("Duplicate name for INPUT:", value);
			case MAPRED_OUTPUT:
				error1("Duplicate name for OUTPUT:", value);
			case MAPRED_MAPPER:
				error1("Duplicate name for MAP:", value);
			case MAPRED_TRANSITION:
				error1("Duplicate name for TRANSITION:", value);
			case MAPRED_COMBINER:
				error1("Duplicate name for COMBINER:", value);
			case MAPRED_FINALIZER:
				error1("Duplicate name for FINALIZER:", value);
			case MAPRED_REDUCER:
				error1("Duplicate name for REDUCER:", value);
			case MAPRED_TASK:
				error1("Duplicate name for TASK:", value);
			default:
				XASSERT(false);
		}
	}
	parser->current_obj->name = copyscalar(value);
}

void parser_set_table(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_INPUT ||
		   parser->current_obj->kind == MAPRED_OUTPUT);

	if (parser->current_obj->kind == MAPRED_INPUT)
	{
		if (parser->current_obj->u.input.type != MAPRED_INPUT_NONE)
		{
			if (parser->current_obj->u.input.type == MAPRED_INPUT_TABLE)
				XRAISE(MAPRED_PARSE_ERROR,
					   "Duplicate TABLE for INPUT");
			XRAISE(MAPRED_PARSE_ERROR,
				   "INPUT may only specify one of FILE, GPFDIST, TABLE, QUERY, EXEC");
		}
		parser->current_obj->u.input.type = MAPRED_INPUT_TABLE;
		parser->current_obj->u.input.desc = copyscalar(value);
	}
	else
	{
		if (parser->current_obj->u.output.type != MAPRED_OUTPUT_NONE)
		{
			if (parser->current_obj->u.input.type == MAPRED_INPUT_TABLE)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate TABLE for OUTPUT");
			XRAISE(MAPRED_PARSE_ERROR,
				   "OUTPUT may only specify one of FILE, TABLE");
		}
		parser->current_obj->u.output.type = MAPRED_OUTPUT_TABLE;
		parser->current_obj->u.output.desc = copyscalar(value);
	}
}

void parser_set_query(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_INPUT);

	if (parser->current_obj->u.input.type != MAPRED_INPUT_NONE)
	{
		if (parser->current_obj->u.input.type == MAPRED_INPUT_QUERY)
			XRAISE(MAPRED_PARSE_ERROR,
				   "Duplicate QUERY for INPUT");
		XRAISE(MAPRED_PARSE_ERROR,
			   "INPUT may only specify one of FILE, GPFDIST, TABLE, QUERY, EXEC");
	}
	parser->current_obj->u.input.type = MAPRED_INPUT_QUERY;
	parser->current_obj->u.input.desc = copyscalar(value);
}

void parser_set_exec(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_INPUT);

	if (parser->current_obj->u.input.type != MAPRED_INPUT_NONE)
	{
		if (parser->current_obj->u.input.type == MAPRED_INPUT_EXEC)
			XRAISE(MAPRED_PARSE_ERROR, 
				   "Duplicate EXEC for INPUT");
		XRAISE(MAPRED_PARSE_ERROR, 
			   "INPUT may only specify one of FILE, GPFDIST, TABLE, QUERY, EXEC");
	}
	parser->current_obj->u.input.type = MAPRED_INPUT_EXEC;
	parser->current_obj->u.input.desc = copyscalar(value);
}

void parser_set_format(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_INPUT);

	if (parser->current_obj->u.input.format != MAPRED_FORMAT_NONE)
		XRAISE(MAPRED_PARSE_ERROR, 
			   "Duplicate FORMAT for INPUT");
	
	if (!strcasecmp(value, "text"))
		parser->current_obj->u.input.format = MAPRED_FORMAT_TEXT;
	else if (!strcasecmp(value, "csv"))
		parser->current_obj->u.input.format = MAPRED_FORMAT_CSV;
	else
		error1("Invalid INPUT FORMAT:", value);
}

void parser_set_delimiter(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_INPUT ||
			parser->current_obj->kind == MAPRED_OUTPUT);

	if (parser->current_obj->kind == MAPRED_INPUT)
	{
		if (parser->current_obj->u.input.delimiter)
			XRAISE(MAPRED_PARSE_ERROR, 
				   "Duplicate DELIMITER for INPUT");
		parser->current_obj->u.input.delimiter = copyscalar(value);
	}
	else
	{
		if (parser->current_obj->u.output.delimiter)
			XRAISE(MAPRED_PARSE_ERROR, 
				   "Duplicate DELIMITER for OUTPUT");
		parser->current_obj->u.output.delimiter = copyscalar(value);
	}
}

void parser_set_escape(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_INPUT);

	if (parser->current_obj->u.input.delimiter)
		XRAISE(MAPRED_PARSE_ERROR, 
			   "Duplicate ESCAPE for INPUT");
	parser->current_obj->u.input.escape = copyscalar(value);
}


void parser_set_null(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_INPUT);

	if (parser->current_obj->u.input.null)
		XRAISE(MAPRED_PARSE_ERROR, 
			   "Duplicate NULL for INPUT");
	parser->current_obj->u.input.null = copyscalar(value);
}

void parser_set_quote(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_INPUT);

	if (parser->current_obj->u.input.quote)
		XRAISE(MAPRED_PARSE_ERROR, 
			   "Duplicate QUOTE for INPUT");
	parser->current_obj->u.input.quote = copyscalar(value);
}


void parser_set_encoding(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_INPUT);

	if (parser->current_obj->u.input.encoding)
		XRAISE(MAPRED_PARSE_ERROR, 
			   "Duplicate ENCODING for INPUT");
	parser->current_obj->u.input.encoding = copyscalar(value);
}

void parser_set_error_limit(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_INPUT);

	if (parser->current_obj->u.input.error_limit > 0)
		XRAISE(MAPRED_PARSE_ERROR, 
			   "Duplicate ERROR_LIMIT for INPUT");		

	/* 
	 * The parse has already assured that the value consists of a sequence
	 * of digits, so strtol should convert successfully. 
	 */
	parser->current_obj->u.input.error_limit = (int) strtol(value, NULL, 10);
}


void parser_set_mode(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);

	/* MODE is supported for the function types and OUTPUTS */
	switch (parser->current_obj->kind)
	{
		case MAPRED_MAPPER:
			if (parser->current_obj->u.function.mode != MAPRED_NO_MODE)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate MODE for MAPPER");
			if (!strcasecmp(value, "single"))
				parser->current_obj->u.function.mode = MAPRED_SINGLE_MODE;
			else if (!strcasecmp(value, "multi"))
				parser->current_obj->u.function.mode = MAPRED_MULTI_MODE;
			else if (!strcasecmp(value, "accumulated"))
				parser->current_obj->u.function.mode = MAPRED_ACCUMULATED_MODE;
            else if (!strcasecmp(value, "windowed"))
				parser->current_obj->u.function.mode = MAPRED_WINDOWED_MODE;
			else
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Unknown MODE for MAPPER");
			break;

		case MAPRED_TRANSITION:
			if (parser->current_obj->u.function.mode != MAPRED_NO_MODE)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate MODE for TRANSITION");
			if (!strcasecmp(value, "single"))
				parser->current_obj->u.function.mode = MAPRED_SINGLE_MODE;
			else if (!strcasecmp(value, "multi"))
				parser->current_obj->u.function.mode = MAPRED_MULTI_MODE;
			else if (!strcasecmp(value, "accumulated"))
				parser->current_obj->u.function.mode = MAPRED_ACCUMULATED_MODE;
            else if (!strcasecmp(value, "windowed"))
				parser->current_obj->u.function.mode = MAPRED_WINDOWED_MODE;
			else
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Unknown MODE for TRANSITION");
			break;

		case MAPRED_COMBINER:
			if (parser->current_obj->u.function.mode != MAPRED_NO_MODE)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate MODE for COMBINER");
			if (!strcasecmp(value, "single"))
				parser->current_obj->u.function.mode = MAPRED_SINGLE_MODE;
			else if (!strcasecmp(value, "multi"))
				parser->current_obj->u.function.mode = MAPRED_MULTI_MODE;
			else if (!strcasecmp(value, "accumulated"))
				parser->current_obj->u.function.mode = MAPRED_ACCUMULATED_MODE;
            else if (!strcasecmp(value, "windowed"))
				parser->current_obj->u.function.mode = MAPRED_WINDOWED_MODE;
			else
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Unknown MODE for COMBINER");
			break;

		case MAPRED_FINALIZER:
			if (parser->current_obj->u.function.mode != MAPRED_NO_MODE)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate MODE for FINALIZER");
			if (!strcasecmp(value, "single"))
				parser->current_obj->u.function.mode = MAPRED_SINGLE_MODE;
			else if (!strcasecmp(value, "multi"))
				parser->current_obj->u.function.mode = MAPRED_MULTI_MODE;
			else if (!strcasecmp(value, "accumulated"))
				parser->current_obj->u.function.mode = MAPRED_ACCUMULATED_MODE;
            else if (!strcasecmp(value, "windowed"))
				parser->current_obj->u.function.mode = MAPRED_WINDOWED_MODE;
			else
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Unknown MODE for FINALIZER");
			break;

		case MAPRED_OUTPUT:
			if (parser->current_obj->u.output.mode != MAPRED_OUTPUT_NO_MODE)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate MODE for FINALIZER");
			if (!strcasecmp(value, "replace"))
				parser->current_obj->u.output.mode = MAPRED_OUTPUT_REPLACE;
			else if (!strcasecmp(value, "append"))
				parser->current_obj->u.output.mode = MAPRED_OUTPUT_APPEND;
			else
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Unknown MODE for OUTPUT");
			break;

		default:
			XASSERT(false);
	}
}

void parser_set_file(mapred_parser_t *parser, char *value)
{
	/* 
	 * Only applies to OUTPUTS which have a single file.
	 * INPUTS use parser_begin_files, parser_add_file ...
	 */
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_OUTPUT);

	switch (parser->current_obj->u.output.type)
	{
		case MAPRED_OUTPUT_NONE:
			parser->current_obj->u.output.type = MAPRED_OUTPUT_FILE;
			parser->current_obj->u.output.desc = copyscalar(value);
			break;

		case MAPRED_OUTPUT_FILE:
			XRAISE(MAPRED_PARSE_ERROR, 
				   "Duplicate FILE for OUTPUT");

		case MAPRED_OUTPUT_TABLE:
			XRAISE(MAPRED_PARSE_ERROR, 
				   "OUTPUT may only specify one of FILE, TABLE");
			
		default:
			XASSERT(false);
	}
}

void parser_set_transition(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_REDUCER);

	if (parser->current_obj->u.reducer.transition.name)
		XRAISE(MAPRED_PARSE_ERROR, 
			   "Duplicate TRANSITION for REDUCER");
	parser->current_obj->u.reducer.transition.name = copyscalar(value);
}

void parser_set_combiner(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_REDUCER);

	if (parser->current_obj->u.reducer.combiner.name)
		XRAISE(MAPRED_PARSE_ERROR, 
			   "Duplicate COMBINER for REDUCER");
	parser->current_obj->u.reducer.combiner.name = copyscalar(value);
}

void parser_set_finalizer(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_REDUCER);

	if (parser->current_obj->u.reducer.finalizer.name)
		XRAISE(MAPRED_PARSE_ERROR, 
			   "Duplicate FINALIZER for REDUCER");
	parser->current_obj->u.reducer.finalizer.name = copyscalar(value);
}

void parser_set_initialize(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_REDUCER);

	if (parser->current_obj->u.reducer.initialize)
		XRAISE(MAPRED_PARSE_ERROR, 
			   "Duplicate INITIALIZE for REDUCER");
	parser->current_obj->u.reducer.initialize = copyscalar(value);
}


void parser_set_language(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);

	/* The function types */
	switch (parser->current_obj->kind)
	{
		case MAPRED_MAPPER:
			if (parser->current_obj->u.function.language)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate LANGUAGE for MAPPER");
			parser->current_obj->u.function.language = copyscalar(value);
			break;

		case MAPRED_TRANSITION:
			if (parser->current_obj->u.function.language)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate LANGUAGE for TRANSITION");
			parser->current_obj->u.function.language = copyscalar(value);
			break;

		case MAPRED_COMBINER:
			if (parser->current_obj->u.function.language)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate LANGUAGE for COMBINER");
			parser->current_obj->u.function.language = copyscalar(value);
			break;

		case MAPRED_FINALIZER:
			if (parser->current_obj->u.function.language)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate LANGUAGE for FINALIZER");
			parser->current_obj->u.function.language = copyscalar(value);
			break;

		default:
			XASSERT(false);
	}
}

void parser_set_function(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);

	/* The function types */
	switch (parser->current_obj->kind)
	{
		case MAPRED_MAPPER:
			if (parser->current_obj->u.function.body)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate FUNCTION for MAPPER");
			parser->current_obj->u.function.body = copyscalar(value);
			break;

		case MAPRED_TRANSITION:
			if (parser->current_obj->u.function.body)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate FUNCTION for TRANSITION");
			parser->current_obj->u.function.body = copyscalar(value);
			break;

		case MAPRED_COMBINER:
			if (parser->current_obj->u.function.body)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate FUNCTION for COMBINER");
			parser->current_obj->u.function.body = copyscalar(value);
			break;

		case MAPRED_FINALIZER:
			if (parser->current_obj->u.function.body)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate FUNCTION for FINALIZER");
			parser->current_obj->u.function.body = copyscalar(value);
			break;

		default:
			XASSERT(false);
	}
}

void parser_set_optimize(mapred_parser_t *parser, char *value)
{
   /* FIXME */
}


void parser_set_source(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);

	switch (parser->current_obj->kind)
	{
		case MAPRED_TASK:
			if (parser->current_obj->u.task.input.name)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate SOURCE for TASK");
			parser->current_obj->u.task.input.name = copyscalar(value);
			break;

		case MAPRED_EXECUTION:
			if (parser->current_obj->u.task.input.name)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate SOURCE for RUN");
			parser->current_obj->u.task.input.name = copyscalar(value);
			break;

		default:
			XASSERT(false);
	}
}

void parser_set_target(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_EXECUTION);

	if (parser->current_obj->u.task.output.name)
		XRAISE(MAPRED_PARSE_ERROR, "Duplicate TARGET for RUN");
	parser->current_obj->u.task.output.name = copyscalar(value);
}

void parser_set_mapper(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);

	switch (parser->current_obj->kind)
	{
		case MAPRED_TASK:
			if (parser->current_obj->u.task.mapper.name)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate MAPPER for TASK");
			parser->current_obj->u.task.mapper.name = copyscalar(value);
			break;

		case MAPRED_EXECUTION:
			if (parser->current_obj->u.task.mapper.name)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate MAPPER for RUN");
			parser->current_obj->u.task.mapper.name = copyscalar(value);
			break;

		default:
			XASSERT(false);
	}
}

void parser_set_reducer(mapred_parser_t *parser, char *value)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);

	switch (parser->current_obj->kind)
	{
		case MAPRED_TASK:
			if (parser->current_obj->u.task.reducer.name)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate REDUCER for TASK");
			parser->current_obj->u.task.reducer.name = copyscalar(value);
			break;

		case MAPRED_EXECUTION:
			if (parser->current_obj->u.task.reducer.name)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate REDUCER for RUN");
			parser->current_obj->u.task.reducer.name = copyscalar(value);
			break;

		default:
			XASSERT(false);
	}
}

/* List functions */
void parser_begin_files(mapred_parser_t *parser)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_INPUT);

	if (parser->current_obj->u.input.type != MAPRED_INPUT_NONE)
	{
		if (parser->current_obj->u.input.type == MAPRED_INPUT_FILE)
			XRAISE(MAPRED_PARSE_ERROR, 
				   "Duplicate FILE for INPUT");
		XRAISE(MAPRED_PARSE_ERROR, 
			   "INPUT may only specify one of FILE, GPFDIST, TABLE, QUERY, EXEC");
	}

	/* files will be added individually */
	parser->current_obj->u.input.type = MAPRED_INPUT_FILE;
}

void parser_begin_gpfdist(mapred_parser_t *parser)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_INPUT);

	if (parser->current_obj->u.input.type != MAPRED_INPUT_NONE)
	{
		if (parser->current_obj->u.input.type == MAPRED_INPUT_GPFDIST)
			XRAISE(MAPRED_PARSE_ERROR, 
				   "Duplicate GPFDIST for INPUT");
		XRAISE(MAPRED_PARSE_ERROR, 
			   "INPUT may only specify one of FILE, GPFDIST, TABLE, QUERY, EXEC");
	}
	parser->current_obj->u.input.type = MAPRED_INPUT_GPFDIST;
}

void parser_begin_columns(mapred_parser_t *parser)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_INPUT);
	if (parser->current_obj->u.input.columns)
		XRAISE(MAPRED_PARSE_ERROR, 
			   "Duplicate COLUMNS for INPUT");
}

void parser_begin_parameters(mapred_parser_t *parser)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	switch (parser->current_obj->kind)
	{
		case MAPRED_MAPPER:
			if (parser->current_obj->u.function.parameters)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate PARAMETERS for MAPPER");
			break;

		case MAPRED_TRANSITION:
			if (parser->current_obj->u.function.parameters)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate PARAMETERS for TRANSITION");
			break;

		case MAPRED_COMBINER:
			if (parser->current_obj->u.function.parameters)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate PARAMETERS for COMBINER");
			break;

		case MAPRED_FINALIZER:
			if (parser->current_obj->u.function.parameters)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate PARAMETERS for FINALIZER");
			break;

		default:
			XASSERT(false);
	}
}

void parser_begin_returns(mapred_parser_t *parser)
{
	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	switch (parser->current_obj->kind)
	{
		case MAPRED_MAPPER:
			if (parser->current_obj->u.function.returns)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate RETURNS for MAPPER");
			break;

		case MAPRED_TRANSITION:
			if (parser->current_obj->u.function.returns)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate RETURNS for TRANSITION");
			break;

		case MAPRED_COMBINER:
			if (parser->current_obj->u.function.returns)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate RETURNS for COMBINER");
			break;

		case MAPRED_FINALIZER:
			if (parser->current_obj->u.function.returns)
				XRAISE(MAPRED_PARSE_ERROR, 
					   "Duplicate RETURNS for FINALIZER");
			break;

		default:
			XASSERT(false);
	}
}

void parser_add_file(mapred_parser_t *parser, char *value)
{
	mapred_clist_t *newitem;
	mapred_clist_t *clist;

	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_INPUT);
	XASSERT(parser->current_obj->u.input.type == MAPRED_INPUT_FILE ||
			parser->current_obj->u.input.type == MAPRED_INPUT_GPFDIST);

	/* Verify the new file */
	/* FIXME */

	/* Allocate the new list item */
	newitem = malloc(sizeof(mapred_clist_t));
	newitem->value = copyscalar(value);
	newitem->next  = (mapred_clist_t *) NULL;

	/* Add the new item into the last slot of the list */
	clist = parser->current_obj->u.input.files;
	while (clist && clist->next)
		clist = clist->next;
	if (clist)
		clist->next = newitem;
	else
		parser->current_obj->u.input.files = newitem;
}

void parser_add_column(mapred_parser_t *parser, char *value)
{
	mapred_plist_t *newitem;
	mapred_plist_t *plist;
	char           *name, *type, *tokenizer;

	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_INPUT);

	/* 
	 * Verify the new column
	 * It should be in one of two forms:
	 *    1)   <name>
	 *    2)   <name> <datatype>
	 */
	name = strtok_r(value, " \t\r", &tokenizer);
	type = strtok_r(NULL, " \t\r", &tokenizer);
	if (!type)
		type = "text";  /* type defaults to 'text' */
	
    /* double check that there's nothing else */
	if (strtok_r(NULL, " \t\r", &tokenizer))
		XRAISE(MAPRED_PARSE_ERROR, 
			   "Invalid Column specification");

	/* Allocate the new list item */
	newitem = malloc(sizeof(mapred_plist_t));
	newitem->name = copyscalar(name);
	newitem->type = copyscalar(type);
	newitem->next  = (mapred_plist_t *) NULL;

	/* Add the new item into the last slot of the list */
	plist = parser->current_obj->u.input.columns;
	while (plist && plist->next)
		plist = plist->next;
	if (plist)
		plist->next = newitem;
	else
		parser->current_obj->u.input.columns = newitem;
}

void parser_add_parameter(mapred_parser_t *parser, char *value)
{
	mapred_plist_t *newitem;
	mapred_plist_t *plist;
	char           *name, *type, *tokenizer;

	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_MAPPER     ||
			parser->current_obj->kind == MAPRED_TRANSITION ||
			parser->current_obj->kind == MAPRED_COMBINER   ||
			parser->current_obj->kind == MAPRED_FINALIZER);

	/* 
	 * Verify the new parameter
	 * It should be in one of two forms:
	 *    1)   <name>
	 *    2)   <name> <datatype>
	 */
	name = strtok_r(value, " \t\r", &tokenizer);
	type = strtok_r(NULL, " \t\r", &tokenizer);
	if (!type)
		type = "text";  /* type defaults to 'text' */
	
    /* double check that there's nothing else */
	if (strtok_r(NULL, " \t\r", &tokenizer))
		XRAISE(MAPRED_PARSE_ERROR, 
			   "Invalid Parameter specification");

	/* Allocate the new list item */
	newitem = malloc(sizeof(mapred_plist_t));
	newitem->name = copyscalar(name);
	newitem->type = copyscalar(type);
	newitem->next  = (mapred_plist_t *) NULL;

	/* Add the new item into the last slot of the list */
	plist = parser->current_obj->u.function.parameters;
	while (plist && plist->next)
		plist = plist->next;
	if (plist)
		plist->next = newitem;
	else
		parser->current_obj->u.function.parameters = newitem;
}

void parser_add_return(mapred_parser_t *parser, char *value)
{
	mapred_plist_t *newitem;
	mapred_plist_t *plist;
	char           *name, *type, *tokenizer;

	XASSERT(parser->current_doc);
	XASSERT(parser->current_obj);
	XASSERT(parser->current_obj->kind == MAPRED_MAPPER     ||
			parser->current_obj->kind == MAPRED_TRANSITION ||
			parser->current_obj->kind == MAPRED_COMBINER   ||
			parser->current_obj->kind == MAPRED_FINALIZER);

	/* 
	 * Verify the new return
	 * It should be in one of two forms:
	 *    1)   <name>
	 *    2)   <name> <datatype>
	 */
	name = strtok_r(value, " \t\r", &tokenizer);
	type = strtok_r(NULL, " \t\r", &tokenizer);
	if (!type)
		type = "text";  /* type defaults to 'text' */
	
    /* double check that there's nothing else */
	if (strtok_r(NULL, " \t\r", &tokenizer))
		XRAISE(MAPRED_PARSE_ERROR, 
			   "Invalid Return specification");

	/* Allocate the new list item */
	newitem = malloc(sizeof(mapred_plist_t));
	newitem->name = copyscalar(name);
	newitem->type = copyscalar(type);
	newitem->next  = (mapred_plist_t *) NULL;

	/* Add the new item into the last slot of the list */
	plist = parser->current_obj->u.function.returns;
	while (plist && plist->next)
		plist = plist->next;
	if (plist)
		plist->next = newitem;
	else
		parser->current_obj->u.function.returns = newitem;
}



/*
 * mapred_dump_yaml - Given an object, dump it's YAML representation.
 *   This is the inverse of parsing
 *
 *   (*) Could be re-written to avoid code duplication issues.
 */
void mapred_dump_yaml(mapred_object_t *obj)
{
	char *ckind = NULL;

	if (!obj)
		return;

	switch (obj->kind)
	{
		case MAPRED_DOCUMENT:
			printf("---\n");
			/* Dumping the current version */
			printf("VERSION:          1.0.0.1\n");  
			if (obj->u.document.database)
				printf("DATABASE:         %s\n", obj->u.document.database);
			if (obj->u.document.user)
				printf("USER:             %s\n", obj->u.document.user);
			if (obj->u.document.host)
				printf("HOST:             %s\n", obj->u.document.host);
			if (obj->u.document.port > 0)
				printf("PORT:             %d\n", obj->u.document.port);
			if (obj->u.document.flags & mapred_document_defines)
			{
				mapred_olist_t *sub;
				printf("DEFINE:\n");
				for (sub = obj->u.document.objects; sub; sub = sub->next)
					if (sub->object->kind != MAPRED_EXECUTION)
						mapred_dump_yaml(sub->object);
			}
			if (obj->u.document.flags & mapred_document_executes)
			{
				mapred_olist_t *sub;
				printf("EXECUTE:\n");
				for (sub = obj->u.document.objects; sub; sub = sub->next)
					if (sub->object->kind == MAPRED_EXECUTION)
						mapred_dump_yaml(sub->object);
			}
			break;

		case MAPRED_INPUT:
			printf("  - INPUT:\n");
			if (obj->name)
				printf("      NAME:       %s\n", obj->name);
			if (obj->u.input.columns)
			{
				mapred_plist_t *plist;
				printf("      COLUMNS:\n");
				for (plist = obj->u.input.columns; plist; plist = plist->next)
					printf("        - %s %s\n", plist->name, plist->type);
			}
			if (obj->u.input.delimiter)
				printf("      DELIMITER:  %s\n", obj->u.input.delimiter);
			if (obj->u.input.encoding)
				printf("      ENCODING:   %s\n", obj->u.input.encoding);
			switch (obj->u.input.format)
			{
				case MAPRED_FORMAT_NONE:
					break;
				case MAPRED_FORMAT_TEXT:
					printf("      FORMAT:     TEXT\n");
					break;
				case MAPRED_FORMAT_CSV:
					printf("      FORMAT:     CSV\n");
					break;
				default:
					XASSERT(false);
			}
			switch (obj->u.input.type)
			{
				case MAPRED_INPUT_NONE:
					break;
				case MAPRED_INPUT_FILE:
				{
					mapred_clist_t *clist;
					printf("      FILE:\n");
					for (clist = obj->u.input.files; clist; clist = clist->next)
						printf("        - %s\n", clist->value);
					break;
				}
				case MAPRED_INPUT_GPFDIST:
					printf("      GPFDIST:    %s\n", obj->u.input.desc);
					break;
				case MAPRED_INPUT_TABLE:
					printf("      TABLE:      %s\n", obj->u.input.desc);
					break;
				case MAPRED_INPUT_QUERY:
					printf("      QUERY: |\n");
					printf("         %s\n", obj->u.input.desc);
					break;
				case MAPRED_INPUT_EXEC:
					printf("      EXEC:       %s\n", obj->u.input.desc);
					break;
				default:
					XASSERT(false);
			}
			break;

		case MAPRED_OUTPUT:
			printf("  - OUTPUT:\n");
			if (obj->name)
				printf("      NAME:       %s\n", obj->name);
			switch (obj->u.output.mode)
			{
				case MAPRED_OUTPUT_NO_MODE:
					break;
				case MAPRED_OUTPUT_REPLACE:
					printf("      MODE:       REPLACE\n");
					break;
				case MAPRED_OUTPUT_APPEND:
					printf("      MODE:       APPEND\n");
					break;
				default:
					XASSERT(false);			
			}
			switch (obj->u.output.type)
			{
				case MAPRED_OUTPUT_NONE:
					break;
				case MAPRED_OUTPUT_FILE:
					printf("      FILE:       %s\n", obj->u.output.desc);
					break;
				case MAPRED_OUTPUT_TABLE:
					printf("      TABLE:      %s\n", obj->u.output.desc);
					break;
				default:
					XASSERT(false);
			}
			break;

		case MAPRED_MAPPER:
			ckind = "MAP";
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

			printf("  - %s:\n", ckind);
			if (obj->name)
				printf("      NAME:       %s\n", obj->name);
			if (obj->u.function.parameters)
			{
				mapred_plist_t *plist;
				printf("      PARAMETERS:\n");
				for (plist = obj->u.function.parameters; plist; 
					 plist = plist->next)
					printf("        - %s %s\n", plist->name, plist->type);
			}
			if (obj->u.function.returns)
			{
				mapred_plist_t *plist;
				printf("      RETURNS:\n");
				for (plist = obj->u.function.returns; plist; 
					 plist = plist->next)
					printf("        - %s %s\n", plist->name, plist->type);
			}
			switch (obj->u.function.mode)
			{
				case MAPRED_NO_MODE:
				case MAPRED_SINGLE_MODE:
					printf("      MODE:       SINGLE\n");
					break;
				case MAPRED_MULTI_MODE:
					printf("      MODE:       MULTI\n");
					break;
				case MAPRED_ACCUMULATED_MODE:
					printf("      MODE:       ACCUMULATED\n");
					break;
				case MAPRED_WINDOWED_MODE:
					printf("      MODE:       WINDOWED\n");
					break;
				default:
					printf("      MODE:       UNKNOWN\n");
					break;
			}
			if (obj->u.function.flags)
			{
				printf("      OPTIMIZE:   ");
				if (obj->u.function.flags & mapred_function_strict)
					printf("STRICT ");
				if (obj->u.function.flags & mapred_function_immutable)
					printf("IMMUTABLE ");
				if (obj->u.function.flags & mapred_function_unordered)
					printf("UNORDERED ");
				printf("\n");
			}
			if (obj->u.function.language)
				printf("      LANGUAGE:   %s\n", obj->u.function.language);
			if (obj->u.function.body)
			{
				printf("      FUNCTION: |\n");
				printf("         %s\n", obj->u.function.body);
			}
			break;

		case MAPRED_REDUCER:
			printf("  - REDUCER:\n");
			if (obj->name)
				printf("      NAME:       %s\n", obj->name);
			if (obj->u.reducer.transition.name)
				printf("      TRANSITION: %s\n", 
					   obj->u.reducer.transition.name);
			if (obj->u.reducer.combiner.name)
				printf("      COMBINER:   %s\n", 
					   obj->u.reducer.combiner.name);
			if (obj->u.reducer.finalizer.name)
				printf("      FINALIZER:  %s\n", 
					   obj->u.reducer.finalizer.name);
			if (obj->u.reducer.initialize)
				printf("      INITIALIZE: %s\n", 
					   obj->u.reducer.initialize);
			break;

		case MAPRED_TASK:
		case MAPRED_EXECUTION:
			if (obj->u.task.execute)
				printf("  - RUN:\n");
			else
				printf("  - TASK:\n");
			if (obj->name)
				printf("      NAME:       %s\n", obj->name);
			if (obj->u.task.input.name)
				printf("      SOURCE:     %s\n", obj->u.task.input.name);
			if (obj->u.task.mapper.name)
				printf("      MAPPER:     %s\n", obj->u.task.mapper.name);
			if (obj->u.task.reducer.name)
				printf("      REDUCER:    %s\n", obj->u.task.reducer.name);
			if (obj->u.task.output.name)
				printf("      TARGET:     %s\n", obj->u.task.output.name);
			break;

		case MAPRED_NO_KIND:
		default:
			XRAISE(MAPRED_PARSE_INTERNAL, 
				   "Unknown object type");
	}
}


	
int mapred_verify_object(mapred_parser_t *parser, mapred_object_t *obj)
{
	char *name, *type;
	int error = NO_ERROR;

	XASSERT(obj);

	/* Verify that all required fields are present and valid */
	name = obj->name ? obj->name : "unnamed";
	type = NULL;
	switch (obj->kind)
	{
		case MAPRED_DOCUMENT:
			if (!obj->u.document.version)
			{
				if (global_verbose_flag)
					fprintf(stderr, "    - ");
				fprintf(stderr,
						"Error: Document #%d Missing VERSION, at line %d\n",
						parser->doc_number, obj->line);
				error = MAPRED_PARSE_ERROR;
			}

			/* FIXME: Check version contents */

			break;

		case MAPRED_INPUT:
			type = "INPUT";

			/* Validate required fields */
			if (!obj->name)
				error = error_missing(type, name, obj->line, "NAME");
			if (obj->u.input.type == MAPRED_INPUT_NONE)
				error = error_missing(type, name, obj->line,
									  "FILE, GPFDIST, TABLE, QUERY, or EXEC");

			/* set default values */
			if (error == NO_ERROR)
			{
				if (!obj->u.input.columns) 
				{
					obj->u.input.columns = malloc(sizeof(mapred_plist_t));
					obj->u.input.columns->name = copyscalar("value");
					obj->u.input.columns->type = copyscalar("text");
					obj->u.input.columns->next = NULL;
				}
			}
			break;

		case MAPRED_OUTPUT:
			type = "OUTPUT";

			if (!obj->name)
				error = error_missing(type, name, obj->line, "NAME");
			if (obj->u.output.type == MAPRED_OUTPUT_NONE)
				error = error_missing(type, name, obj->line, 
									  "FILE or TABLE");
			break;

		case MAPRED_MAPPER:
			type = "MAP";

			if (!obj->name)
				error = error_missing(type, name, obj->line, "NAME");
			if (!obj->u.function.language)
				error = error_missing(type, name, obj->line, "LANGUAGE");
			if (!obj->u.function.body)
				error = error_missing(type, name, obj->line, "FUNCTION");

			/*
			 * Good idea, but perl doesn't support it (yet)
			 */
			/*
			if (obj->u.function.mode == MAPRED_NO_MODE)
				obj->u.function.mode = MAPRED_MULTI_MODE;
			*/

			/* set default values */
			if (error == NO_ERROR)
			{
				if (!obj->u.function.parameters)
				{
					obj->u.function.parameters = malloc(sizeof(mapred_plist_t));
					obj->u.function.parameters->name = copyscalar("key");
					obj->u.function.parameters->type = copyscalar("text");
					obj->u.function.parameters->next = malloc(sizeof(mapred_plist_t));
					obj->u.function.parameters->next->name = copyscalar("value");
					obj->u.function.parameters->next->type = copyscalar("text");
					obj->u.function.parameters->next->next = NULL;
				}
				if (!obj->u.function.returns)
				{
					obj->u.function.returns = malloc(sizeof(mapred_plist_t));
					obj->u.function.returns->name = copyscalar("key");
					obj->u.function.returns->type = copyscalar("text");
					obj->u.function.returns->next = malloc(sizeof(mapred_plist_t));
					obj->u.function.returns->next->name = copyscalar("value");
					obj->u.function.returns->next->type = copyscalar("text");
					obj->u.function.returns->next->next = NULL;
				}
			}
			break;

		case MAPRED_TRANSITION:
			type = "TRANSITION";

			if (!obj->name)
				error = error_missing(type, name, obj->line, "NAME");
			if (!obj->u.function.language)
				error = error_missing(type, name, obj->line, "LANGUAGE");
			if (!obj->u.function.body)
				error = error_missing(type, name, obj->line, "FUNCTION");

			/* set default values */
			if (error == NO_ERROR)
			{
				if (!obj->u.function.parameters)
				{
					obj->u.function.parameters = malloc(sizeof(mapred_plist_t));
					obj->u.function.parameters->name = copyscalar("state");
					obj->u.function.parameters->type = copyscalar("text");
					obj->u.function.parameters->next = malloc(sizeof(mapred_plist_t));
					obj->u.function.parameters->next->name = copyscalar("value");
					obj->u.function.parameters->next->type = copyscalar("text");
					obj->u.function.parameters->next->next = NULL;
				}
				if (!obj->u.function.returns)
				{
					obj->u.function.returns = malloc(sizeof(mapred_plist_t));
					obj->u.function.returns->name = copyscalar("value");
					obj->u.function.returns->type = copyscalar("text");
					obj->u.function.returns->next = NULL;
				}
			}
			break;

		case MAPRED_COMBINER:
			type = "COMBINER";

			if (!obj->name)
				error = error_missing(type, name, obj->line, "NAME");
			if (!obj->u.function.language)
				error = error_missing(type, name, obj->line, "LANGUAGE");
			if (!obj->u.function.body)
				error = error_missing(type, name, obj->line, "FUNCTION");

			/* set default values */
			if (error == NO_ERROR)
			{
				if (!obj->u.function.parameters)
				{
					obj->u.function.parameters = malloc(sizeof(mapred_plist_t));
					obj->u.function.parameters->name = copyscalar("state1");
					obj->u.function.parameters->type = copyscalar("text");
					obj->u.function.parameters->next = malloc(sizeof(mapred_plist_t));
					obj->u.function.parameters->next->name = copyscalar("state2");
					obj->u.function.parameters->next->type = copyscalar("text");
					obj->u.function.parameters->next->next = NULL;
				}
				if (!obj->u.function.returns)
				{
					obj->u.function.returns = malloc(sizeof(mapred_plist_t));
					obj->u.function.returns->name = copyscalar("value");
					obj->u.function.returns->type = copyscalar("text");
					obj->u.function.returns->next = NULL;
				}
			}
			break;

		case MAPRED_FINALIZER:
			type = "FINALIZER";

			if (!obj->name)
				error = error_missing(type, name, obj->line, "NAME");
			if (!obj->u.function.language)
				error = error_missing(type, name, obj->line, "LANGUAGE");
			if (!obj->u.function.body)
				error = error_missing(type, name, obj->line, "FUNCTION");

			/* set default values */
			if (error == NO_ERROR)
			{
				if (!obj->u.function.parameters)
				{
					obj->u.function.parameters = malloc(sizeof(mapred_plist_t));
					obj->u.function.parameters->name = copyscalar("state");
					obj->u.function.parameters->type = copyscalar("text");
					obj->u.function.parameters->next = NULL;
				}
				if (!obj->u.function.returns)
				{
					obj->u.function.returns = malloc(sizeof(mapred_plist_t));
					obj->u.function.returns->name = copyscalar("value");
					obj->u.function.returns->type = copyscalar("text");
					obj->u.function.returns->next = NULL;
				}
			}
			break;

		case MAPRED_REDUCER:
			type = "REDUCER";

			if (!obj->name)
				error = error_missing(type, name, obj->line, "NAME");
			if (!obj->u.reducer.transition.name)
				error = error_missing(type, name, obj->line, "TRANSITION");
			/* 
			 * Will verify that functions are valid for reducer input after we
			 * have resolved the pointers.
			 */
			break;

		case MAPRED_TASK:
			type = "TASK";
			if (!obj->name)
				error = error_missing(type, name, obj->line, "NAME");

			/* Fallthrough */

		case MAPRED_EXECUTION:
			if (!type)
				type = "RUN";
			
			if (!obj->u.task.input.name)
				error = error_missing(type, name, obj->line, "SOURCE");
			
			/* IDENTITY Mappers and Reducers */
			if (obj->u.task.mapper.name && 
				!strcasecmp("IDENTITY", obj->u.task.mapper.name))
			{
				free(obj->u.task.mapper.name);
				obj->u.task.mapper.name = NULL;
			}
			if (obj->u.task.reducer.name && 
				!strcasecmp("IDENTITY", obj->u.task.reducer.name))
			{
				free(obj->u.task.reducer.name);
				obj->u.task.reducer.name = NULL;
			}

			/* STDOUT Output */
			if (obj->u.task.output.name && 
				!strcasecmp("STDOUT", obj->u.task.output.name))
			{
				free(obj->u.task.output.name);
				obj->u.task.output.name = NULL;
			}
			break;

		case MAPRED_NO_KIND:
		default:
			XASSERT(false);
	}

	return error;
}



