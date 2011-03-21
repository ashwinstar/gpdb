/* A Bison parser, made by GNU Bison 2.0.  */

/* Skeleton parser for Yacc-like parsing with Bison,
   Copyright (C) 1984, 1989, 1990, 2000, 2001, 2002, 2003, 2004 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */

/* As a special exception, when this file is copied by Bison into a
   Bison output file, you may use that output file without restriction.
   This special exception was added by the Free Software Foundation
   in version 1.24 of Bison.  */

/* Tokens.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
   /* Put the tokens into the symbol table, so that GDB and other debuggers
      know about them.  */
   enum yytokentype {
     X_IDENT = 258,
     X_NCONST = 259,
     X_SCONST = 260,
     X_OP = 261,
     X_PARAM = 262,
     X_COMMENT = 263,
     X_WHITESPACE = 264,
     X_KEYWORD = 265,
     X_OTHERS = 266,
     X_TYPECAST = 267
   };
#endif
#define X_IDENT 258
#define X_NCONST 259
#define X_SCONST 260
#define X_OP 261
#define X_PARAM 262
#define X_COMMENT 263
#define X_WHITESPACE 264
#define X_KEYWORD 265
#define X_OTHERS 266
#define X_TYPECAST 267




#if ! defined (YYSTYPE) && ! defined (YYSTYPE_IS_DECLARED)
#line 58 "sqlparse.y"
typedef union YYSTYPE {
    int 	ival;
    orafce_lexnode	*node;
    List		*list;
    struct
    {
	    char 	*str;
	    int		keycode;
	    int		lloc;
	    char	*sep;
	    char *modificator;
    }				val;


} YYSTYPE;
/* Line 1274 of yacc.c.  */
#line 77 "sqlparse.h"
# define yystype YYSTYPE /* obsolescent; will be withdrawn */
# define YYSTYPE_IS_DECLARED 1
# define YYSTYPE_IS_TRIVIAL 1
#endif

extern YYSTYPE orafce_sql_yylval;

#if ! defined (YYLTYPE) && ! defined (YYLTYPE_IS_DECLARED)
typedef struct YYLTYPE
{
  int first_line;
  int first_column;
  int last_line;
  int last_column;
} YYLTYPE;
# define yyltype YYLTYPE /* obsolescent; will be withdrawn */
# define YYLTYPE_IS_DECLARED 1
# define YYLTYPE_IS_TRIVIAL 1
#endif

extern YYLTYPE orafce_sql_yylloc;


