#include <postgres_fe.h>
#include <libpq-fe.h>

#include <mapred.h>
#include <except.h>

#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>

ALLOW_EXCEPTIONS;

int             global_print_flag   = 0;
int             global_explain_flag = 0;
int             global_verbose_flag = 0;
int             global_debug_flag   = 0;
mapred_plist_t *global_plist        = NULL;


static char *wordchars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";

void usage(char *procname)
{
	fprintf(stderr, "Usage: %s [options] file.yml\n", procname);
	exit(0);
}

int
main (int argc, char *argv[])
{
	int                i, j;
	mapred_olist_t    *documents;
	mapred_olist_t    *doc_item;
	char              *filename;
	FILE              *file;

	/* 
	 * We accept any "--key=value" input, and there is no way to directly
	 * tell getopt_long about supporting arbitrary 'options', so unfortunately 
	 * the option parsing must be handled manually...
	 */
	filename = NULL;
	for (i = 1; i < argc; i++)
	{
		if (argv[i][0] != '-')
		{
			/* If it doesn't begin with a dash then it should be the filename */
			if (filename)
				usage(argv[0]);
			filename = argv[i];
		}
		else if (argv[i][1] == '\0')
		{
			/* 
			 * An empty dash should imply reading the yml from stdin.
			 * This is not currently supported.
			 */
			usage(argv[0]);
		}
		else if (argv[i][1] != '-')
		{
			/* 
			 * option is a short option
			 */
			for (j = 1; j < strlen(argv[i]); j++)
			{
				switch (argv[i][j])
				{
					case 'p':
						global_print_flag = 1;
						break;
					case 'e':
						global_explain_flag = 1;
						break;
					case 'v':
						global_verbose_flag = 1;
						break;
					case 'd':
						global_debug_flag = 1;
						break;
					default:
						fprintf(stderr, "bad parameter -%c\n", argv[i][j]);
						usage(argv[0]);  /* unknown option */
				}
			}
		} 
		else
		{
			/* option is a long option */
			char *name  = argv[i]+2;
			char *value = NULL;
			char *eq    = strchr(name, '=');

			if (eq)
			{
				eq[0] = '\0';
				value = eq+1;
			}
			else
			{
				/* 
				 * If there isn't an '=' then the NEXT parameter should be the 
				 * value to use for this parameter
				 */
				i++;
				if (i == argc || argv[i][0] == '-')
				{
					fprintf(stderr, "no value specified for --%s\n", name);
					usage(argv[0]);
				}
				value = argv[i];
			}

			/* 
			 * value can be any string, and is thus guaranteed valid.
			 * name can only contain word characters
			 */
			if (strspn(name, wordchars) != strlen(name))
			{
				fprintf(stderr, "bad parameter --%s\n", name, value);
				usage(argv[0]);
			}
			else
			{
				/* Add the parameter to the global parameter list */
				mapred_plist_t *newitem = malloc(sizeof(mapred_plist_t));
				newitem->name  = name;
				newitem->type  = value;
				newitem->next  = global_plist;
				global_plist   = newitem;
			}
		}
	}

	/* open the file */
	if (!filename)
		usage(argv[0]);
	file     = fopen(filename, "rb");
	if (!file) 
	{
		fprintf(stderr, "Error: Could not open file '%s'\n", filename);
		exit(1);
	}

	if (global_verbose_flag)
		fprintf(stderr, "- Parsing '%s':\n", filename);

	documents = NULL;
	XTRY
	{
		documents = mapred_parse_file(file);
	}
	XCATCH(ASSERTION_FAILURE)
	{
		fprintf(stderr, "Assertion failure at %s:%d\n",
				xframe.file, xframe.lineno);	
	}
	XCATCH_ANY
	{
		if (xframe.exception)
			fprintf(stderr, "Error (%d): %s\n", 
					xframe.errcode, xframe.exception);
		else
			fprintf(stderr, "Unknown Error (%d) at %s:%d\n", 
					xframe.errcode, xframe.file, xframe.lineno);
	}
	XTRY_END;

	/* Do something interesting with documents */
	for (doc_item = documents; doc_item; doc_item = doc_item->next)
	{
		PGconn   *conn;
		char     *pwd, *port, *options, *tty;

		XTRY
		{
			mapred_document_t *doc = &doc_item->object->u.document;

			port = NULL; //doc->port;  /* fixme */

			/* FIXME: get password */
			pwd     = (char *) NULL;
			options = (char *) NULL;
			tty     = (char *) NULL;

			conn = PQsetdbLogin(doc->host, port, options,tty, 
								doc->database, doc->user, pwd);
			if (PQstatus(conn) == CONNECTION_BAD)
				fprintf(stderr, "%s: %s", argv[0], PQerrorMessage(conn));
			else
				mapred_run_document(conn, doc);
		}
		XCATCH(ASSERTION_FAILURE)
		{
			fprintf(stderr, "Assertion failure at %s:%d\n",
					xframe.file, xframe.lineno);	
		}
		XCATCH_ANY
		{
			if (xframe.exception)
				fprintf(stderr, "Error (%d): %s\n", 
						xframe.errcode, xframe.exception);
			else
				fprintf(stderr, "Unknown Error (%d) at %s:%d\n", 
						xframe.errcode, xframe.file, xframe.lineno);
		}
		XFINALLY
		{
			PQfinish(conn);
		}
		XTRY_END;
	}
	
	/* Cleanup */
	mapred_destroy_olist(&documents);
	fclose(file);

	return 0;
}

