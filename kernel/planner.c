/*
 * Copyright (c) 2000 Matteo Frigo
 * Copyright (c) 2000 Steven G. Johnson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/* $Id: planner.c,v 1.110 2002-09-18 21:16:16 athena Exp $ */
#include "ifftw.h"
#include <string.h>

/* GNU Coding Standards, Sec. 5.2: "Please write the comments in a GNU
   program in English, because English is the one language that nearly
   all programmers in all countries can read."

                    ingemisco tanquam reus
		    culpa rubet vultus meus
		    supplicanti parce [rms]
*/

#define BLESSEDP(solution) ((solution)->flags & BLESSING)
#define VALIDP(solution) ((solution)->flags & H_VALID)

#define MAXNAM 64  /* maximum length of registrar's name.
		      Used for reading wisdom.  There is no point
		      in doing this right */

/* Flags f1 subsumes flags f2 iff f1 is less/equally impatient than
   f2, defining a partial ordering. */
#define IMPATIENCE(flags) ((flags) & IMPATIENCE_FLAGS)
#define ORDERED(f1, f2) (subsumes(f1, f2) || subsumes(f2, f1))

static unsigned short canonicalize(unsigned short x)
{
     return x;
}

#define CANONICALP(x) ((x) == canonicalize(x))

static int subsumes(unsigned short f1, unsigned short f2)
{
     A(CANONICALP(f1));
     A(CANONICALP(f2));
     return (IMPATIENCE(f1) & f2) == IMPATIENCE(f1);
}


/*
  slvdesc management:
*/
static uint hash_regnam(const char *s)
{
     uint h = 0xDEADBEEFul;
     do {
	  h = h * 17 + *s;
     } while (*s++);
     return h;
}

static void sgrow(planner *ego)
{
     uint osiz = ego->slvdescsiz, nsiz = 1 + osiz + osiz / 4;
     slvdesc *ntab = (slvdesc *)fftw_malloc(nsiz * sizeof(slvdesc), SLVDESCS);
     slvdesc *otab = ego->slvdescs;
     uint i;

     ego->slvdescs = ntab;
     ego->slvdescsiz = nsiz;
     for (i = 0; i < osiz; ++i)
	  ntab[i] = otab[i];
     if (otab) 
	  X(free)(otab);
}

static void register_solver(planner *ego, solver *s)
{
     slvdesc *n;
     if (s) { /* add s to solver list */
	  X(solver_use)(s);

	  if (ego->nslvdesc >= ego->slvdescsiz)
	       sgrow(ego);

	  n = ego->slvdescs + ego->nslvdesc++;

	  n->slv = s;
	  n->reg_nam = ego->cur_reg_nam;
	  n->reg_id = ego->cur_reg_id++;
	  
	  A(strlen(n->reg_nam) < MAXNAM);
	  n->nam_hash = hash_regnam(n->reg_nam);
     }
}

static short slookup(planner *ego, char *nam, int id)
{
     uint h = hash_regnam(nam); /* used to avoid strcmp in the common case */
     FORALL_SOLVERS(ego, s, sp, {
	  UNUSED(s);
	  if (sp->reg_id == id && sp->nam_hash == h
	      && !strcmp(sp->reg_nam, nam))
	       return sp - ego->slvdescs;
     });
     return -1;
}

/*
  md5-related stuff:
*/

/* first hash function */
static uint h1(planner *ego, const md5sig s)
{
     return s[0] % ego->hashsiz;
}

/* second hash function (for double hashing) */
static uint h2(planner *ego, const md5sig s)
{
     return 1 + s[1] % (ego->hashsiz - 1);
}

static void md5hash(md5 *m, const problem *p, const planner *plnr)
{
     X(md5begin)(m);
     X(md5uint)(m, sizeof(R)); /* so we don't mix different precisions */
     X(md5uint)(m, plnr->problem_flags);
     X(md5uint)(m, plnr->nthr);
     p->adt->hash(p, m);
     X(md5end)(m);
}

