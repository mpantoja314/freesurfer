
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include "macros.h"
#include "error.h"
#include "diag.h"
#include "proto.h"
#include "mrisurf.h"
#include "mri.h"
#include "macros.h"
#include "mrishash.h"
#include "icosahedron.h"

static char vcid[] = "$Id: mris_spherical_average.c,v 1.1 2000/12/15 02:52:54 fischl Exp $";

int main(int argc, char *argv[]) ;

static int  get_option(int argc, char *argv[]) ;
static void usage_exit(void) ;
static void print_usage(void) ;
static void print_help(void) ;
static void print_version(void) ;

char *Progname ;

static int normalize_flag = 0 ;
static int condition_no = 0 ;
static int stat_flag = 0 ;
static char *output_surf_name = NULL ;
static int navgs = 0 ;
static char *ohemi = NULL ;
static char *osurf = NULL ;

static int which_ic = 5 ;
static char *sdir = NULL ;

int
main(int argc, char *argv[])
{
  char            **av, *out_fname, *surf_name, fname[STRLEN], 
                  *hemi, *cp, *data_fname ;
  int             ac, nargs, i, which, nsubjects ;
  double          max_len ;
  MRI_SURFACE     *mris, *mris_avg ;
  MRIS_HASH_TABLE *mht = NULL ;

  Progname = argv[0] ;
  ErrorInit(NULL, NULL, NULL) ;
  DiagInit(NULL, NULL, NULL) ;

  if (!sdir)
  {
    sdir = getenv("SUBJECTS_DIR") ;
    if (!sdir)
      ErrorExit(ERROR_BADPARM, "%s: no SUBJECTS_DIR in envoronment.\n",
                Progname);
  }
  ac = argc ;
  av = argv ;
  for ( ; argc > 1 && ISOPTION(*argv[1]) ; argc--, argv++)
  {
    nargs = get_option(argc, argv) ;
    argc -= nargs ;
    argv += nargs ;
  }
  
  /* 
     command line: <which> <fname> <hemi> <spherical surf> <subject 1> ... 
                   <output>
  */
  if (argc < 7)
    usage_exit() ;

  which = -1 ;
  if (!stricmp(argv[1], "coords"))
    which = VERTEX_COORDS ;
  else if (!stricmp(argv[1], "vals"))
    which = VERTEX_VALS ;
  else if (!stricmp(argv[1], "area"))
    which = VERTEX_AREA ;
  else if (!stricmp(argv[1], "curv"))
    which = VERTEX_CURV ;
  else
    usage_exit() ;

  data_fname = argv[2] ;
  hemi = argv[3] ;
  if (!ohemi)
    ohemi = hemi ;
  surf_name = argv[4] ;
  if (!osurf)
    osurf = surf_name ;
  out_fname = argv[argc-1] ;

  cp = getenv("MRI_DIR") ;
  if (!cp)
    ErrorExit(ERROR_BADPARM,"%s: MRI_DIR not defined in environment",Progname);

  sprintf(fname, "%s/lib/bem/ic%d.tri", cp, which_ic) ;
  mris_avg = ICOread(fname) ;
  if (!mris_avg)
    ErrorExit(ERROR_NOFILE, "%s: could not read ico from %s",Progname,fname) ;

  MRISclear(mris_avg, which) ;

#define FIRST_SUBJECT 5
  for (nsubjects = 0, i = FIRST_SUBJECT ; i < argc-1 ; i++, nsubjects++)
  {
    fprintf(stderr, "processing subject %s...\n", argv[i]) ;
    sprintf(fname, "%s/%s/surf/%s.%s", sdir, argv[i], hemi, surf_name) ;
    mris = MRISread(fname) ;
    if (!mris)
      ErrorExit(ERROR_NOFILE, "%s: could not read surface file %s",
              Progname, fname) ;
    if (i == FIRST_SUBJECT)  /* scale the icosahedron up */
    {
      MRISprojectOntoSphere(mris_avg, mris_avg, MRISaverageRadius(mris)) ;
      MRIScomputeVertexSpacingStats(mris_avg, NULL, NULL, &max_len, NULL,NULL);
      mht = MHTfillVertexTableRes(mris_avg, NULL, CURRENT_VERTICES,
                                  2*max_len);
    }

    switch (which)
    {
    case VERTEX_CURVATURE:
      if (MRISreadCurvatureFile(mris, data_fname) != NO_ERROR)
        ErrorExit(ERROR_BADFILE,"%s: could not read curvature file %s.\n",
                  Progname, data_fname);
      MRISaverageCurvatures(mris, navgs) ;
      if (normalize_flag)
        MRISnormalizeCurvature(mris) ;
      break ;
    case VERTEX_AREA:
      if (MRISreadOriginalProperties(mris, data_fname) != NO_ERROR)
        ErrorExit(ERROR_BADFILE,"%s: could not read surface file %s.\n",
                  Progname, data_fname);
#if 0
      fprintf(stderr, "total orig area = %2.1f 10^3 mm\n",
              mris->orig_area/1000) ;
#endif
      break ;
    default:
      
      break ;
    }
    MRIScombine(mris, mris_avg, mht, which) ;
    if (i < argc-2)
      MRISfree(&mris) ;
  }
  MRISnormalize(mris_avg, nsubjects, which) ;
  MHTfree(&mht) ;

  if (output_surf_name)
  {
    sprintf(fname, "%s/%s/surf/%s.%s", sdir,output_surf_name,ohemi,osurf);
    fprintf(stderr, "reading output surface %s...\n", fname) ;
    MRISfree(&mris) ;
    mris = MRISread(fname) ;
    if (!mris)
      ErrorExit(ERROR_NOFILE, "%s: could not read surface file %s",
              Progname, fname) ;
  }

  MRISclear(mris, which) ;
  MRIScomputeVertexSpacingStats(mris, NULL, NULL, &max_len, NULL,NULL);
  mht = MHTfillVertexTableRes(mris, NULL, CURRENT_VERTICES,
                              2*max_len);
  MRISsphericalCopy(mris_avg, mris, mht, which) ;
  MHTfree(&mht) ;
  if (which == VERTEX_AREA)
    MRISorigAreaToCurv(mris) ;
  if (stat_flag)    /* write out summary statistics files */
  {
    int    vno ;
    VERTEX *v ;
    float  dof ;
    FILE   *fp ;

    sprintf(fname, "%s/sigavg%d-%s.w", out_fname, condition_no, ohemi);
    fprintf(stderr, "writing output means to %s\n", fname) ;
    MRISwriteCurvatureToWFile(mris, fname) ;

    /* change variances to squared standard errors */
    dof = nsubjects ;
    if (!FZERO(dof)) for (vno = 0 ; vno < mris->nvertices ; vno++)
      {
        v = &mris->vertices[vno] ;
        if (v->ripflag)
          continue ;
        v->curv = v->val2 / dof ;   /* turn it into a standard error */
      }

    sprintf(fname, "%s/sigvar%d-%s.w", out_fname, condition_no, ohemi);
    fprintf(stderr, "writing output variances to %s\n", fname) ;
    MRISwriteCurvatureToWFile(mris, fname) ;

    /* write out dof file */
    sprintf(fname, "%s/sigavg%d.dof", out_fname, condition_no) ;
    fp = fopen(fname, "w") ;
    if (!fp)
      ErrorExit(ERROR_NOFILE, "%s: could not open dof file %s\n",
                Progname,fname);
    fprintf(stderr, "writing dof file %s\n", fname) ;
    fprintf(fp, "%d\n", (int)dof) ;
    fclose(fp) ;
  }
  else
  {
    if (Gdiag & DIAG_SHOW)
      fprintf(stderr,"writing blurred pattern to surface to %s\n",out_fname);
    switch (which)
    {
    case VERTEX_AREA:
    case VERTEX_CURV:
      MRISwriteCurvature(mris, out_fname) ;
      break ;
    default:
      break ;
    }
  }

  MRISfree(&mris) ;
  exit(0) ;
  return(0) ;  /* for ansi */
}

