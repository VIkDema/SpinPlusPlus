#include "dstep.hpp"

#include "../fatal/fatal.hpp"
#include "../lexer/line_number.hpp"
#include "../run/flow.hpp"
#include "../main/launch_settings.hpp"
#include "y.tab.h"
#include <assert.h>
#include <iomanip>
#include <sstream>

extern LaunchSettings launch_settings;
extern models::Symbol *Fname;
extern FILE *fd_th;
constexpr int kMaxDstep = 2048;

std::string NextLab[64]; /* must match value in pangen2.c:41 */
int Level = 0, GenCode = 0, IsGuard = 0, TestOnly = 0;

static int Tj = 0, Jt = 0, LastGoto = 0;
static int Tojump[kMaxDstep], Jumpto[kMaxDstep], Special[kMaxDstep];

extern int Pid_nr, OkBreak;

namespace dstep {
namespace {
void Sourced(int n, int special) {
  int i;
  for (i = 0; i < Tj; i++)
    if (Tojump[i] == n)
      return;
  if (Tj >= kMaxDstep)
    loger::fatal("d_step sequence too long");
  Special[Tj] = special;
  Tojump[Tj++] = n;
}

void Dested(int n) {
  int i;
  for (i = 0; i < Tj; i++)
    if (Tojump[i] == n)
      return;
  for (i = 0; i < Jt; i++)
    if (Jumpto[i] == n)
      return;
  if (Jt >= kMaxDstep)
    loger::fatal("d_step sequence too long");
  Jumpto[Jt++] = n;
  LastGoto = 1;
}

void Mopup(FILE *fd) {
  int i, j;

  for (i = 0; i < Jt; i++) {
    for (j = 0; j < Tj; j++)
      if (Tojump[j] == Jumpto[i])
        break;
    if (j == Tj) {
      char buf[16];
      if (Jumpto[i] == OkBreak) {
        if (!LastGoto)
          fprintf(fd, "S_%.3d_0:	/* break-dest */\n", OkBreak);
      } else {
        sprintf(buf, "S_%.3d_0", Jumpto[i]);
        loger::non_fatal("goto %s breaks from d_step seq", buf);
      }
    }
  }
  for (j = 0; j < Tj; j++) {
    for (i = 0; i < Jt; i++)
      if (Tojump[j] == Jumpto[i])
        break;
  }
  for (j = i = 0; j < Tj; j++)
    if (Special[j]) {
      if (i >= kMaxDstep) {
        loger::fatal("cannot happen (dstep.c)");
      }
      Tojump[i] = Tojump[j];
      Special[i] = 2;
      i++;
    }
  Tj = i; /* keep only the global exit-labels */
  Jt = 0;
}

int FirstTime(int n) {
  int i;
  for (i = 0; i < Tj; i++)
    if (Tojump[i] == n)
      return (Special[i] <= 1);
  return 1;
}

void illegal(models::Element *e, char *str) {
  printf("illegal operator in 'd_step:' '");
  comment(stdout, e->n, 0);
  printf("'\n");
  loger::fatal("'%s'", str);
}

void filterbad(models::Element *e) {
  switch (e->n->node_type) {
  case ASSERT:
  case PRINT:
  case 'c':
    /* run cannot be completely undone
     * with sv_save-sv_restor
     */
    if (any_oper(e->n->left, RUN))
      illegal(e, "run operator in d_step");

    /* remote refs inside d_step sequences
     * would be okay, but they cannot always
     * be interpreted by the simulator the
     * same as by the verifier (e.g., for an
     * error trail)
     */
    if (any_oper(e->n->left, 'p'))
      illegal(e, "remote reference in d_step");
    break;
  case '@':
    illegal(e, "process termination");
    break;
  case D_STEP:
    illegal(e, "nested d_step sequence");
    break;
  case ATOMIC:
    illegal(e, "nested atomic sequence");
    break;
  default:
    break;
  }
}

int CollectGuards(FILE *fd, models::Element *e, int inh) {
  models::SeqList *h;
  models::Element *ee;

  for (h = e->sub; h; h = h->next) {
    ee = huntstart(h->this_sequence->frst);
    filterbad(ee);
    switch (ee->n->node_type) {
    case NON_ATOMIC:
      inh += CollectGuards(fd, ee->n->seq_list->this_sequence->frst, inh);
      break;
    case IF:
      inh += CollectGuards(fd, ee, inh);
      break;
    case '.':
      if (ee->next->n->node_type == DO)
        inh += CollectGuards(fd, ee->next, inh);
      break;
    case ELSE:
      if (inh++ > 0)
        fprintf(fd, " || ");
      /* 4.2.5 */ if (!pid_is_claim(Pid_nr))
        fprintf(fd, "(boq == -1 /* else */)");
      else
        fprintf(fd, "(1 /* else */)");
      break;
    case 'R':
      if (inh++ > 0)
        fprintf(fd, " || ");
      fprintf(fd, "(");
      TestOnly = 1;
      putstmnt(fd, ee->n, ee->seqno);
      fprintf(fd, ")");
      TestOnly = 0;
      break;
    case 'r':
      if (inh++ > 0)
        fprintf(fd, " || ");
      fprintf(fd, "(");
      TestOnly = 1;
      putstmnt(fd, ee->n, ee->seqno);
      fprintf(fd, ")");
      TestOnly = 0;
      break;
    case 's':
      if (inh++ > 0)
        fprintf(fd, " || ");
      fprintf(fd, "(");
      TestOnly = 1;
      /* 4.2.1 */ if (!pid_is_claim(Pid_nr))
        fprintf(fd, "(boq == -1) && ");
      putstmnt(fd, ee->n, ee->seqno);
      fprintf(fd, ")");
      TestOnly = 0;
      break;
    case 'c':
      if (inh++ > 0)
        fprintf(fd, " || ");
      fprintf(fd, "(");
      TestOnly = 1;
      if (!pid_is_claim(Pid_nr))
        fprintf(fd, "(boq == -1 && ");
      putstmnt(fd, ee->n->left, e->seqno);
      if (!pid_is_claim(Pid_nr))
        fprintf(fd, ")");
      fprintf(fd, ")");
      TestOnly = 0;
      break;
    }
  }
  return inh;
}

void putCode(FILE *fd, models::Element *f, models::Element *last,
             models::Element *next, int isguard) {
  models::Element *e, *N;
  models::SeqList *h;
  int i;
  std::string NextOpt;
  static int bno = 0;

  for (e = f; e; e = e->next) {
    if (e->status & DONE2)
      continue;
    e->status |= DONE2;

    if (!(e->status & D_ATOM)) {
      if (!LastGoto) {
        fprintf(fd, "\t\tgoto S_%.3d_0;\n", e->Seqno);
        Dested(e->Seqno);
      }
      break;
    }
    fprintf(fd, "S_%.3d_0: /* 2 */\n", e->Seqno);
    LastGoto = 0;
    Sourced(e->Seqno, 0);

    if (!e->sub) {
      filterbad(e);
      switch (e->n->node_type) {
      case NON_ATOMIC:
        h = e->n->seq_list;
        putCode(fd, h->this_sequence->frst, h->this_sequence->extent, e->next,
                0);
        break;
      case BREAK:
        if (LastGoto)
          break;
        if (e->next) {
          i = target(huntele(e->next, e->status, -1))->Seqno;
          fprintf(fd, "\t\tgoto S_%.3d_0;	", i);
          fprintf(fd, "/* 'break' */\n");
          Dested(i);
        } else {
          if (next) {
            fprintf(fd, "\t\tgoto S_%.3d_0;", next->Seqno);
            fprintf(fd, " /* NEXT */\n");
            Dested(next->Seqno);
          } else
            loger::fatal("cannot interpret d_step");
        }
        break;
      case GOTO:
        if (LastGoto)
          break;
        i = huntele(flow::GetLabel(e->n, 1), e->status, -1)->Seqno;
        fprintf(fd, "\t\tgoto S_%.3d_0;	", i);
        fprintf(fd, "/* 'goto' */\n");
        Dested(i);
        break;
      case '.':
        if (LastGoto)
          break;
        if (e->next && (e->next->status & DONE2)) {
          i = e->next->Seqno;
          fprintf(fd, "\t\tgoto S_%.3d_0;", i);
          fprintf(fd, " /* '.' */\n");
          Dested(i);
        }
        break;
      default:
        putskip(e->seqno);
        GenCode = 1;
        IsGuard = isguard;
        fprintf(fd, "\t\t");
        putstmnt(fd, e->n, e->seqno);
        fprintf(fd, ";\n");
        GenCode = IsGuard = isguard = LastGoto = 0;
        break;
      }
      i = e->next ? e->next->Seqno : 0;
      if (e->next && (e->next->status & DONE2) && !LastGoto) {
        fprintf(fd, "\t\tgoto S_%.3d_0; ", i);
        fprintf(fd, "/* ';' */\n");
        Dested(i);
        break;
      }
    } else {
      for (h = e->sub, i = 1; h; h = h->next, i++) {
        std::ostringstream oss;
        oss << "goto S_" << std::setfill('0') << std::setw(3) << e->Seqno << "_"
            << i;
        NextOpt = oss.str();
        NextLab[++Level] = NextOpt;
        N = (e->n && e->n->node_type == DO) ? e : e->next;
        putCode(fd, h->this_sequence->frst, h->this_sequence->extent, N, 1);
        Level--;
        fprintf(fd, "%s: /* 3 */\n", &NextOpt[5]);
        LastGoto = 0;
      }
      if (!LastGoto) {
        fprintf(fd, "\t\tUerror(\"blocking sel ");
        fprintf(fd, "in d_step (nr.%d, near line %d)\");\n", bno++,
                (e->n) ? e->n->line_number : 0);
        LastGoto = 0;
      }
    }
    if (e == last) {
      if (!LastGoto && next) {
        fprintf(fd, "\t\tgoto S_%.3d_0;\n", next->Seqno);
        Dested(next->Seqno);
      }
      break;
    }
  }
}
} // namespace

int putcode(FILE *fd, models::Sequence *s, models::Element *next,
            int justguards, int ln, int seqno) {
  int isg = 0;
  static std::string buf;

  NextLab[0] = "continue";
  filterbad(s->frst);

  switch (s->frst->n->node_type) {
  case UNLESS:
    loger::non_fatal("'unless' inside d_step - ignored");
    return putcode(fd, s->frst->n->seq_list->this_sequence, next, 0, ln, seqno);
  case NON_ATOMIC:
    putcode(fd, s->frst->n->seq_list->this_sequence, ZE, 1, ln, seqno);
    if (justguards)
      return 0; /* 6.2.5 */
    break;
  case IF:
    fprintf(fd, "if (!(");
    if (!CollectGuards(fd, s->frst, 0))
      /* what about boq ? */ fprintf(fd, "1");
    fprintf(fd, "))\n\t\t\tcontinue;");
    isg = 1;
    break;
  case '.':
    if (s->frst->next->n->node_type == DO) {
      fprintf(fd, "if (!(");
      if (!CollectGuards(fd, s->frst->next, 0))
        fprintf(fd, "1");
      fprintf(fd, "))\n\t\t\tcontinue;");
      isg = 1;
    }
    break;
  case 'R': /* <- can't really happen (it's part of a 'c')*/
    fprintf(fd, "if (!(");
    TestOnly = 1;
    putstmnt(fd, s->frst->n, s->frst->seqno);
    fprintf(fd, "))\n\t\t\tcontinue;");
    TestOnly = 0;
    break;
  case 'r':
    fprintf(fd, "if (!(");
    TestOnly = 1;
    putstmnt(fd, s->frst->n, s->frst->seqno);
    fprintf(fd, "))\n\t\t\tcontinue;");
    TestOnly = 0;
    break;
  case 's':
    fprintf(fd, "if (");
#if 1
    /* 4.2.1 */
    if (!pid_is_claim(Pid_nr))
      fprintf(fd, "(boq != -1) || ");
#endif
    fprintf(fd, "!(");
    TestOnly = 1;
    putstmnt(fd, s->frst->n, s->frst->seqno);
    fprintf(fd, "))\n\t\t\tcontinue;");
    TestOnly = 0;
    break;
  case 'c':
    fprintf(fd, "if (!(");
    if (!pid_is_claim(Pid_nr))
      fprintf(fd, "boq == -1 && ");
    TestOnly = 1;
    putstmnt(fd, s->frst->n->left, s->frst->seqno);
    fprintf(fd, "))\n\t\t\tcontinue;");
    TestOnly = 0;
    break;
  case ELSE:
    fprintf(fd, "if (boq != -1 || (");
    if (launch_settings.separate_version != 2)
      fprintf(fd, "trpt->");
    fprintf(fd, "o_pm&1))\n\t\t\tcontinue;");
    {
      fprintf(fd_th, "#ifndef ELSE_IN_GUARD\n");
      fprintf(fd_th, " #define ELSE_IN_GUARD\n");
      fprintf(fd_th, "#endif\n");
    }
    break;
  case ASGN: /* new 3.0.8 */
    fprintf(fd, "IfNotBlocked");
    break;
  default:
    fprintf(fd, "/ default %d */\n\t\t", s->frst->n->node_type);
  }

  /* 6.2.5 : before TstOnly */
  fprintf(fd, "\n\n\t\treached[%d][%d] = 1;\n\t\t", Pid_nr, seqno);
  fprintf(fd, "reached[%d][t->st] = 1;\n\t\t", Pid_nr); /* next state*/
  fprintf(fd, "reached[%d][tt] = 1;\n", Pid_nr);        /* current state */

  /* 6.2.5 : before sv_save() */
  if (s->frst->n->node_type != NON_ATOMIC) {
    fprintf(fd,
            "\n\t\tif (TstOnly) return 1;\n"); /* if called from enabled() */
  }
  if (justguards) {
    return 0;
  }
  fprintf(fd, "\n\t\tsv_save();\n\t\t");
  buf = "Uerror(\"block in d_step seq, line " + std::to_string(ln) + "\")";
  NextLab[0] = buf;
  putCode(fd, s->frst, s->extent, next, isg);

  if (next) {

    if (FirstTime(next->Seqno) &&
        (!(next->status & DONE2) || !(next->status & D_ATOM))) {
      fprintf(fd, "S_%.3d_0: /* 1 */\n", next->Seqno);
      next->status |= DONE2;
      LastGoto = 0;
    }
    Sourced(next->Seqno, 1);
    file::LineNumber::Set(ln);
    Fname = next->n->file_name;
    Mopup(fd);
  }
  unskip(s->frst->seqno);
  return LastGoto;
}
} // namespace dstep