%output="yaml_parse.c"
%name-prefix="yaml_yy"
%pure-parser                             /* Because global variables are bad */
%error-verbose                           /* A little extra debugging info */

%{
  #include <yaml_parse.h>
  #include <yaml.h>
  #include <parser.h>

/*
 * Ancient flex versions, like the ones on our build machines don't support
 * flex .h file generation.
 */
#if USE_FLEX_HFILE
# include <yaml_scan.h>
#endif

  #include <stdio.h>

  int  yaml_yylex(YYSTYPE *lvalp, mapred_parser_t *parser);
  void yaml_yyerror(mapred_parser_t *parser, char const *);
%}
%parse-param {mapred_parser_t *parser}   /* So we can pass it to the lexer */
%lex-param   {mapred_parser_t *parser}

%union {
  int                    keyword;
  char                  *token;
}

/* Keyword tokens */
%token <keyword>
  _COLUMNS_     _CONSOLIDATE_
  _DATABASE_    _DEFINE_      _DELIMITER_
  _ENCODING_    _ERROR_LIMIT_ _ESCAPE_      _EXEC_        _EXECUTE_
  _FILE_        _FINALIZE_    _FORMAT_      _FUNCTION_
  _GPFDIST_
  _HOST_
  _INITIALIZE_  _INPUT_
  _LANGUAGE_
  _MAP_         _MODE_
  _NAME_        _NULL_
  _OPTIMIZE_    _OUTPUT_
  _PARAMETERS_  _PORT_ 
  _QUERY_       _QUOTE_
  _REDUCE_      _RETURNS_     _RUN_
  _SOURCE_
  _TABLE_       _TARGET_      _TASK_        _TRANSITION_ 
  _USER_
  _VERSION_

 /* Non-keyword scalar tokens */
%token <token> _INTEGER_
%token <token> _VERSION_STRING_
%token <token> _ID_
%token <token> _STRING_


/* YAML State tokens */
%token START_STREAM   END_STREAM
%token START_DOCUMENT END_DOCUMENT
%token START_LIST     END_LIST
%token START_MAP      END_MAP
%type  <token>        scalar

%% /* Grammar rules and actions follow */


stream:
    START_STREAM 
    document_list 
    END_STREAM
    | error END_STREAM             /* Fallback for error recovery */
    ;

document_list: 
    document
    | document_list document
    ;

document:  
    START_DOCUMENT                 { parser_begin_document(parser); }
    START_MAP
    doc_item_list
    END_MAP
    END_DOCUMENT
    | error END_DOCUMENT           /* Fallback for error recovery */
    ;

doc_item_list:
    doc_item
    | doc_item_list doc_item

doc_item:
    _VERSION_  _VERSION_STRING_   { parser_set_version(parser, $2); }
    | _DATABASE_ _ID_             { parser_set_database(parser, $2); }
    | _USER_     _ID_             { parser_set_user(parser, $2); }
    | _HOST_     scalar           { parser_set_host(parser, $2); }
    | _PORT_     _INTEGER_        { parser_set_port(parser, $2); }
    | _DEFINE_  START_LIST        { parser_begin_define(parser); }
       define_list  END_LIST
    | _EXECUTE_ START_LIST        { parser_begin_execute(parser); }
      execute_list END_LIST
    ;

execute_list:
      /* empty */
      | execute_list  START_MAP run_item END_MAP 
       ;

define_list:
      /* empty */
      | define_list   START_MAP define_item END_MAP
      ;

run_item:
        _RUN_        { parser_add_run(parser); }        run_map
      ;