/*----------------------------------------------------------------------
            Parameters:

           Description:
----------------------------------------------------------------------*/
static int
get_option(int argc, char *argv[])
{
  int  nargs = 0 ;
  char *option ;
  
  option = argv[1] + 1 ;            /* past '-' */
  if (!stricmp(option, "-help"))
    print_help() ;
  else if (!stricmp(option, "-version"))
    print_version() ;
  else if (!stricmp(option, "ohemi"))
  {
    ohemi = argv[2] ;
    fprintf(stderr, "output hemisphere = %s\n", ohemi) ;
    nargs = 1 ;
  }
  else if (!stricmp(option, "ic"))
  {
    which_ic = atoi(argv[2]) ;
    nargs = 1 ;
  }
  else if (!stricmp(option, "sdir"))
  {
    sdir = argv[2] ;
    nargs = 1 ;
  }
  else if (!stricmp(option, "osurf"))
  {
    osurf = argv[2] ;
    fprintf(stderr, "output surface = %s\n", osurf) ;
    nargs = 1 ;
  }
  else switch (toupper(*option))
  {
  case 'A':
    navgs = atoi(argv[2]) ;
    fprintf(stderr, "blurring thickness for %d iterations\n",navgs);
    nargs = 1 ;
    break ;
  case 'O':
    output_surf_name = argv[2] ;
    nargs = 1 ;
    fprintf(stderr, "painting output onto subject %s.\n", output_surf_name);
    break ;
  case '?':
  case 'U':
    print_usage() ;
    exit(1) ;
    break ;
  case 'S':   /* write out stats */
    stat_flag = 1 ;
    condition_no = atoi(argv[2]) ;
    nargs = 1 ;
    fprintf(stderr, "writing out summary statistics as condition %d\n",
            condition_no) ;
    break ;
  case 'N':
    normalize_flag = 1 ;
    break ;
  default:
    fprintf(stderr, "unknown option %s\n", argv[1]) ;
    exit(1) ;
    break ;
  }

  return(nargs) ;
}

static void
usage_exit(void)
{
  print_usage() ;
  exit(1) ;
}

static void
print_usage(void)
{
  fprintf(stderr, 
          "usage: %s [option] <which> <fname> <hemi> <spherical surf> "
          "<subject 1> ... <output>\n", Progname) ;
  fprintf(stderr, "where which is one of\n"
          "\tcoords\n"
          "\tvals\n"
          "\tcurv\n"
          "\tarea\n") ;
}

static void
print_help(void)
{
  print_usage() ;
  fprintf(stderr, 
       "\nThis program will add a template into an average surface.\n");
  fprintf(stderr, "\nvalid options are:\n\n") ;
  fprintf(stderr, "-s <cond #>     generate summary statistics and write\n"
                  "                them into sigavg<cond #>-<hemi>.w and\n"
                  "                sigvar<cond #>-<hemi>.w.\n") ;
  exit(1) ;
}

static void
print_version(void)
{
  fprintf(stderr, "%s\n", vcid) ;
  exit(1) ;
}

