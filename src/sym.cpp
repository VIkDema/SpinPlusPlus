/***** spin: sym.c *****/

#include "fatal/fatal.hpp"
#include "lexer/lexer.hpp"
#include "lexer/scope.hpp"
#include "main/launch_settings.hpp"
#include "models/symbol.hpp"
#include "models/access.hpp"
#include "spin.hpp"
#include "utils/verbose/verbose.hpp"
#include "y.tab.h"
#include <iostream>
#include "models/lextok.hpp"

extern LaunchSettings launch_settings;
extern lexer::ScopeProcessor scope_processor_;

extern models::Symbol *Fname, *owner;
extern int lineno, depth, verbose, NamesNotAdded;
extern int has_hidden;
extern short has_xu;

models::Symbol *context = ZS;
models::Ordered *all_names = nullptr;
int Nid_nr = 0;

models::Mtypes_t *Mtypes;
models::Lextok *runstmnts = ZN;

static models::Ordered *last_name = nullptr;
static models::Symbol *symtab[Nhash + 1];

static int samename(models::Symbol *a, models::Symbol *b) {
  if (!a && !b)
    return 1;
  if (!a || !b)
    return 0;
  return a->name != b->name;
}

unsigned int hash(const std::string &s) {
  unsigned int h = 0;

  for (char c : s) {
    h += static_cast<unsigned int>(c);
    h <<= 1;
    if (h & (Nhash + 1))
      h |= 1;
  }
  return h & Nhash;
}

void disambiguate(void) {
  models::Ordered *walk;
  models::Symbol *sp;
  std::string n, m;

  if (launch_settings.need_old_scope_rules) {
    return;
  }
  /* prepend the scope_prefix to the names */

  for (walk = all_names; walk; walk = walk->next) {
    sp = walk->entry;
    if (sp->type != 0 && sp->type != LABEL && sp->block_scope.size() > 1) {
      if (sp->context != nullptr) {
        m = "_" + std::to_string(sp->context->sc) + "_";
        if (m == sp->block_scope)
          continue;
        /* 6.2.0: only prepend scope for inner-blocks,
           not for top-level locals within a proctype
           this means that you can no longer use the same name
           for a global and a (top-level) local variable
         */
      }

      sp->name = sp->block_scope + sp->name; /* discard the old memory */
    }
  }
}

models::Symbol *lookup(const std::string &s) {
  models::Symbol *sp;
  models::Ordered *no;
  unsigned int h = hash(s);

  if (launch_settings.need_old_scope_rules) { /* same scope - global refering to
                            global or local to local */
    for (sp = symtab[h]; sp; sp = sp->next) {
      if (sp->name == s && samename(sp->context, context) &&
          samename(sp->owner_name, owner)) {
        return sp; /* found */
      }
    }
  } else { /* added 6.0.0: more traditional, scope rule */
    for (sp = symtab[h]; sp; sp = sp->next) {
      if (sp->name == s && samename(sp->context, context) &&
          (sp->block_scope == scope_processor_.GetCurrScope() ||
           (sp->block_scope.compare(0, sp->block_scope.length(),
                                    scope_processor_.GetCurrScope()) == 0 &&
            samename(sp->owner_name, owner)))) {
        if (!samename(sp->owner_name, owner)) {
          printf("spin: different container %s\n", sp->name.c_str());
          printf("    old: %s\n",
                 sp->owner_name ? sp->owner_name->name.c_str() : "--");
          printf("    new: %s\n", owner ? owner->name.c_str() : "--");
          /*        MainProcessor::Exit(1);    */
        }
        return sp; /* found */
      }
    }
  }

  if (context) /* in proctype, refers to global */
    for (sp = symtab[h]; sp; sp = sp->next) {
      if (sp->name == s.c_str() && !sp->context &&
          samename(sp->owner_name, owner)) {
        return sp; /* global */
      }
    }

  sp = (models::Symbol *)emalloc(sizeof(models::Symbol));
  sp->name += s;
  sp->value_type = 1;
  sp->last_depth = depth;
  sp->context = context;
  sp->owner_name = owner; /* if fld in struct */
  sp->block_scope = scope_processor_.GetCurrScope();

  if (NamesNotAdded == 0) {
    sp->next = symtab[h];
    symtab[h] = sp;
    no = (models::Ordered *)emalloc(sizeof(models::Ordered));
    no->entry = sp;
    if (!last_name)
      last_name = all_names = no;
    else {
      last_name->next = no;
      last_name = no;
    }
  }

  return sp;
}