define_item:
        _INPUT_      { parser_add_object(parser, MAPRED_INPUT); } 
        input_map
      | _OUTPUT_     { parser_add_object(parser, MAPRED_OUTPUT); } 
        output_map
      | _REDUCE_     { parser_add_object(parser, MAPRED_REDUCER); } 
        reduce_map 
      | _MAP_        { parser_add_object(parser, MAPRED_MAPPER); } 
        function_map
      | _TRANSITION_ { parser_add_object(parser, MAPRED_TRANSITION); } 
        function_map
      | _CONSOLIDATE_ { parser_add_object(parser, MAPRED_COMBINER); } 
        function_map
      | _FINALIZE_   { parser_add_object(parser, MAPRED_FINALIZER); } 
        function_map
      | _TASK_       { parser_add_object(parser, MAPRED_TASK); } 
        task_map
	  | _STRING_     {
                         char buffer[128];
			             snprintf(buffer, 40, "%s is not a valid object type", $1);
			             yaml_yyerror(parser, buffer); 
						 YYERROR;
                     }
      ;

input_map:        START_MAP input_item_list    END_MAP ;
output_map:       START_MAP output_item_list   END_MAP ;
reduce_map:       START_MAP reduce_item_list   END_MAP ;
function_map:     START_MAP function_item_list END_MAP ;
task_map:         START_MAP task_item_list     END_MAP ;
run_map:          START_MAP run_item_list      END_MAP ;

input_item_list:       input_item | input_item_list    input_item ;
output_item_list:     output_item | output_item_list   output_item ;
reduce_item_list:     reduce_item | reduce_item_list   reduce_item ;
function_item_list: function_item | function_item_list function_item ;
task_item_list:         task_item | task_item_list     task_item ;
run_item_list:           run_item | run_item_list      run_item ;

input_item:
        _NAME_  _ID_          { parser_set_name(parser, $2); }
      | _TABLE_ _ID_          { parser_set_table(parser, $2); }
      | _QUERY_ scalar        { parser_set_query(parser, $2); }
      | _FILE_                { parser_begin_files(parser); } 
           file_list
      | _GPFDIST_             { parser_begin_gpfdist(parser); }
           file_list
      | _EXEC_ scalar         { parser_set_exec(parser, $2); }
      | _COLUMNS_             { parser_begin_columns(parser); }
           column_list
      | _FORMAT_ scalar       { parser_set_format(parser, $2); }
      | _DELIMITER_ scalar    { parser_set_delimiter(parser, $2); }
      | _NULL_ scalar         { parser_set_null(parser, $2); }
      | _QUOTE_ scalar        { parser_set_quote(parser, $2); }
      | _ESCAPE_ scalar       { parser_set_escape(parser, $2); }
      | _ENCODING_ scalar     { parser_set_encoding(parser, $2); }
      | _ERROR_LIMIT_ _INTEGER_ { parser_set_error_limit(parser, $2); }
	  | _STRING_     {
                         char buffer[128];
			             snprintf(buffer, 128, "%s is not a valid INPUT attribute", $1);
			             yaml_yyerror(parser, buffer); 
						 mapred_destroy_object(&parser->current_obj);
						 YYERROR;
                     }
      ;

output_item:
      _NAME_  _ID_            { parser_set_name(parser, $2); }
      | _FILE_ scalar         { parser_set_file(parser, $2); }
      | _TABLE_ scalar        { parser_set_table(parser, $2); }
      | _FORMAT_ scalar       { parser_set_format(parser, $2); }
      | _DELIMITER_ scalar    { parser_set_delimiter(parser, $2); }
      | _MODE_ scalar         { parser_set_mode(parser, $2); }
	  | _STRING_     {
                         char buffer[128];
			             snprintf(buffer, 128, "%s is not a valid OUTPUT attribute", $1);
			             yaml_yyerror(parser, buffer);
						 mapred_destroy_object(&parser->current_obj);
						 YYERROR;
                     }
      ;

reduce_item:
        _NAME_       _ID_     { parser_set_name(parser, $2); }
      | _TRANSITION_ _ID_     { parser_set_transition(parser, $2); }
      | _CONSOLIDATE_ _ID_     { parser_set_combiner(parser, $2); }
      | _FINALIZE_   _ID_     { parser_set_finalizer(parser, $2); }
      | _INITIALIZE_ scalar   { parser_set_initialize(parser, $2); }
	  | _STRING_     {
                         char buffer[128];
			             snprintf(buffer, 128, "%s is not a valid REDUCER attribute", $1);
			             yaml_yyerror(parser, buffer); 
						 mapred_destroy_object(&parser->current_obj);
						 YYERROR;
                     }
      ;

