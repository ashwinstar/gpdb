#ifndef PARSER_H
#define PARSER_H

#ifndef MAPRED_H
#include <mapred.h>
#endif
#ifndef YAML_H
#include <yaml.h>
#endif


typedef enum {
	STATE_BOOTSTRAP = 0, 
	STATE_YAML_PARSE,
	STATE_SCALAR_LEX,
	STATE_DONE,
} mapred_parser_state_t;

typedef enum {
	CONTEXT_NONE = 0,
	CONTEXT_HASH_KEY,
	CONTEXT_HASH_VALUE,
	CONTEXT_LIST,
} mapred_parser_context_t;

/*
 * Current max context depth = 5
 *   -DOCUMENT HASH:
 *     - DEFINE LIST:
 *       - ITEM HASH:
 *         - ATTR HASH:
 *           - VALUE LIST
 */
#define MAX_CONTEXT_DEPTH 5

typedef struct mapred_parser_ {
	mapred_parser_state_t     state;
	mapred_parser_context_t   context[MAX_CONTEXT_DEPTH];
	int                       frame;
	yaml_parser_t            *yparser;
	void                     *yscanner;
	void                     *yscan_buffer;
	mapred_olist_t           *doclist;
	mapred_object_t          *current_doc;
	mapred_object_t          *current_obj;
	int                       doc_number;
} mapred_parser_t;

/* Given a yaml parser return a list of parsed documents */
mapred_olist_t* mapred_parse_yaml(yaml_parser_t *parser);

/* object functions */
void parser_begin_document(mapred_parser_t *parser);
void   parser_begin_define(mapred_parser_t *parser);
void  parser_begin_execute(mapred_parser_t *parser);

void   parser_add_document(mapred_parser_t *parser);
void     parser_add_object(mapred_parser_t *parser, mapred_kind_t kind);
void        parser_add_run(mapred_parser_t *parser);

/* list functions */
void      parser_begin_files(mapred_parser_t *parser);
void    parser_begin_columns(mapred_parser_t *parser);
void parser_begin_parameters(mapred_parser_t *parser);
void    parser_begin_returns(mapred_parser_t *parser);

void       parser_add_file(mapred_parser_t *parser, char *value);
void     parser_add_column(mapred_parser_t *parser, char *value);
void  parser_add_parameter(mapred_parser_t *parser, char *value);
void     parser_add_return(mapred_parser_t *parser, char *value);

/* scalar functions */
void    parser_set_version(mapred_parser_t *parser, char *value);
void   parser_set_database(mapred_parser_t *parser, char *value);
void       parser_set_user(mapred_parser_t *parser, char *value);
void       parser_set_host(mapred_parser_t *parser, char *value);
void       parser_set_port(mapred_parser_t *parser, char *value);
void       parser_set_name(mapred_parser_t *parser, char *value);
void      parser_set_table(mapred_parser_t *parser, char *value);
void      parser_set_query(mapred_parser_t *parser, char *value);
void    parser_set_gpfdist(mapred_parser_t *parser, char *value);
void       parser_set_exec(mapred_parser_t *parser, char *value);
void     parser_set_format(mapred_parser_t *parser, char *value);
void       parser_set_null(mapred_parser_t *parser, char *value);
void      parser_set_quote(mapred_parser_t *parser, char *value);
void  parser_set_delimiter(mapred_parser_t *parser, char *value);
void   parser_set_encoding(mapred_parser_t *parser, char *value);
void       parser_set_mode(mapred_parser_t *parser, char *value);
void       parser_set_file(mapred_parser_t *parser, char *value);
void parser_set_transition(mapred_parser_t *parser, char *value);
void   parser_set_combiner(mapred_parser_t *parser, char *value);
void  parser_set_finalizer(mapred_parser_t *parser, char *value);
void parser_set_initialize(mapred_parser_t *parser, char *value);
void   parser_set_language(mapred_parser_t *parser, char *value);
void   parser_set_function(mapred_parser_t *parser, char *value);
void   parser_set_optimize(mapred_parser_t *parser, char *value);
void     parser_set_source(mapred_parser_t *parser, char *value);
void     parser_set_target(mapred_parser_t *parser, char *value);
void     parser_set_mapper(mapred_parser_t *parser, char *value);
void    parser_set_reducer(mapred_parser_t *parser, char *value);

#endif