static int md5eq(const md5sig a, const md5sig b)
{
     return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static void sigcpy(const md5sig a, md5sig b)
{
     b[0] = a[0]; b[1] = a[1]; b[2] = a[2]; b[3] = a[3];
}

/*
  memoization routines :
*/

/*
   liber scriptus proferetur
   in quo totum continetur
   unde mundus iudicetur
*/
struct solution_s {
     md5sig s;
     unsigned short flags;
     short slvndx;
};

static solution *hlookup(planner *ego, const md5sig s, unsigned short flags)
{
     uint g, h = h1(ego, s), d = h2(ego, s);

     ++ego->lookup;

     for (g = h; ; g = (g + d) % ego->hashsiz) {
	  solution *l = ego->solutions + g;
	  ++ego->lookup_iter;
	  if (VALIDP(l)) {
	       if (md5eq(s, l->s) && ORDERED(l->flags, flags)) { 
		    ++ego->succ_lookup;
		    return l; 
	       }
	  } else {
	       return 0;
	  }
	  A((g + d) % ego->hashsiz != h);
     }
}


static void hinsert0(planner *ego, const md5sig s, unsigned short flags,
		     short slvndx, solution *l)
{
     A(CANONICALP(flags));

     ++ego->insert;
     if (!l) { 	 
	  /* search for nonfull slot */
	  uint g, h = h1(ego, s), d = h2(ego, s); 
	  ++ego->insert_unknown;
	  for (g = h; ; g = (g + d) % ego->hashsiz) {
	       ++ego->insert_iter;
	       l = ego->solutions + g;
	       if (!VALIDP(l)) break;
	       A((g + d) % ego->hashsiz != h);
	  }
     }

     /* fill slot */
     l->flags = flags | H_VALID;
     l->slvndx = slvndx;
     sigcpy(s, l->s);
}

static void rehash(planner *ego, uint nsiz)
{
     uint osiz = ego->hashsiz, h;
     solution *osol = ego->solutions, *nsol;

     nsiz = X(next_prime)(nsiz);
     nsol = (solution *)fftw_malloc(nsiz * sizeof(solution), HASHT);
     ++ego->nrehash;

     /* init new table */
     for (h = 0; h < nsiz; ++h) 
	  nsol[h].flags = 0;

     /* install new table */
     ego->hashsiz = nsiz;
     ego->solutions = nsol;

     /* copy table */
     for (h = 0; h < osiz; ++h) {
	  solution *l = osol + h;
	  if (VALIDP(l))
	       hinsert0(ego, l->s, l->flags, l->slvndx, 0);
     }

     if (osol)
	  X(free)(osol);
}

static uint minsz(uint nelem)
{
     return 1 + nelem + nelem / 8;
}

static uint nextsz(uint nelem)
{
     return minsz(minsz(nelem));
}

static void hgrow(planner *ego)
{
     uint nelem = ego->nelem;
     if (minsz(nelem) >= ego->hashsiz)
	  rehash(ego, nextsz(nelem));
}

static void hshrink(planner *ego)
{
     uint nelem = ego->nelem;
     /* always rehash after deletions */
     rehash(ego, nextsz(nelem));
}

static void hinsert(planner *ego, const md5sig s, 
		    unsigned short flags, short slvndx)
{
     solution *l;

     if ((l = hlookup(ego, s, flags))) {
	  if (subsumes(flags, l->flags)) {
	       /* overwrite old solution */
	       flags |= l->flags & BLESSING; /* ne me perdas illa die */
	  } else {
	       A(subsumes(l->flags, flags));
	       return;
	  }
     } else {
	  ++ego->nelem;
	  hgrow(ego);
     }
     hinsert0(ego, s, flags, slvndx, l);
}

static void hcurse_subsumed(planner *ego)
{
     uint h;

     /* unbless any entries that are unreachable because they
        are subsumed by less-impatient ones.  */
     for (h = 0; h < ego->hashsiz; ++h) {
	  solution *l = ego->solutions + h;
	  if (VALIDP(l)) {
	       uint d = h2(ego, l->s), g = (h + d) % ego->hashsiz;
	       for (; ; g = (g + d) % ego->hashsiz) {
		    solution *m = ego->solutions + g;
		    if (VALIDP(m)) {
			 if (md5eq(l->s, m->s) 
			     && subsumes(l->flags, m->flags)) {
			      /* quidquid latet apparebit */
			      l->flags |= m->flags & BLESSING;
			      /* cum vix justus sit securus */
			      m->flags &= ~BLESSING;
			 }
		    }
		    else break;
		    A((g + d) % ego->hashsiz != h);
	       }
	  }
     }
}


static void evaluate_plan(planner *ego, plan *pln, const problem *p)
{
     if (!BELIEVE_PCOSTP(ego) || pln->pcost == 0.0) {
	  ego->nplan++;
	  if (ESTIMATEP(ego)) {
	       /* heuristic */
	       pln->pcost = 0
		    + pln->ops.add
		    + pln->ops.mul
		    + 2 * pln->ops.fma
		    + pln->ops.other;
	  } else {
	       pln->pcost = X(measure_execution_time)(pln, p);
	  }
     }
     ego->hook(pln, p, 0);
}

/* maintain dynamic scoping of flags, nthr: */
static plan *invoke_solver(planner *ego, problem *p, solver *s, 
			   unsigned short nflags)
{
     unsigned short planner_flags = ego->planner_flags;
     uint problem_flags = ego->problem_flags;
     uint nthr = ego->nthr;
     plan *pln;
     ego->planner_flags |= nflags;
     pln = s->adt->mkplan(s, p, ego);
     ego->problem_flags = problem_flags;
     ego->nthr = nthr;
     ego->planner_flags = planner_flags;
     return pln;
}

static void mkplan0(planner *ego, problem *p, plan **bestp, slvdesc **descp)
{
     plan *best = 0;
     int best_not_yet_timed = 1;
     int pass, lpass;

     *bestp = 0;

     /* if a solver is suggested (by wisdom) use it */
     {
	  slvdesc *sp;
	  if ((sp = *descp)) {
	       solver *s = sp->slv;
	       best = invoke_solver(ego, p, s, 0);
	       if (best) 
		    X(plan_use)(best);
	  }
     }

     lpass = 2;

     for (pass = 0; pass < lpass; ++pass) {
	  static const unsigned short nflags[2] = { NO_UGLY, 0 };

	  if (best) break;
          FORALL_SOLVERS(ego, s, sp, {
	       plan *pln = invoke_solver(ego, p, s, nflags[pass]);

	       if (pln) {
		    X(plan_use)(pln);
		    if (best) {
			 if (best_not_yet_timed) {
			      evaluate_plan(ego, best, p);
			      best_not_yet_timed = 0;
			 }
			 evaluate_plan(ego, pln, p);
			 if (pln->pcost < best->pcost) {
			      X(plan_destroy)(best);
			      best = pln;
			      *descp = sp;
			 } else {
			      X(plan_destroy)(pln);
			 }
		    } else {
			 best = pln;
			 *descp = sp;
		    }
	       }
	  });
     }

     if (best) {
	  /* hack: postulate de iure that NO_UGLY subsumes ~NO_UGLY 
	     if the problem is feasible */
	  ego->planner_flags &= ~NO_UGLY;
     }
     *bestp = best;
}

static plan *mkplan(planner *ego, problem *p)
{
     solution *sol;
     plan *pln;
     md5 m;
     slvdesc *sp;

     ego->planner_flags = canonicalize(ego->planner_flags);

     ++ego->nprob;
     md5hash(&m, p, ego);

     sp = 0; /* nothing known about this problem, yet */
     if ((sol = hlookup(ego, m.s, ego->planner_flags))) {
	  if (subsumes(sol->flags, ego->planner_flags)) {
	       /* wisdom is acceptable */
	       if (sol->slvndx < 0)
		    return 0;   /* known to be infeasible */
	       sp = ego->slvdescs + sol->slvndx;
	  } else {
	       A(subsumes(ego->planner_flags, sol->flags));
	  }
     }

     mkplan0(ego, p, &pln, &sp);

     hinsert(ego, m.s, ego->planner_flags, 
	     sp ? sp - ego->slvdescs : -1);

     if (pln)
	  ego->hook(pln, p, 1);
     return pln;
}

/* destroy hash table entries.  If FORGET_EVERYTHING, destroy the whole
   table.  If FORGET_ACCURSED, then destroy entries that are not blessed. */
static void forget(planner *ego, amnesia a)
{
     uint h;

     /* garbage-collect while we are at it */ 
     if (a != FORGET_EVERYTHING)
	  hcurse_subsumed(ego);

     for (h = 0; h < ego->hashsiz; ++h) {
	  solution *l = ego->solutions + h;
	  if (VALIDP(l)) {
	       if (a == FORGET_EVERYTHING ||
		   (a == FORGET_ACCURSED && !BLESSEDP(l))) {
		    /* confutatis maledictis
		       flammis acribus addictis */
		    l->flags &= ~H_VALID;
		    --ego->nelem;
	       }
	  }
     }

     /* nil inultum remanebit */

     hshrink(ego);
}

static void htab_destroy(planner *ego)
{
     forget(ego, FORGET_EVERYTHING);
     X(free)(ego->solutions);
     ego->nelem = 0;
}

/* FIXME: what sort of version information should we write? */
#define WISDOM_PREAMBLE PACKAGE "-" VERSION "-wisdom "

/* tantus labor non sit cassus */
static void exprt(planner *ego, printer *p)
{
     uint h;

     hcurse_subsumed(ego);

     p->print(p, "(" WISDOM_PREAMBLE "%(");
     for (h = 0; h < ego->hashsiz; ++h) {
	  solution *l = ego->solutions + h;
	  if (VALIDP(l) && BLESSEDP(l) && l->slvndx >= 0) {
	       slvdesc *sp = ego->slvdescs + l->slvndx;
	       /* qui salvandos salvas gratis
		  salva me fons pietatis */
	       p->print(p, "(%s %d #x%x #x%M #x%M #x%M #x%M)\n",
			sp->reg_nam, sp->reg_id, (uint)l->flags,
			l->s[0], l->s[1], l->s[2], l->s[3]);
	  }
     }
     p->print(p, "%))\n");
}

/* mors stupebit et natura
   cum resurget creatura */
static int imprt(planner *ego, scanner *sc)
{
     char buf[MAXNAM + 1];
     md5uint sig[4];
     uint flags;
     int reg_id;
     short slvndx;

     if (!sc->scan(sc, "(" WISDOM_PREAMBLE))
	  goto bad;

     while (1) {
	  if (sc->scan(sc, ")"))
	       break;

	  /* qua resurget ex favilla */
	  if (!sc->scan(sc, "(%*s %d #x%x #x%M #x%M #x%M #x%M)",
			MAXNAM, buf, &reg_id, &flags, 
			sig + 0, sig + 1, sig + 2, sig + 3))
	       goto bad;

	  if ((slvndx = slookup(ego, buf, reg_id)) < 0)
	       goto bad; /* TODO: panic? */

	  /* inter oves locum praesta */
	  hinsert(ego, sig, (unsigned short)flags, slvndx);
     }
     return 1;

 bad:
     return 0;
}

static void hooknil(plan *pln, const problem *p, int optimalp)
{
     UNUSED(pln);
     UNUSED(p);
     UNUSED(optimalp);
     /* do nothing */
}

/*
 * create a planner
 */
planner *X(mkplanner)(void)
{
     static const planner_adt padt = {
	  register_solver, mkplan, forget, exprt, imprt
     };

     planner *p = (planner *) fftw_malloc(sizeof(planner), PLANNERS);

     p->adt = &padt;
     p->nplan = p->nprob = p->nrehash = 0;
     p->succ_lookup = p->lookup = p->lookup_iter = 0;
     p->insert = p->insert_iter = p->insert_unknown = 0;
     p->hook = hooknil;
     p->cur_reg_nam = 0;

     p->slvdescs = 0;
     p->nslvdesc = p->slvdescsiz = 0;

     p->solutions = 0;
     p->hashsiz = p->nelem = 0;

     p->problem_flags = 0;
     p->planner_flags = 0;
     p->nthr = 1;
     hgrow(p);			/* so that hashsiz > 0 */

     return p;
}

void X(planner_destroy)(planner *ego)
{
     /* destroy hash table */
     htab_destroy(ego);

     /* destroy solvdesc table */
     FORALL_SOLVERS(ego, s, sp, {
	  UNUSED(sp);
	  X(solver_destroy)(s);
     });

     if (ego->slvdescs)
	  X(free)(ego->slvdescs);

     X(free)(ego); /* dona eis requiem */
}

/*
 * Debugging code:
 */
#ifdef FFTW_DEBUG
#define IMPLIES(a, b) ((b) || !(a))

static void check_partial_order(void)
{
     unsigned short a, b, c;
     for (a = 0; a <= IMPATIENCE_FLAGS; ++a) if (CANONICALP(a)) {
	  /* reflexive */
	  CK(subsumes(a, a));
	  for (b = 0; b <= IMPATIENCE_FLAGS; ++b) if (CANONICALP(b)) {
	       /* antisymmetric */
	       CK(IMPLIES(subsumes(a, b) && subsumes(b, a), a == b));
	       for (c = 0; c <= IMPATIENCE_FLAGS; ++c) if (CANONICALP(c)) {
		    /* transitive */
		    CK(IMPLIES(subsumes(a, b) && subsumes(b, c),
			       subsumes(a, c)));
	       }
	  }
     }
}

void X(planner_dump)(planner *ego, int verbose)
{
     uint valid = 0, empty = 0, infeasible = 0;
     uint h;
     UNUSED(verbose); /* historical */

     if (0)
	  check_partial_order();

     for (h = 0; h < ego->hashsiz; ++h) {
	  solution *l = ego->solutions + h; 
	  if (VALIDP(l)) {
	       ++valid; 
	       if (l->slvndx < 0) ++infeasible;
	  } else
	       ++empty;
	  
     }

     D("nplan = %u\n", ego->nplan);
     D("nprob = %u\n", ego->nprob);
     D("lookup = %u\n", ego->lookup);
     D("succ_lookup = %u\n", ego->succ_lookup);
     D("lookup_iter = %u\n", ego->lookup_iter);
     D("insert = %u\n", ego->insert);
     D("insert_iter = %u\n", ego->insert_iter);
     D("insert_unknown = %u\n", ego->insert_unknown);
     D("nrehash = %d\n", ego->nrehash);
     D("hashsiz = %d\n", ego->hashsiz);
     D("empty = %d\n", empty);
     D("valid = %d\n", valid);
     D("infeasible = %d\n", infeasible);
     A(ego->nelem == valid);
}

#endif