function_item:
       _NAME_ _ID_            { parser_set_name(parser, $2); }
      | _LANGUAGE_ scalar     { parser_set_language(parser, $2); }
      | _FUNCTION_ scalar     { parser_set_function(parser, $2); }
      | _MODE_ scalar         { parser_set_mode(parser, $2); }
      | _OPTIMIZE_ scalar     { parser_set_optimize(parser, $2); }
      | _PARAMETERS_          { parser_begin_parameters(parser); } 
           parameter_list
      | _RETURNS_             { parser_begin_returns(parser); }
           return_list
	  | _STRING_     {
                         char buffer[128];
			             snprintf(buffer, 128, "%s is not a valid function attribute", $1);
			             yaml_yyerror(parser, buffer); 
						 mapred_destroy_object(&parser->current_obj);
						 YYERROR;
                     }
      ;

task_item:
      _NAME_      _ID_        { parser_set_name(parser, $2); }
      | _SOURCE_  _ID_        { parser_set_source(parser, $2); }
      | _MAP_     _ID_        { parser_set_mapper(parser, $2); }
      | _REDUCE_  _ID_        { parser_set_reducer(parser, $2); }
	  | _STRING_     {
                         char buffer[128];
			             snprintf(buffer, 128, "%s is not a valid TASK attribute", $1);
			             yaml_yyerror(parser, buffer); 
						 mapred_destroy_object(&parser->current_obj);
						 YYERROR;
                     }
      ;

run_item:
      _NAME_      _ID_        { parser_set_name(parser, $2); }
      | _SOURCE_  _ID_        { parser_set_source(parser, $2); }
      | _TARGET_  _ID_        { parser_set_target(parser, $2); }
      | _MAP_     _ID_        { parser_set_mapper(parser, $2); }
      | _REDUCE_  _ID_        { parser_set_reducer(parser, $2); }
	  | _STRING_     {
                         char buffer[128];
			             snprintf(buffer, 128, "%s is not a valid RUN attribute", $1);
			             yaml_yyerror(parser, buffer); 
						 mapred_destroy_object(&parser->current_obj);
						 YYERROR;
                     }
      ;


/* Could probably make these more generic */
file_list:
        scalar                { parser_add_file(parser, $1); }
      | START_LIST file_list_2 END_LIST
      ;

file_list_2:
      /* empty */
      | file_list_2 scalar    { parser_add_file(parser, $2); }
      ;

column_list:
        scalar                { parser_add_column(parser, $1); }
      | START_LIST column_list_2 END_LIST
      ;

column_list_2:
      /* empty */
      | column_list_2 scalar  { parser_add_column(parser, $2); }
      ;

parameter_list:
        scalar                { parser_add_parameter(parser, $1); }
      | START_LIST parameter_list_2 END_LIST
      ;

parameter_list_2:
      /* empty */
      | parameter_list_2 scalar { parser_add_parameter(parser, $2); }
      ;


return_list:
        scalar                { parser_add_return(parser, $1); }
      | START_LIST return_list_2 END_LIST
	  ;

return_list_2:
      /* empty */
      | return_list_2 scalar    { parser_add_return(parser, $2); }
      ;

scalar:
      _STRING_
	  | _INTEGER_
	  | _ID_
	  | _VERSION_STRING_
	  ;


