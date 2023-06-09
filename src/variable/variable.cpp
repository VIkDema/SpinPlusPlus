#include "variable.hpp"

/***** spin: vars.c *****/

#include "../fatal/fatal.hpp"
#include "../lexer/line_number.hpp"
#include "../main/launch_settings.hpp"
#include "../models/lextok.hpp"
#include "../models/symbol.hpp"
#include "../run/flow.hpp"
#include "../run/run.hpp"
#include "../run/sched.hpp"
#include "../spin.hpp"
#include "../structs/structs.hpp"
#include "../symbol/symbol.hpp"
#include "../trail/mesg.hpp"
#include "../utils/verbose/verbose.hpp"
#include "y.tab.h"
#include <fmt/core.h>
#include <iostream>

extern LaunchSettings launch_settings;

extern char GBuf[];
extern int nproc, nstop;
extern int depth, limited_vis, Pid_nr;
extern models::Lextok *Xu_List;
extern models::Ordered *all_names;
extern models::RunList *X_lst, *LastX;
extern short no_arrays, Have_claim, terse;
extern models::Symbol *Fname;


static int maxcolnr = 1;

namespace variable {
namespace {

int GetGlobal(models::Lextok *sn) {
  models::Symbol *s = sn->symbol;
  int i, n = run::Eval(sn->left);

  if (s->type == 0 && X_lst &&
      (i = flow::FindLabel(s, X_lst->n, 0))) /* getglobal */
  {
    std::cout << fmt::format("findlab through getglobal on {}", s->name)
              << std::endl;
    return i; /* can this happen? */
  }
  if (s->type == STRUCT) {
    return structs::Rval_struct(sn, s, 1); /* 1 = check init */
  }
  if (CheckVar(s, n)) {
    return CastValue(s->type, s->value[n], (int)s->nbits.value_or(0));
  }
  return 0;
}

int SetGlobal(models::Lextok *v, int m) {
  if (v->symbol->type == STRUCT) {
    structs::Lval_struct(v, v->symbol, 1, m);
  } else {
    int n = run::Eval(v->left);
    if (CheckVar(v->symbol, n)) {
      int oval = v->symbol->value[n];
      int nval =
          CastValue((int)v->symbol->type, m, v->symbol->nbits.value_or(0));
      v->symbol->value[n] = nval;
      if (oval != nval) {
        v->symbol->last_depth = depth;
      }
    }
  }
  return 1;
}

void RemoveSelfRefs(models::Symbol *s, models::Lextok *i) {
  if (!i)
    return;

  if (i->node_type == NAME && i->symbol->name == s->name &&
      ((!i->symbol->context && !s->context) ||
       (i->symbol->context && s->context &&
        i->symbol->context->name == s->context->name))) {
    file::LineNumber::Set(i->line_number);
    Fname = i->file_name;
    loger::non_fatal("self-reference initializing '%s'", s->name);
    i->node_type = CONST;
    i->value = 0;
  } else {
    RemoveSelfRefs(s, i->left);
    RemoveSelfRefs(s, i->right);
  }
}

} // namespace

int GetValue(models::Lextok *sn) {
  models::Symbol *s = sn->symbol;

  if (s->name == "_") {
    loger::non_fatal("attempt to read value of '_'");
    return 0;
  }
  if (s->name == "_last")
    return (LastX) ? LastX->pid : 0;
  if (s->name == "_p")
    return (X_lst && X_lst->pc) ? X_lst->pc->seqno : 0;
  if (s->name == "_pid") {
    if (!X_lst)
      return 0;
    return X_lst->pid - Have_claim;
  }
  if (s->name == "_priority") {
    if (!X_lst)
      return 0;

    if (launch_settings.need_revert_old_rultes_for_priority) {
      loger::non_fatal("cannot refer to _priority with -o6");
      return 1;
    }
    return X_lst->priority;
  }

  if (s->name == "_nr_pr") {
    return nproc - nstop; /* new 3.3.10 */
  }

  if (s->context && s->type) {
    return sched::GetLocalValue(sn);
  }

  if (!s->type) /* not declared locally */
  {
    s = models::Symbol::BuildOrFind(s->name); /* try global */
    sn->symbol = s;                           /* fix it */
  }

  return GetGlobal(sn);
}

int SetVal(models::Lextok *v, int n) {
  if (v->symbol->name == "_last" || v->symbol->name == "_p" ||
      v->symbol->name == "_pid" || v->symbol->name == "_nr_qs" ||
      v->symbol->name == "_nr_pr") {
    loger::non_fatal("illegal assignment to %s", v->symbol->name.c_str());
  }
  if (v->symbol->name == "_priority") {
    if (launch_settings.need_revert_old_rultes_for_priority) {
      loger::non_fatal("cannot refer to _priority with -o6");
      return 1;
    }
    if (!X_lst) {
      loger::non_fatal("no context for _priority");
      return 1;
    }
    X_lst->priority = n;
  }

  if (v->symbol->context && v->symbol->type) {
    return sched::AssignLocalValue(v, n);
  }
  if (!v->symbol->type) {
    v->symbol = models::Symbol::BuildOrFind(v->symbol->name);
  }
  return SetGlobal(v, n);
}

int CheckVar(models::Symbol *s, int n) {
  int i, oln = file::LineNumber::Get(); /* calls on run::Eval() change it */
  models::Symbol *ofnm = Fname;
  models::Lextok *z, *y;

  if (!sched::IsIndexInBounds(s, n))
    return 0;

  if (s->type == 0) {
    loger::non_fatal("undecl var %s (assuming int)", s->name.c_str());
    s->type = models::kInt;
  }
  /* not a STRUCT */
  if (s->value.empty()) /* uninitialized */
  {
    s->value.resize(s->value_type);
    z = s->init_value;
    for (i = 0; i < s->value_type; i++) {
      if (z && z->node_type == ',') {
        y = z->left;
        z = z->right;
      } else {
        y = z;
      }
      if (s->type != models::kChan) {
        RemoveSelfRefs(s, y);
        s->value[i] = run::Eval(y);
      } else if (!launch_settings.need_to_analyze) {
        s->value[i] = mesg::QMake(s);
      }
    }
  }
  file::LineNumber::Set(oln);
  Fname = ofnm;

  return 1;
}

int CastValue(int t, int v, int w) {
  int i = 0;
  short s = 0;
  unsigned int u = 0;

  if (t == PREDEF || t == INT || t == CHAN)
    i = v; /* predef means _ */
  else if (t == SHORT)
    s = (short)v;
  else if (t == BYTE || t == MTYPE)
    u = (unsigned char)v;
  else if (t == BIT)
    u = (unsigned char)(v & 1);
  else if (t == UNSIGNED) {
    if (w == 0)
      loger::fatal("cannot happen, cast_val");
    /*	u = (unsigned)(v& ((1<<w)-1));		problem when w=32	*/
    u = (unsigned)(v & (~0u >> (8 * sizeof(unsigned) - w))); /* doug */
  }

  if (v != i + s + (int)u) {
    char buf[64];
    sprintf(buf, "%d->%d (%d)", v, i + s + (int)u, t);
    loger::non_fatal("value (%s) truncated in assignment", buf);
  }
  return (int)(i + s + (int)u);
}

void DumpClaims(FILE *fd, int pid, const std::string &s) {
  models::Lextok *m;
  int cnt = 0;
  int oPid = Pid_nr;

  for (m = Xu_List; m; m = m->right)
    if (m->symbol->name == s) {
      cnt = 1;
      break;
    }
  if (cnt == 0)
    return;

  Pid_nr = pid;
  fprintf(fd, "#ifndef XUSAFE\n");
  for (m = Xu_List; m; m = m->right) {
    if (m->symbol->name != s)
      continue;
    no_arrays = 1;
    putname(fd, "\t\tsetq_claim(", m->left, 0, "");
    no_arrays = 0;
    fprintf(fd, ", %d, ", m->value);
    terse = 1;
    putname(fd, "\"", m->left, 0, "\", h, ");
    terse = 0;
    fprintf(fd, "\"%s\");\n", s.c_str());
  }
  fprintf(fd, "#endif\n");
  Pid_nr = oPid;
}

void DumpGlobals(void) {
  models::Ordered *walk;
  static models::Lextok *dummy = ZN;
  models::Symbol *sp;
  int j;
  auto &verbose_flags = utils::verbose::Flags::getInstance();
  if (!dummy)
    dummy =
        models::Lextok::nn(ZN, NAME, models::Lextok::nn(ZN, CONST, ZN, ZN), ZN);

  for (walk = all_names; walk; walk = walk->next) {
    sp = walk->entry;
    if (!sp->type || sp->context || sp->owner_name || sp->type == PROCTYPE ||
        sp->type == PREDEF || sp->type == CODE_FRAG || sp->type == CODE_DECL ||
        (sp->type == MTYPE && symbol::IsMtype(sp->name)))
      continue;

    if (sp->type == STRUCT) {
      if (verbose_flags.NeedToPrintAllProcessActions() &&
          !verbose_flags.NeedToPrintVeryVerbose() &&
          (sp->last_depth < depth &&
           launch_settings.count_of_skipping_steps != depth)) {
        continue;
      }
      structs::DumpStruct(sp, sp->name, 0);
      continue;
    }
    for (j = 0; j < sp->value_type; j++) {
      int prefetch;
      std::string s;
      if (sp->type == CHAN) {
        mesg::PrintQueueContents(sp, j, 0);
        continue;
      }
      if (verbose_flags.NeedToPrintAllProcessActions() &&
          !verbose_flags.NeedToPrintVeryVerbose() &&
          (sp->last_depth < depth &&
           launch_settings.count_of_skipping_steps != depth)) {
        continue;
      }

      dummy->symbol = sp;
      dummy->left->value = j;
      /* in case of cast_val warnings, do this first: */
      prefetch = GetGlobal(dummy);
      printf("\t\t%s", sp->name.c_str());
      if (sp->value_type > 1 || sp->is_array)
        printf("[%d]", j);
      printf(" = ");
      if (sp->type == MTYPE && sp->mtype_name) {
        s = sp->mtype_name->name;
      }
      mesg::PrintFormattedMessage(stdout, prefetch, sp->type == MTYPE, s);
      printf("\n");
      if (limited_vis && (sp->hidden_flags & 2)) {
        GBuf[0] = '\0';
        sprintf(GBuf, "%s = ", sp->name.c_str());
        mesg::FormatMessage(prefetch, sp->type == MTYPE, s);
        if (sp->color_number == 0) {
          sp->color_number = (unsigned char)maxcolnr;
          maxcolnr = 1 + (maxcolnr % 10);
        }
        printf("\t\t%s\n", GBuf);
        continue;
      }
    }
  }
}

void DumpLocal(models::RunList *r, int final) {
  static models::Lextok *dummy = ZN;
  models::Symbol *z, *s;
  int i;
  auto &verbose_flags = utils::verbose::Flags::getInstance();

  if (!r)
    return;

  s = r->symtab;

  if (!dummy) {
    dummy =
        models::Lextok::nn(ZN, NAME, models::Lextok::nn(ZN, CONST, ZN, ZN), ZN);
  }

  for (z = s; z; z = z->next) {
    if (z->type == STRUCT) {
      structs::DumpStruct(z, z->name, r);
      continue;
    }
    for (i = 0; i < z->value_type; i++) {
      std::string t;
      if (z->type == CHAN) {
        mesg::PrintQueueContents(z, i, r);
        continue;
      }

      if (verbose_flags.NeedToPrintAllProcessActions() &&
          !verbose_flags.NeedToPrintVeryVerbose() && !final &&
          (z->last_depth < depth &&
           launch_settings.count_of_skipping_steps != depth)) {
        continue;
      }

      dummy->symbol = z;
      dummy->left->value = i;

      printf("\t\t%s(%d):%s", r->n->name.c_str(), r->pid - Have_claim,
             z->name.c_str());
      if (z->value_type > 1 || z->is_array)
        printf("[%d]", i);
      printf(" = ");

      if (z->type == MTYPE && z->mtype_name) {
        t = z->mtype_name->name;
      }
      mesg::PrintFormattedMessage(stdout, GetValue(dummy), z->type == MTYPE, t);
      printf("\n");
      if (limited_vis && (z->hidden_flags & 2)) {
        int colpos;
        GBuf[0] = '\0';
        sprintf(GBuf, "%s(%d):%s = ", r->n->name.c_str(), r->pid,
                z->name.c_str());
         mesg::FormatMessage(GetValue(dummy), z->type == MTYPE, t);
        if (z->color_number == 0) {
          z->color_number = (unsigned char)maxcolnr;
          maxcolnr = 1 + (maxcolnr % 10);
        }
        colpos = nproc + z->color_number - 1;
        printf("\t\t%s\n", GBuf);
        continue;
      }
    }
  }
}
} // namespace variable