void trackvar(models::Lextok *n, models::Lextok *m) {
  models::Symbol *sp = n->symbol;

  if (!sp)
    return; /* a structure list */
  switch (m->node_type) {
  case NAME:
    if (m->symbol->type != BIT) {
      sp->hidden_flags |= 4;
      if (m->symbol->type != models::SymbolType::kByte)
        sp->hidden_flags |= 8;
    }
    break;
  case CONST:
    if (m->value != 0 && m->value != 1)
      sp->hidden_flags |= 4;
    if (m->value < 0 || m->value > 256)
      sp->hidden_flags |= 8; /* ditto byte-equiv */
    break;
  default:                       /* unknown */
    sp->hidden_flags |= (4 | 8); /* not known bit-equiv */
  }
}

void trackrun(models::Lextok *n) { runstmnts = nn(ZN, 0, n, runstmnts); }

void checkrun(models::Symbol *parnm, int posno) {
  models::Lextok *n, *now, *v;
  int i, m;
  int res = 0;
  std::string buf, buf2;
  auto &verbose_flags = utils::verbose::Flags::getInstance();
  for (n = runstmnts; n; n = n->right) {
    now = n->left;
    if (now->symbol != parnm->context)
      continue;
    for (v = now->left, i = 0; v; v = v->right, i++)
      if (i == posno) {
        m = v->left->node_type;
        if (m == CONST) {
          m = v->left->value;
          if (m != 0 && m != 1)
            res |= 4;
          if (m < 0 || m > 256)
            res |= 8;
        } else if (m == NAME) {
          m = v->left->symbol->type;
          if (m != BIT) {
            res |= 4;
            if (m != BYTE)
              res |= 8;
          }
        } else
          res |= (4 | 8); /* unknown */
        break;
      }
  }
  if (!(res & 4) || !(res & 8)) {
    if (!verbose_flags.NeedToPrintVerbose())
      return;
    buf = !(res & 4) ? "bit" : "byte";

    sputtype(buf, parnm->type);
    i = buf.length();
    while (i > 0 && buf[--i] == ' ')
      buf[i] = '\0';
    if (i == 0 || buf == buf2)
      return;
    prehint(parnm);
    printf("proctype %s, '%s %s' could be declared",
           parnm->context ? parnm->context->name.c_str() : "", buf.c_str(),
           parnm->name.c_str());
    printf(" '%s %s'\n", buf2.c_str(), parnm->name.c_str());
  }
}

void trackchanuse(models::Lextok *m, models::Lextok *w, int t) {
  models::Lextok *n = m;
  int count = 1;
  while (n) {
    if (n->left && n->left->symbol && n->left->symbol->type == CHAN)
      setaccess(n->left->symbol, w ? w->symbol : ZS, count, t);
    n = n->right;
    count++;
  }
}

void setptype(models::Lextok *mtype_name, models::Lextok *n, int t,
              models::Lextok *vis) /* predefined types */
{
  int oln = lineno, cnt = 1;
  extern int Expand_Ok;

  while (n) {
    if (n->symbol->type && !(n->symbol->hidden_flags & 32)) {
      lineno = n->line_number;
      Fname = n->file_name;
      loger::fatal("redeclaration of '%s'", n->symbol->name);
      lineno = oln;
    }
    n->symbol->type = (models::SymbolType)t;

    if (mtype_name && t != MTYPE) {
      lineno = n->line_number;
      Fname = n->file_name;
      loger::fatal("missing semi-colon after '%s'?", mtype_name->symbol->name);
      lineno = oln;
    }

    if (mtype_name && n->symbol->mtype_name &&
        mtype_name->symbol->name != n->symbol->mtype_name->name) {
      fprintf(stderr,
              "spin: %s:%d, Error: '%s' is type '%s' but assigned type '%s'\n",
              n->file_name->name.c_str(), n->line_number, n->symbol->name.c_str(),
              mtype_name->symbol->name.c_str(), n->symbol->mtype_name->name.c_str());
      loger::non_fatal("type error");
    }

    n->symbol->mtype_name =
        mtype_name ? mtype_name->symbol : 0; /* if mtype, else 0 */

    if (Expand_Ok) {
      n->symbol->hidden_flags |= (4 | 8 | 16); /* formal par */
      if (t == CHAN)
        setaccess(n->symbol, ZS, cnt, 'F');
    }

    if (t == UNSIGNED) {
      if (!n->symbol->nbits.has_value() || n->symbol->nbits.value() >= 32)
        loger::fatal("(%s) has invalid width-field", n->symbol->name);
      if (n->symbol->nbits.has_value() && n->symbol->nbits.value() == 0) {
        n->symbol->nbits = 16;
        loger::non_fatal("unsigned without width-field");
      }
    } else if (n->symbol->nbits.has_value() && n->symbol->nbits.value() > 0) {
      loger::non_fatal("(%s) only an unsigned can have width-field",
                       n->symbol->name);
    }

    if (vis) {
      std::string name = vis->symbol->name;
      if (name.compare(0, 6, ":hide:") == 0) {
        n->symbol->hidden_flags |= 1;
        has_hidden++;
        if (t == BIT)
          loger::fatal("bit variable (%s) cannot be hidden_flags",
                       n->symbol->name.c_str());
      } else if (name.compare(0, 6, ":show:") == 0) {
        n->symbol->hidden_flags |= 2;
      } else if (name.compare(0, 7, ":local:") == 0) {
        n->symbol->hidden_flags |= 64;
      }
    }

    if (t == CHAN) {
      n->symbol->id = ++Nid_nr;
    } else {
      n->symbol->id = 0;
      if (n->symbol->init_value && n->symbol->init_value->node_type == CHAN) {
        Fname = n->file_name;
        lineno = n->line_number;
        loger::fatal("chan initializer for non-channel %s", n->symbol->name);
      }
    }

    if (n->symbol->value_type <= 0) {
      lineno = n->line_number;
      Fname = n->file_name;
      loger::non_fatal("bad array size for '%s'", n->symbol->name);
      lineno = oln;
    }

    n = n->right;
    cnt++;
  }
}