/*
valid_yaml:
     scalar
	 | START_LIST valid_yaml_list END_LIST
     | START_MAP  valid_yaml_map  END_MAP
	 ;

valid_yaml_list:
	| valid_yaml_list valid_yaml
	;

valid_yaml_map:
    | valid_yaml_map valid_yaml valid_yaml
    | valid_yaml_map keyword valid_yaml
	;

keyword:
  _COLUMNS_|_COMBINER_|_DATABASE_|_DEFINE_|_DELIMITER_|_EXECUTE_|
  _FILE_|_FINALIZER_|_FORMAT_|_FUNCTION_|_GPFDIST_|_HOST_|
  _INITIALIZE_|_INPUT_|_LANGUAGE_|_MAP_|_MAPPER_|_MODE_|_NAME_|
  _OPTIMIZE_|_OUTPUT_|_PARAMETERS_|_PORT_|_QUERY_|_REDUCER_|_RETURNS_|
  _RUN_|_SOURCE_|_TABLE_|_TARGET_|_TASK_|_TRANSITION_|_USER_|_VERSION_
  ;
*/


%%



/* Called by yyparse on error.  */
void yaml_yyerror (mapred_parser_t *parser, char const *s)
{
	if (parser->current_doc)
	{
		if (global_verbose_flag)
			fprintf(stderr, "    - ");
		parser->current_doc->u.document.flags |= 
			mapred_document_error;
	}
	else
	{
		if (global_verbose_flag)
			fprintf(stderr, "  - ");
	}
	fprintf(stderr, "Error: %s\n", s);
}

#if 0
#define DEBUG_TOKEN(x) printf("%s\n", x)
#else
#define DEBUG_TOKEN(x) do { } while (0)
#endif
#if 0
#define DEBUG_YYTOKEN(x) printf("  YYTOKEN=%d\n", x)
#else
#define DEBUG_YYTOKEN(x) do { } while (0)
#endif


/* 
 * int yamllex(lval, parser)
 *
 * We use a crazy mix of the yaml parse library and flex/bison to build
 * our parser.  
 * 
 * The YAML library handles all of the whitespace and flow parsing very
 * cleanly, but it has no domain knowledge of the Greenplum Mapreduce
 * YAML Schema.
 *
 * Coding up the YAML flow stuff in flex/bison is a pain
 *
 * So... we put the parse first through the YAML library parser and use
 * that as our first pass tokenizer to handle all of the YAML document flow
 * proccessing.  
 *
 * If it was just that simple then we could feed these tokens into bison and 
 * be done, but we also want additional lexical analysis on the scalar values 
 * so we feed them back into a flex tokenizer.
 *
 */
