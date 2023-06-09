/***** tl_spin: tl_lex.c *****/

/*
 * This file is part of the public release of Spin. It is subject to the
 * terms in the LICENSE file that is included in this source directory.
 * Tool documentation is available at http://spinroot.com
 *
 * Based on the translation algorithm by Gerth, Peled, Vardi, and Wolper,
 * presented at the PSTV Conference, held in 1995, Warsaw, Poland 1995.
 */

#include "../helpers/helpers.hpp"
#include "../utils/utils.hpp"
#include "tl.hpp"
#include <ctype.h>
#include <stdlib.h>

static Symbol *symtab[Nhash + 1];
static int tl_lex(void);
extern int tl_peek(int);

extern YYSTYPE tl_yylval;
extern std::string yytext;

#define Token(y)                                                               \
  tl_yylval = tl_nn(y, ZN, ZN);                                                \
  return y

static void tl_getword(int first, int (*tst)(int)) {
  int i = 0;
  int c;
  yytext = "";
  yytext.push_back((char)first);

  c = tl_Getchar();
  while (c != -1 && tst(c)) {
    yytext.push_back((char)c);
    c = tl_Getchar();
  }

  tl_UnGetchar();
}

static int tl_follow(int tok, int ifyes, int ifno) {
  int c;
  char buf[32];
  extern int tl_yychar;

  if ((c = tl_Getchar()) == tok)
    return ifyes;
  tl_UnGetchar();
  tl_yychar = c;
  sprintf(buf, "expected '%c'", tok);
  tl_yyerror(buf); /* no return from here */
  return ifno;
}

int tl_yylex(void) {
  int c = tl_lex();
  return c;
}

static int is_predicate(int z) {
  char c, c_prev = z;
  char want = (z == '{') ? '}' : ')';
  int i = 0, j, nesting = 0;
  char peek_buf[512];

  c = tl_peek(i++); /* look ahead without changing position */
  while ((c != want || nesting > 0) && c != -1 && i < 2047) {
    if (islower((int)c) || c == '_') {
      peek_buf[0] = c;
      j = 1;
      while (j < (int)sizeof(peek_buf) &&
             (isalnum((int)(c = tl_peek(i))) || c == '_')) {
        peek_buf[j++] = c;
        i++;
      }
      c = 0; /* make sure we don't match on z or want on the peekahead */
      if (j >= (int)sizeof(peek_buf)) {
        peek_buf[j - 1] = '\0';
        loger::fatal("name '%s' in ltl formula too long", peek_buf);
      }
      peek_buf[j] = '\0';
      if (strcmp(peek_buf, "always") == 0 ||
          strcmp(peek_buf, "equivalent") == 0 ||
          strcmp(peek_buf, "eventually") == 0 ||
          strcmp(peek_buf, "until") == 0 || strcmp(peek_buf, "next") == 0 ||
          strcmp(peek_buf, "c_expr") == 0) {
        return 0;
      }
    } else {
      int c_nxt = tl_peek(i);
      if (((c == 'U' || c == 'V' || c == 'X') && !helpers::isalnum_(c_prev) &&
           (c_nxt == -1 || !helpers::isalnum_(c_nxt))) ||
          (c == '<' && c_nxt == '>') || (c == '<' && c_nxt == '-') ||
          (c == '-' && c_nxt == '>') || (c == '[' && c_nxt == ']')) {
        return 0;
      }
    }

    if (c == z) {
      nesting++;
    }
    if (c == want) {
      nesting--;
    }
    c_prev = c;
    c = tl_peek(i++);
  }
  return 1;
}

static void read_upto_closing(int z) {
  char c, want = (z == '{') ? '}' : ')';
  int i = 0, nesting = 0;

  c = tl_Getchar();
  yytext="";
  while ((c != want || nesting > 0) && c != -1 && i < 2047) /* yytext is 2048 */
  {
    yytext.push_back(c);
    if (c == z) {
      nesting++;
    }
    if (c == want) {
      nesting--;
    }
    c = tl_Getchar();
  }
}

static int tl_lex(void) {
  int c;

  do {
    c = tl_Getchar();
    yytext = "";
    yytext.push_back((char)c);

    if (c <= 0) {
      Token(';');
    }

  } while (c == ' '); /* '\t' is removed in tl_main.c */

  if (c == '{' || c == '(') /* new 6.0.0 */
  {
    if (is_predicate(c)) {
      read_upto_closing(c);
      tl_yylval = tl_nn(PREDICATE, ZN, ZN);
      if (!tl_yylval) {
        loger::fatal("unexpected error 4");
      }
      tl_yylval->symbol = tl_lookup(yytext.c_str());
      return PREDICATE;
    }
  }

  if (c == '}') {
    tl_yyerror("unexpected '}'");
  }
  if (islower(c)) {
    tl_getword(c, helpers::isalnum_);
    if ("true" == yytext) {
      Token(TRUE);
    }
    if ("false" == yytext) {
      Token(FALSE);
    }
    if ("always" == yytext) {
      Token(ALWAYS);
    }
    if ("eventually" == yytext) {
      Token(EVENTUALLY);
    }
    if ("until" == yytext) {
      Token(U_OPER);
    }
#ifdef NXT
    if ("next" == yytext) {
      Token(NEXT);
    }
#endif
    if ("c_expr" == yytext) {
      Token(CEXPR);
    }
    if ("not" == yytext) {
      Token(NOT);
    }
    tl_yylval = tl_nn(PREDICATE, ZN, ZN);
    if (!tl_yylval) {
      loger::fatal("unexpected error 5");
    }
    tl_yylval->symbol = tl_lookup(yytext.c_str());
    return PREDICATE;
  }

  if (c == '<') {
    c = tl_Getchar();
    if (c == '>') {
      Token(EVENTUALLY);
    }
    if (c != '-') {
      tl_UnGetchar();
      tl_yyerror("expected '<>' or '<->'");
    }
    c = tl_Getchar();
    if (c == '>') {
      Token(EQUIV);
    }
    tl_UnGetchar();
    tl_yyerror("expected '<->'");
  }

  switch (c) {
  case '/':
    c = tl_follow('\\', AND, '/');
    break;
  case '\\':
    c = tl_follow('/', OR, '\\');
    break;
  case '&':
    c = tl_follow('&', AND, '&');
    break;
  case '|':
    c = tl_follow('|', OR, '|');
    break;
  case '[':
    c = tl_follow(']', ALWAYS, '[');
    break;
  case '-':
    c = tl_follow('>', IMPLIES, '-');
    break;
  case '!':
    c = NOT;
    break;
  case 'U':
    c = U_OPER;
    break;
  case 'V':
    c = V_OPER;
    break;
#ifdef NXT
  case 'X':
    c = NEXT;
    break;
#endif
  default:
    break;
  }
  Token(c);
}

Symbol *tl_lookup(const char *s) {
  Symbol *sp;
  unsigned int h = utils::hash(s);

  for (sp = symtab[h]; sp; sp = sp->next)
    if (strcmp(sp->name, s) == 0)
      return sp;

  sp = (Symbol *)tl_emalloc(sizeof(Symbol));
  sp->name = (char *)tl_emalloc((int)strlen(s) + 1);
  strcpy(sp->name, s);
  sp->next = symtab[h];
  symtab[h] = sp;

  return sp;
}

Symbol *getsym(Symbol *s) {
  Symbol *n = (Symbol *)tl_emalloc(sizeof(Symbol));

  n->name = s->name;
  return n;
}
