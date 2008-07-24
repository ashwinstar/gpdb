#include <postgres_fe.h>
#include <libpq-fe.h>

#include <mapred.h>
#include <mapred_errors.h>
#include <except.h>

#include <stdlib.h>
#include <stdio.h>
#include <termios.h>

ALLOW_EXCEPTIONS;

static char  VERSION[] = 
	"Greenplum Map/Reduce Driver 1.00b1";

static char *wordchars = 
	"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";

int             global_print_flag   = 0;
int             global_explain_flag = 0;
int             global_verbose_flag = 0;
int             global_debug_flag   = 0;
mapred_plist_t *global_plist        = NULL;

void usage(char *procname)
{
	fprintf(stderr, "Usage: %s [options] file.yml\n", procname);
	exit(0);
}

void read_password(char *p, size_t len);
void check_version(PGconn *conn);

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
#if INTERNAL_BUILD
					case 'p':
						global_print_flag = 1;
						break;
					case 'e':
						global_explain_flag = 1;
						break;
					case 'd':
						global_debug_flag = 1;
						break;
#endif
					case 'V':  /* Version */
#ifdef PRERELEASE
						fprintf(stderr, "%s (%s Pre-release)\n",
								VERSION, PRERELEASE);
#else
						fprintf(stderr, "%s\n",VERSION);
#endif
						exit(0);
					
					case 'v':  /* verbose */
						global_verbose_flag = 1;
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

#ifdef PRERELEASE
	/* PRE-RELEASE always prints version */
	fprintf(stderr, "%s (%s Pre-release)\n", VERSION, PRERELEASE);
#endif

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
		PGconn   *conn = NULL;
		char      pwdbuf[100];
		char      portbuf[11];  /* max int size should be 10 digits */
		char     *pwd, *port, *options, *tty;
		boolean   needpass;

		XTRY
		{
			mapred_document_t *doc = &doc_item->object->u.document;


			if (doc->port > 0)
			{
				snprintf(portbuf, sizeof(portbuf), "%d", doc->port);
				port = portbuf;
			}
			else
				port = NULL;

			options = NULL;
			tty     = NULL;
			pwd     = NULL;

			do {
				needpass = false;			
				conn = PQsetdbLogin(doc->host, port, options,tty, 
									doc->database, doc->user, pwd);
				if (PQstatus(conn) == CONNECTION_BAD &&
					!strcmp(PQerrorMessage(conn), PQnoPasswordSupplied))
				{
					PQfinish(conn);
					needpass = true;
					read_password(pwdbuf, sizeof(pwdbuf));
					pwd = pwdbuf;
				}
			} while (needpass);

			if (PQstatus(conn) == CONNECTION_BAD)
				fprintf(stderr, PQerrorMessage(conn));
			else
			{
				if (global_verbose_flag)
				{
					fprintf(stderr, "  - Connected Established:\n");
					fprintf(stderr, "    HOST: %s\n", 
							PQhost(conn) ? PQhost(conn) : "localhost");
					fprintf(stderr, "    PORT: %s\n", PQport(conn));
					fprintf(stderr, "    USER: %s/%s\n", PQuser(conn), PQdb(conn));
				}
#ifndef PRERELEASE
				check_version(conn);
#endif
				mapred_run_document(conn, doc);
			}
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




void read_password(char *p, size_t len)
{
    struct termios t_orig, t;
    FILE *termin, *termout;

    termin  = fopen("/dev/tty", "r");
    termout = fopen("/dev/tty", "w");
    if (!termin || !termout) 
    {
        if (termin)
			fclose(termin);
        if (termout)
            fclose(termout);
        termin = stdin;
        termout = stdout;
    }

    tcgetattr(fileno(termin), &t);
    t_orig = t;
    t.c_lflag &= ~ECHO;
    tcsetattr(fileno(termin), TCSAFLUSH, &t);
	
    fputs("Password> ", termout);
    fflush(termout);

    if (fgets(p, len, termin) == NULL)
        p[0] = '\0';
    
    /* If that didn't get the whole input suck the rest off */
    len = strlen(p);
    if (len > 0)
    {
        if (p[len-1] == '\n')
            p[len-1] = '\0';
        else
        {
            char c[25];
            do {
                if (fgets(c, sizeof(c), termin) == NULL)
                    break;
                len = strlen(c);
            } while (len > 0 && c[len-1] != '\n');
        }
    }

    tcsetattr(fileno(termin), TCSAFLUSH, &t_orig);
    fputs("\n", termout);
    fflush(termout);

    if (termin != stdin)
    {
        fclose(termin);
        fclose(termout);
    }
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