int
yaml_yylex (YYSTYPE *lvalp, mapred_parser_t *parser)
{
	yaml_event_t event;

	if (parser->state == STATE_DONE)
		return 0;

	if (parser->state == STATE_SCALAR_LEX)
	{
		int token;
#if USE_FLEX_REENTRANT
		token = yaml_scalar_yylex(parser->yscanner, parser);
#else
		token = yaml_scalar_yylex(parser);
#endif
		if (token)
		{
			DEBUG_YYTOKEN(token);
			return token;
		}
		else
		{
#if USE_FLEX_REENTRANT
			yaml_scalar_yy_delete_buffer(parser->yscan_buffer, parser->yscanner);
			parser->yscan_buffer = NULL;
#endif
			parser->state = STATE_YAML_PARSE;
		}
	}

	if (parser->state != STATE_YAML_PARSE)
	{
		parser->state = STATE_DONE;
		return END_STREAM;   /* should be an error */
	}

	if (!yaml_parser_parse(parser->yparser, &event))
	{
		parser->state = STATE_DONE;
		return END_STREAM;   /* should be an error */
	}

#if 0
	if (parser->frame < 0)
		printf("no frame\n");
	else if (parser->frame >= MAX_CONTEXT_DEPTH) 
		printf("bad frame\n");
	else 
	{
		switch (parser->context[parser->frame]) 
		{
			case CONTEXT_NONE:
				printf("FRAME [NONE]\n");
				break;

			case CONTEXT_HASH_KEY:
				printf("FRAME [HASH_KEY]\n");
				break;
				
			case CONTEXT_HASH_VALUE:
				printf("FRAME [HASH_VALUE]\n");
				break;

			case CONTEXT_LIST:
				printf("FRAME [LIST]\n");
				break;
				
			default:
				printf("FRAME [BAD FRAME]\n");
				break;
		}
	}
#endif

	while (1) {
		switch (event.type) {

			case YAML_NO_EVENT:
				parser->state = STATE_DONE;
				return 0;

			case YAML_STREAM_START_EVENT:
				DEBUG_TOKEN("YAML_STREAM_START");
				return START_STREAM;

			case YAML_DOCUMENT_START_EVENT:
				DEBUG_TOKEN("YAML_DOCUMENT_START");
				return START_DOCUMENT;

			case YAML_MAPPING_START_EVENT:
				if (++parser->frame >= MAX_CONTEXT_DEPTH) {
					fprintf(stderr, "Maximum context depth exceded");
					parser->state = STATE_DONE;
					return END_STREAM;
				}
				parser->context[parser->frame] = CONTEXT_HASH_KEY;

				DEBUG_TOKEN("YAML_MAPPING_START");
				return START_MAP;

			case YAML_SEQUENCE_START_EVENT:
				if (++parser->frame >= MAX_CONTEXT_DEPTH) {
					printf("Maximum context depth exceded");
					parser->state = STATE_DONE;
					return END_STREAM;
				}
				parser->context[parser->frame] = CONTEXT_LIST;

				DEBUG_TOKEN("YAML_SEQUENCE_START");
				return START_LIST;

			case YAML_SCALAR_EVENT:
			{
				int   token;
				char *value = (char*) event.data.scalar.value;

				DEBUG_TOKEN("SCALAR:");
				lvalp->token = value;

				/* Switch to the scalar scanner and continue */
				parser->state = STATE_SCALAR_LEX;

#if USE_FLEX_REENTRANT
				parser->yscan_buffer = 
					yaml_scalar_yy_scan_string(value, parser->yscanner);
#else
				yaml_scalar_yy_scan_string(value);
#endif
				token = yaml_yylex(lvalp, parser);

				/* 
				 * If we are in a hash context then we switch between the
				 * key states and the value states.
				 */
				if (parser->frame >= 0 && parser->frame < MAX_CONTEXT_DEPTH)
				{
					switch (parser->context[parser->frame]) 
					{
						case CONTEXT_HASH_KEY:
							parser->context[parser->frame] = CONTEXT_HASH_VALUE;
							break;

						case CONTEXT_HASH_VALUE:
							parser->context[parser->frame] = CONTEXT_HASH_KEY;
							break;

						default:
							break;
					}
				}
				return token;
			}
			
			case YAML_SEQUENCE_END_EVENT:
				if (--parser->frame < -1)
					parser->frame = -1;

				/* 
				 * If the sequence was the value pair of a hash then the next
				 * scalar will be a key 
				 */
				if (parser->frame >= 0 && parser->frame < MAX_CONTEXT_DEPTH &&
					parser->context[parser->frame] == CONTEXT_HASH_VALUE)
					parser->context[parser->frame] = CONTEXT_HASH_KEY;

				DEBUG_TOKEN("YAML_SEQUENCE_END");
				return END_LIST;
				
			case YAML_MAPPING_END_EVENT:
				if (--parser->frame < -1)
					parser->frame = -1;

				/* 
				 * If the mapping was the value pair of a hash then the next
				 * scalar will be a key 
				 */
				if (parser->frame >= 0 && parser->frame < MAX_CONTEXT_DEPTH &&
					parser->context[parser->frame] == CONTEXT_HASH_VALUE)
					parser->context[parser->frame] = CONTEXT_HASH_KEY;

				DEBUG_TOKEN("YAML_MAPPING_END");
				return END_MAP;
				
			case YAML_DOCUMENT_END_EVENT:
				DEBUG_TOKEN("YAML_DOCUMENT_END");
				return END_DOCUMENT;
				
			case YAML_STREAM_END_EVENT:
				DEBUG_TOKEN("YAML_STREAM_END");
				return END_STREAM;
				
			default:
				printf("WARNING: Unknown event %d\n", event.type);
		}
		
		yaml_event_delete(&event);
	}
}