static void setonexu(models::Symbol *sp, int t) {
  sp->xu |= t;
  if (t == XR || t == XS) {
    if (sp->xup[t - 1] && sp->xup[t - 1]->name != context->name) {
      printf("error: x[rs] claims from %s and %s\n",
             sp->xup[t - 1]->name.c_str(), context->name.c_str());
      loger::non_fatal("conflicting claims on chan '%s'", sp->name.c_str());
    }
    sp->xup[t - 1] = context;
  }
}

static void setallxu(models::Lextok *n, int t) {
  models::Lextok *fp, *tl;

  for (fp = n; fp; fp = fp->right)
    for (tl = fp->left; tl; tl = tl->right) {
      if (tl->symbol->type == STRUCT)
        setallxu(tl->symbol->struct_template, t);
      else if (tl->symbol->type == CHAN)
        setonexu(tl->symbol, t);
    }
}

models::Lextok *Xu_List = (models::Lextok *)0;

void setxus(models::Lextok *p, int t) {
  models::Lextok *m, *n;

  has_xu = 1;

  if (launch_settings.need_lose_msgs_sent_to_full_queues && t == XS) {
    printf(
        "spin: %s:%d, warning, xs tag not compatible with -m (message loss)\n",
        (p->file_name != NULL) ? p->file_name->name.c_str() : "stdin", p->line_number);
  }

  if (!context) {
    lineno = p->line_number;
    Fname = p->file_name;
    loger::fatal("non-local x[rs] assertion");
  }
  for (m = p; m; m = m->right) {
    models::Lextok *Xu_new = (models::Lextok *)emalloc(sizeof(models::Lextok));
    Xu_new->opt_inline_id = p->opt_inline_id;
    Xu_new->value = t;
    Xu_new->left = m->left;
    Xu_new->symbol = context;
    Xu_new->right = Xu_List;
    Xu_List = Xu_new;

    n = m->left;
    if (n->symbol->type == STRUCT)
      setallxu(n->symbol->struct_template, t);
    else if (n->symbol->type == CHAN)
      setonexu(n->symbol, t);
    else {
      int oln = lineno;
      lineno = n->line_number;
      Fname = n->file_name;
      loger::non_fatal("xr or xs of non-chan '%s'", n->symbol->name);
      lineno = oln;
    }
  }
}

models::Lextok **find_mtype_list(const std::string &s) {
  models::Mtypes_t *lst;

  for (lst = Mtypes; lst; lst = lst->next) {
    if (lst->name_of_mtype == s) {
      return &(lst->list_of_names);
    }
  }

  /* not found, create it */
  lst = (models::Mtypes_t *)emalloc(sizeof(models::Mtypes_t));
  lst->name_of_mtype = s;
  lst->next = Mtypes;
  Mtypes = lst;
  return &(lst->list_of_names);
}

void setmtype(models::Lextok *mtype_name, models::Lextok *m) {
  models::Lextok **mtl; /* mtype list */
  models::Lextok *n, *Mtype;
  int cnt, oln = lineno;
  std::string s = "_unnamed_";

  if (m) {
    lineno = m->line_number;
    Fname = m->file_name;
  }

  if (mtype_name && mtype_name->symbol) {
    s = mtype_name->symbol->name;
  }

  mtl = find_mtype_list(s);
  Mtype = *mtl;

  if (!Mtype) {
    *mtl = Mtype = m;
  } else {
    for (n = Mtype; n->right; n = n->right) {
      ;
    }
    n->right = m; /* concatenate */
  }

  for (n = Mtype, cnt = 1; n; n = n->right, cnt++) /* syntax check */
  {
    if (!n->left || !n->left->symbol || n->left->node_type != NAME ||
        n->left->left) /* indexed variable */
      loger::fatal("bad mtype definition");

    /* label the name */
    if (n->left->symbol->type != models::SymbolType::kMtype) {
      n->left->symbol->hidden_flags |= 128; /* is used */
      n->left->symbol->type = models::SymbolType::kMtype;
      n->left->symbol->init_value = nn(ZN, CONST, ZN, ZN);
      n->left->symbol->init_value->value = cnt;
    } else if (n->left->symbol->init_value->value != cnt) {
      loger::non_fatal("name %s appears twice in mtype declaration",
                       n->left->symbol->name);
    }
  }

  lineno = oln;
  if (cnt > 256) {
    loger::fatal("too many mtype elements (>255)");
  }
}

std::string which_mtype(
    const std::string &str) /* which mtype is str, 0 if not an mtype at all  */
{
  models::Mtypes_t *lst;
  models::Lextok *n;

  for (lst = Mtypes; lst; lst = lst->next) {
    for (n = lst->list_of_names; n; n = n->right) {
      if (str == n->left->symbol->name) {
        return lst->name_of_mtype;
      }
    }
  }

  return (char *)0;
}

int ismtype(const std::string &str) /* name to number */
{
  models::Mtypes_t *lst;
  models::Lextok *n;
  int count;

  for (lst = Mtypes; lst; lst = lst->next) {
    count = 1;
    for (n = lst->list_of_names; n; n = n->right) {
      if (str == std::string(n->left->symbol->name)) {
        return count;
      }
      count++;
    }
  }

  return 0;
}

int sputtype(std::string &foo, int m) {
  switch (m) {
  case UNSIGNED:
    foo.append("unsigned ");
    break;
  case BIT:
    foo.append("bit   ");
    break;
  case BYTE:
    foo.append("byte  ");
    break;
  case CHAN:
    foo.append("chan  ");
    break;
  case SHORT:
    foo.append("short ");
    break;
  case INT:
    foo.append("int   ");
    break;
  case MTYPE:
    foo.append("mtype ");
    break;
  case STRUCT:
    foo.append("struct");
    break;
  case PROCTYPE:
    foo.append("proctype");
    break;
  case LABEL:
    foo.append("label ");
    return 0;
  default:
    foo.append("value ");
    return 0;
  }
  return 1;
}

static int puttype(int m) {
  std::string buf;
  if (sputtype(buf, m)) {
    std::cout << buf;
    return 1;
  }
  return 0;
}

void symvar(models::Symbol *sp) {
  models::Lextok *m;

  if (!puttype(sp->type))
    return;

  printf("\t");
  if (sp->owner_name)
    printf("%s.", sp->owner_name->name.c_str());
  printf("%s", sp->name.c_str());
  if (sp->value_type > 1 || sp->is_array == 1)
    printf("[%d]", sp->value_type);

  if (sp->type == CHAN)
    printf("\t%d", (sp->init_value) ? sp->init_value->value : 0);
  else if (sp->type == STRUCT &&
           sp->struct_name != nullptr) /* Frank Weil, 2.9.8 */
    printf("\t%s", sp->struct_name->name.c_str());
  else
    printf("\t%d", eval(sp->init_value));

  if (sp->owner_name)
    printf("\t<:struct-field:>");
  else if (!sp->context)
    printf("\t<:global:>");
  else
    printf("\t<%s>", sp->context->name.c_str());

  if (sp->id < 0) /* formal parameter */
    printf("\t<parameter %d>", -(sp->id));
  else if (sp->type == models::SymbolType::kMtype)
    printf("\t<constant>");
  else if (sp->is_array)
    printf("\t<array>");
  else
    printf("\t<variable>");

  if (sp->type == CHAN && sp->init_value) {
    int i;
    for (m = sp->init_value->right, i = 0; m; m = m->right)
      i++;
    printf("\t%d\t", i);
    for (m = sp->init_value->right; m; m = m->right) {
      if (m->node_type == STRUCT)
        printf("struct %s", m->symbol->name.c_str());
      else
        (void)puttype(m->node_type);
      if (m->right)
        printf("\t");
    }
  }

  if (!launch_settings.need_old_scope_rules) {
    printf("\t{scope %s}", sp->block_scope.c_str());
  }

  printf("\n");
}

void symdump(void) {
  models::Ordered *walk;

  for (walk = all_names; walk; walk = walk->next)
    symvar(walk->entry);
}

void chname(models::Symbol *sp) {
  printf("chan ");
  if (sp->context)
    printf("%s-", sp->context->name.c_str());
  if (sp->owner_name)
    printf("%s.", sp->owner_name->name.c_str());
  printf("%s", sp->name.c_str());
  if (sp->value_type > 1 || sp->is_array == 1)
    printf("[%d]", sp->value_type);
  printf("\t");
}

static struct X_lkp {
  int typ;
  std::string nm;
} xx[] = {
    {'A', "exported as run parameter"},
    {'F', "imported as proctype parameter"},
    {'L', "used as l-value in asgnmnt"},
    {'V', "used as r-value in asgnmnt"},
    {'P', "polled in receive stmnt"},
    {'R', "used as parameter in receive stmnt"},
    {'S', "used as parameter in send stmnt"},
    {'r', "received from"},
    {'s', "sent to"},
};

static void chan_check(models::Symbol *sp) {
  auto &verbose_flags = utils::verbose::Flags::getInstance();

  models::Access *a;
  int i, b = 0, d;

  if (verbose_flags.NeedToPrintGlobalVariables())
    goto report; /* -C -g */

  for (a = sp->access; a; a = a->next)
    if (a->type == 'r')
      b |= 1;
    else if (a->type == 's')
      b |= 2;
  if (b == 3 || (sp->hidden_flags & 16)) /* balanced or formal par */
    return;
report:
  chname(sp);
  for (i = d = 0; i < (int)(sizeof(xx) / sizeof(struct X_lkp)); i++) {
    b = 0;
    for (a = sp->access; a; a = a->next) {
      if (a->type == xx[i].typ) {
        b++;
      }
    }
    if (b == 0) {
      continue;
    }
    d++;
    printf("\n\t%s by: ", xx[i].nm.c_str());
    for (a = sp->access; a; a = a->next)
      if (a->type == xx[i].typ) {
        printf("%s", a->who->name.c_str());
        if (a->what)
          printf(" to %s", a->what->name.c_str());
        if (a->count)
          printf(" par %d", a->count);
        if (--b > 0)
          printf(", ");
      }
  }
  printf("%s\n", (!d) ? "\n\tnever used under this name" : "");
}
void chanaccess(void) {
  models::Ordered *walk;
  std::string buf;
  extern lexer::Lexer lexer_;
  auto &verbose_flags = utils::verbose::Flags::getInstance();

  for (walk = all_names; walk; walk = walk->next) {
    if (!walk->entry->owner_name)
      switch (walk->entry->type) {
      case models::SymbolType::kChan: {
        if (launch_settings.need_print_channel_access_info) {
          chan_check(walk->entry);
        }
        break;
      }
      case models::SymbolType::kMtype:
      case models::SymbolType::kBit:
      case models::SymbolType::kByte:
      case models::SymbolType::kShort:
      case models::SymbolType::kInt:
      case models::SymbolType::kUnsigned:
        if ((walk->entry->hidden_flags & 128)) /* was: 32 */
          continue;

        if (!launch_settings.separate_version && !walk->entry->context &&
            !lexer_.GetHasCode() &&
            launch_settings.need_hide_write_only_variables) {
          walk->entry->hidden_flags |= 1; /* auto-hide */
        }
        if (!verbose_flags.NeedToPrintVerbose() || lexer_.GetHasCode())
          continue;

        printf("spin: %s:0, warning, ", Fname->name.c_str());
        sputtype(buf, walk->entry->type);
        if (walk->entry->context) {
          printf("proctype %s", walk->entry->context->name.c_str());
        } else {
          printf("global");
        }
        printf(", '%s%s' variable is never used (other than in print stmnts)\n",
               buf.c_str(), walk->entry->name.c_str());
      }
  }
}
