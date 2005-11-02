////////////////////////////////////////////////////////////////////
//               Florent Segonne & Bruce Fischl 
//
// mri_watershed.cpp
//
// Warning: Do not edit the following four lines.  CVS maintains them.
// Revision Author: $Author: kteich $
// Revision Date  : $Date: 2005/11/02 16:29:44 $
// Revision       : $Revision: 1.38 $
//
////////////////////////////////////////////////////////////////////
char *MRI_WATERSHED_VERSION = "$Revision: 1.38 $";

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>

extern "C" {
#include "mri.h"
#include "macros.h"
#include "error.h"
#include "tags.h"
#include "mrisurf.h"
#include "matrix.h"
#include "proto.h"
#include "stats.h"
#include "timer.h"
#include "const.h"
#include "mrishash.h"
#include "icosahedron.h"
#include "tritri.h"
#include "timer.h"
#include "chklc.h"
#include "diag.h"
#include "version.h"
#include "mrisegment.h"
}

#define SQR(x) ((x)*(x))

#define WM_CONST 110 /* not used anymore */
#define MAX_INT 100 /*100% is a good value for the watershed algo */
 
#define ITER_SMOOTH 10
#define NBR_AVGS 20
#define NBR_NGBS 2

#define DEFAULT_MODE 0
#define DIST_MODE 1
#define CURV_MODE 2

/////////////////////////////// debug
// ascii histogram on console
#define DEBUG_CURVE 0
// write histogram on "curves.out"
#define OUTPUT_CURVES 0

#define VERBOSE_MODE 0

#define WRITE_SURFACES 0
#define NO_SELF_INTERSECTION 0

#define MAX_MASK_VOLUMES  50
static int nmask_volumes = 0 ;
static char mask_in_fnames[MAX_MASK_VOLUMES][STRLEN] ;
static char mask_out_fnames[MAX_MASK_VOLUMES][STRLEN] ;

typedef struct Cell
{
  unsigned char type;
  void * next;
} Cell;

typedef struct Basincell
{
  unsigned char depth;
  unsigned long size;
  unsigned long ambiguous;
} BasinCell;

typedef struct Bound
{
  unsigned char x,y,z,val;
  struct Bound *next;
} Bound;

typedef unsigned short Coord[3];

typedef struct STRIP_PARMS    
{ 
  /*  float  fill_level;*/
  int template_deformation; 
  /*to save the surfaces into the output volume*/
  int surf_dbg; 
  /*to write out the brain surface into the file surfname*/
  /*the brain surface is shrank inward of h_shrk mm*/
  int brainsurf;
  /*to write out all the BEM surfaces : brain, outer and inner skull, scalp*/
  int surf;
  /*to labelize the volume into scalp, skull, csf, white and gray*/
  int label;
  /*to use the atlas validation and correction*/
  int atlas;

  /*specify T1 volume*/
  int T1;
  int noT1analysis;
  /*specify the center fo the brain and its radius*/
  int cx,cy,cz,rb; 

  char *surfname;
  int h_shk;
  int skull_type;  
  int watershed_analyze;  
  int threshold_analyze;   

  int seed_coord[30][4];
  int nb_seed_points/*=0*/;
  unsigned char hpf;  
          
  int manual_params;
  int manual_CSF_MAX,manual_TRANSITION_intensity,manual_GM_intensity;

  //number of iteration in MRISgoToClosestDarkestPoint
  int dark_iter;

  // whether to use surfaceRAS or not
  int useSRAS;

} STRIP_PARMS ;


typedef struct
{
  float direction[26][3]; // 3x3x3 neighbor (exclude self) = 27-1 = 26
  MRIS *mrisphere,*mris,*mris_curv,*mris_var_curv,*mris_dCOG,*mris_var_dCOG;

  double xCOG,yCOG,zCOG,rad_Brain; // voxel coordinates
  double xsCOG,ysCOG,zsCOG;        // RAS coordinates
  int i_global_min/*=0*/,j_global_min,k_global_min,int_global_min;
  unsigned long estimated_size/*=0*/,main_basin_size/*=0*/;
  unsigned long brain_size /*=0*/;
  unsigned long basinnumber,basinsize;

  MRI *mri_src,*mri_dst,*mri_orig;
  int width,height,depth;

  unsigned char Imax;
  int WM_intensity,WM_VARIANCE,WM_HALF_MAX,WM_HALF_MIN,WM_MAX,WM_MIN;
  int CSF_intensity,CSF_HALF_MAX,CSF_MAX,CSF_MIN;
  int GM_MIN, GM_intensity,TRANSITION_intensity; 
  // hard to read with capital INTENSITY

  unsigned long gmnumber[256];

  Bound *Bound1,*Bound2;

  Cell *** Basin;

  Coord** Table[256];

  unsigned char intbasin[256];
  unsigned long tabdim[256];
  unsigned long sqrdim[256];
  unsigned long count[256];

  Coord* T1Table;
  long T1nbr;

  int decision;
  float scale;

  int atlas;
  int validation;
  int verbose_mode;
  int dark_iter;

} MRI_variables;

char *Progname;

static int type_changed = 0 ;
static int old_type ;

#ifndef __OPTIMIZE__
// this routine is slow and should be used only for diagnostics
static int calcBrainSize(const MRI* mri_src, const MRIS *mris);
#endif
static void Error(char *string);
static int get_option(int argc, char *argv[],STRIP_PARMS *parms) ;
static STRIP_PARMS* init_parms(void);
static MRI_variables* init_variables(MRI *mri_with_skull);
MRI *MRIstripSkull(MRI *mri_with_skull, MRI *mri_without_skull, 
                   STRIP_PARMS *parms);
static void MRIVfree(MRI_variables *MRI_var);
/*WATERSHED FUNCTIONS*/
static int Watershed(STRIP_PARMS *parms,MRI_variables *MRI_var);
static void AnalyzeT1Volume(STRIP_PARMS *parms,MRI_variables *MRI_var);
static void Allocation(MRI_variables *MRI_var);
static int calCSFIntensity(MRI_variables *MRI_var);
static int calCOGMAX(MRI_variables *MRI_var, STRIP_PARMS *parms, 
                     int *x, int *y, int *z);
static int Pre_CharSorting(STRIP_PARMS *parms, MRI_variables *MRI_var);
static void analyseWM(double *tab,MRI_variables *MRI_var);
static BasinCell* CreateBasinCell(int val, unsigned long size, 
                                  unsigned long ambiguous);
static int Decision(STRIP_PARMS *parms,  MRI_variables *MRI_var);
static void FindMainWmComponent(MRI_variables *MRI_var);
static int CharSorting(MRI_variables *MRI_var);
static int sqroot(int r);
static int Analyze(STRIP_PARMS *parms,MRI_variables *MRI_var);
static Cell* FindBasin(Cell *cell);
static int Lookat(int,int,int,unsigned char,int*,Cell**,int*,Cell* adtab[27],
                  STRIP_PARMS *parms,MRI_variables *MRI_var);
static int Test(Coord crd,STRIP_PARMS *parms,MRI_variables *MRI_var);
static Cell* TypeVoxel(Cell *cell);
static int PostAnalyze(STRIP_PARMS *parms,MRI_variables *MRI_var);
static int Merge(unsigned char i,unsigned char j,unsigned char k,
                 int val,int *n,MRI_variables *MRI_var);
static int AddVoxel(MRI_variables *MRI_var);
static int AroundCell(unsigned char i,unsigned char j,unsigned char k,
                      MRI_variables *MRI_var);
static int MergeRoutine(unsigned char,unsigned char,unsigned char,int,int*,
                        MRI_variables *MRI_var);
static int FreeMem(MRI_variables *MRI_var);
static int Save(MRI_variables *MRI_var);
#if 0
static int Mediane(int i,int j,int k,int rang);
static int Ambiguous(Cell* cell);
#endif
/*TEMPLATE DEFORMATION FUNCTIONS*/
// print the int_percent curve on the stderr
// only when OUTPUT_CURVES or DEBUG_CURVE is set to 1
template <typename T> void DebugCurve(const T *int_percent, 
                                      const int max, 
                                      const char *msg);
template <typename T> int findMaxIndex(const T *tab);
template <typename T> int findHalfMax(const T *tab, int maxIndex);
template <typename T> int findHalfMin(const T *tab, int maxIndex);
void calcForce1(double &force0, double &force1, double &force, 
                const double &x, const double &y, const double &z,
                const double &sx, const double &sy, const double &sz, 
                const double &sd,
                const double &nx, const double &ny, const double &nz,
                MRI_variables *mri_var);
void calcForce2(double &force0, double &force1, double &force, 
                const double &x, const double &y, const double &z,
                const double &sx, const double &sy, const double &sz, 
                const double &sd,
                const double &nx, const double &ny, const double &nz,
                MRI_variables *mri_var);
// pass a function pointer for force calculation
static void FitShape(MRI_variables *MRI_var, const int convLimit,
                     const int maxIter,
                     void (*calcForce)
                     (double &force0, double &force1, double &force, 
                      const double &x, const double &y, const double &z,
                      const double &sx, const double &sy, const double &sz, 
                      const double &sd,
                      const double &nx, const double &ny, const double &nz,
                      MRI_variables *mri_var)
                     );
static void read_geometry(int type,MRI_variables *MRI_var,char *surf_fname);
static void Template_Deformation(STRIP_PARMS *parms,MRI_variables *MRI_var);
static void brain_params(MRI_variables *MRI_var);
static void init_surf_to_image(float rx, float ry, float rz,
                               MRI_variables *MRI_var);
static void write_image(MRI_variables *MRI_var);
static void init_direction(MRI_variables *MRI_var);
static void find_normal(const float nx,const float ny, const float nz,
                        float* n1,float *n2,
                        float direction[26][3]);
static void local_params(STRIP_PARMS *parms,MRI_variables *MRI_var);
static void analyseCSF(unsigned long *CSF_percent,MRI_variables *MRI_var);
static void analyseGM(unsigned long *CSF_percent,unsigned long *int_percent,
                      MRI_variables *MRI_var);
static int FindWM(unsigned char tab[4][9],MRI_variables *MRI_var);
static void lisse(unsigned long *tab, const char *msg);
static unsigned long MRISpeelBrain(float h,MRI *mri_dst,MRIS *mris,
                                   unsigned char val);
static void shrinkstep(MRI_variables *MRI_var);
static void mean(float tab[4][9],float *moy);
static void MRIShighlyTesselatedSmoothedSurface(MRI_variables *MRI_var);
static void MRISsmooth_surface(MRI_SURFACE *mris,int niter);
static void MRISshrink_surface(MRIS *mris,int h);
static void MRISshrink_Outer_Skin(MRI_variables *MRI_var,MRI* mri_src);
static void label_voxels(STRIP_PARMS *parms, MRI_variables *MRI_var,
                         MRI* mri_with_skull);
#if 0
static void mrisComputeNormals(MRIS *mris);
static void normal_face(int f,float *n,MRIS *mris);
static int mrisNormalFace(MRIS *mris, int fac,int n,float norm[]);
static float rtanh(float x);
#endif
/*VALIDATION - SURFACE CORRECTION*/
static int ValidationSurfaceShape(MRI_variables *MRI_var);
static void MRIScenterCOG(MRI_SURFACE *mris);
void MRISscale(MRI_SURFACE *mris);
double MRISradius(MRI_SURFACE *mris);
static void MRISinitSurfaces(MRIS *mris_curv,MRIS *mris_dCOG, 
                             const MRIS *mrisphere);
static void MRISdistanceToCOG(MRI_SURFACE *mris);
static double mrisComputeCorrelationError(MRI_SURFACE *mris, 
                                          INTEGRATION_PARMS *parms,
                                          int use_stds);
void MRISchangeCoordinates(MRI_SURFACE *mris, const MRI_SURFACE *mris_orig);
static int
mrisRigidBodyAlignGlobal(MRIS *mris,MRIS *mris_dist, INTEGRATION_PARMS *parms,
                         int mode,float min_degrees, float max_degrees, 
                         int nangles);
static int mrisLocalizeErrors(MRIS* mris_curv,MRIS *mris_dCOG,
                              MRI_variables *MRI_var,MRIS *mrisphere);
static void MRISCorrectSurface(MRI_variables *MRI_var);
static void MRISComputeLocalValues(MRI_variables *MRI_var);
static void MRISFineSegmentation(MRI_variables *MRI_var);
static void MRISscaleFields(MRIS *mris_src,MRIS *mris_dst,
                            MRIS *mris_vdst,
                            int whichfield);
static void MRISgoToClosestDarkestPoint(MRI_variables *MRI_var);
/*mri->type correction*/

//////////////////////////////////////////////////////////////////
// declare global function pointers  
// initialized at init_parms(). reset when -useSRAS is set
int (*myWorldToVoxel)(MRI *mri,
                      Real xw, Real yw, Real zw, 
                      Real *xv, Real *yv, Real *zv);
int (*myVoxelToWorld)(MRI *mri, 
                      Real xv, Real yv, Real zv, 
                      Real *xw, Real *yw, Real *zw);
///////////////////////////////////////////////////////////////////

void usageHelp()
{
  fprintf(stderr, "\nUsage: %s [options] input_file output_file", Progname);
  fprintf(stderr, "\noptions are:");
  fprintf(stderr, "\n-atlas               : "
          "use the atlas information to correct the segmentation");
  fprintf(stderr, "\n-less                : shrink the surface");
  fprintf(stderr, "\n-more                : expand the surface");
  fprintf(stderr, "\n-wat                 : use only the watershed algorithm");
  fprintf(stderr, "\n-T1                  : specify T1 input volume");
  fprintf(stderr, "\n-noT1                : "
          "specify no T1 analysis.  Useful when running out of memory");
  fprintf(stderr, "\n-wat+temp            : "
          "watershed algo and first template smoothing");
  fprintf(stderr, "\n-first_temp          : "
          "use only the first template smoothing + local matching"); 
  fprintf(stderr, "\n-useSRAS             : "
          "use the surface RAS (c_(ras) = 0) for surface vertex position\n");
  fprintf(stderr, "                       : "
          "The default is to use the scanner RAS coordinate c_(ras) != 0\n");
  fprintf(stderr, "\n-surf_debug          : "
          "visualize the surfaces onto the output volume");
  fprintf(stderr, "\n-surf surfname       : save the BEM surfaces");
  fprintf(stderr, "\n-brainsurf surfname  : save the brain surface");
  fprintf(stderr, "\n-shk_br_surf int_h surfname : "
          "to save the brain surface shrank inward of int_h mm");
  fprintf(stderr, "\n-s int_i int_j int_k : add a seed point (range 0-255)");
  fprintf(stderr, "\n-c int_i int_j int_k : "
          "specify the center of the brain (in voxel unit)");
  fprintf(stderr, "\n-r int_r             : "
          "specify the radius of the brain (in voxel unit)");
  fprintf(stderr, "\n-t int_threshold     : "
          "change the threshold in the watershed analyze process");
  fprintf(stderr, "\n-h int_hpf           : "
          "precize the preflooding height (in percent)");
  fprintf(stderr, "\n-n                   : "
          "not use the watershed analyze process");
  fprintf(stderr, "\n-LABEL               : "
          "labelize the output volume into scalp, skull, csf, gray and white");
  fprintf(stderr, "\n-man int_csf int_trn int_gray: "
          "to change the different parameters csf_max, "
          "transition_intensity and GM_intensity");
  fprintf(stderr, "\n-dark int_niter      : "
          "go to the closest darkest point during int_niter "
          "iterations (default 10)");
  fprintf(stderr, "\n-mask                : "
          "mask a volume with the brain mask");
  fprintf(stderr, "\n\n--help               : show this usage message");
  fprintf(stderr, "\n--version            : show the current version\n\n");
}

/*-----------------------------------------------------
  Parameters:   argc, char *argv[], STRIP_PARMS *parms
  Returns   :   number of options read
  Description:  read the different options of the command line
  ------------------------------------------------------*/

static int
get_option(int argc, char *argv[],STRIP_PARMS *parms)
{
  int  nargs = 0 ;
  char *option ;
  
  option = argv[1] + 1 ;            /* past '-' */
  if (!strcmp(option, "-help"))
    {
      usageHelp();
      exit(0);
    }
  else if (!strcmp(option, "more"))
    {
      parms->skull_type=1;
      nargs = 0 ;
      fprintf(stderr,"Mode:          surface expanded") ;
    }
  else if (!strcmp(option, "T1"))
    {
      parms->T1=1;
      nargs = 0 ;
      fprintf(stderr,"Mode:          T1 normalized volume\n") ;
    }
  else if (!strcmp(option, "noT1"))
    {
      parms->noT1analysis = 1;
      nargs = 0;
      fprintf(stderr,"Mode:          no T1-segment-analysis is done\n") ;
    }
  else if (!strcmp(option, "useSRAS"))
    {
      parms->useSRAS=1;
      // change the global function ptrs
      myWorldToVoxel = MRIsurfaceRASToVoxel;
      myVoxelToWorld = MRIvoxelToSurfaceRAS;
      //
      nargs = 0;
      fprintf(stderr,"Mode:          "
              "use surfaceRAS to save surface vertex positions\n");
    }
  else if (!strcmp(option, "less"))
    {
      parms->skull_type=-1;
      nargs = 0 ;
      fprintf(stderr,"Mode:          surface shrunk\n") ;
    }
  else if (!strcmp(option, "wat+temp"))
    {
      parms->template_deformation=2;
      fprintf(stderr,"Mode:          "
              "watershed algorithm + first template smoothing\n") ;
      nargs = 0 ;
    }
  else if (!strcmp(option, "first_temp"))
    {
      parms->template_deformation=3;
      fprintf(stderr,"Mode:          "
              "watershed algorithm + first template smoothing + "
              "local matching\n") ;
      nargs = 0 ;
    }
  else if (!strcmp(option, "mask"))
    {
      if (nmask_volumes >= MAX_MASK_VOLUMES)
        ErrorExit(ERROR_UNSUPPORTED, "%s: too many mask volumes "
                  "specified (max=%d)\n",
                  MAX_MASK_VOLUMES) ;
      strcpy(mask_in_fnames[nmask_volumes], argv[2]) ;
      strcpy(mask_out_fnames[nmask_volumes++], argv[3]) ;
      printf("Mode:          masking volume %s with brain mask "
             "and writing it to %s...\n", argv[2], argv[3]) ;
      nargs = 2 ;
    }
  else if(!strcmp(option, "LABEL"))
    {
      parms->label=1;
      parms->brainsurf=1;
      parms->surf=1;
      if(parms->surfname==NULL)
        parms->surfname="./";
      fprintf(stderr,"Mode:          Writing out tissue label "
              "into output volume\n") ;
      fprintf(stderr,"                       Assumption : "
              "no biais field and FGM\n");
      fprintf(stderr,"                       0 -> exterior\n") ;
      fprintf(stderr,"                       1 -> scalp\n") ;
      fprintf(stderr,"                       2 -> skull\n") ;
      fprintf(stderr,"                       3 -> csf\n") ;
      fprintf(stderr,"                       4 -> gray\n") ;
      fprintf(stderr,"                       5 -> white\n") ;
      fprintf(stderr,"                       6 -> fat tissue\n") ;
      nargs = 0 ;
    }
  else if(!strcmp(option, "surf_debug"))
    {
      parms->surf_dbg=1;
      fprintf(stderr,"Mode:          "
              "Writing out surfaces into output volume\n") ;
      nargs = 0 ;
    }
  else if(!strcmp(option, "brainsurf"))
    {
      parms->brainsurf=1;
      parms->surfname=argv[2];
      fprintf(stderr,"Mode:          Saving brain surface\n") ;
      nargs = 1 ;
    }
  else if(!strcmp(option, "surf"))
    {
      parms->brainsurf=1;
      parms->surf=1;
      parms->surfname=argv[2];
      fprintf(stderr,"Mode:          Saving out BEM surfaces\n") ;
      nargs = 1 ;
    }
  else if(!strcmp(option, "shk_br_surf"))
    {
      parms->brainsurf=1;
      parms->h_shk=atoi(argv[2]);
      parms->surfname=argv[3];
      fprintf(stderr,"Mode:          Saving shrank brain surface\n") ;
      nargs = 2 ;
    }
  else if(!strcmp(option, "dark"))
    {
      parms->dark_iter=atoi(argv[2]);
      fprintf(stderr,"Mode:          "
              "moving to closest darkest points during %d iterations\n",
              parms->dark_iter) ;
      nargs = 1 ;
    }
  else if(!strcmp(option, "wat"))
    {
      parms->template_deformation=0;
      fprintf(stderr,"Mode:          Watershed algorithm only\n") ;
      nargs = 0 ;
    }
  else if(!strcmp(option, "atlas"))
    {
      parms->atlas=1;
      fprintf(stderr,"Mode:          Atlas analysis\n") ;
      nargs = 0 ;
    }
  else if(!strcmp(option, "man"))
    {
      fprintf(stderr,"Mode:          Modification of the local parameters\n") ;
      parms->manual_params=1;
      parms->manual_CSF_MAX=atoi(argv[2]);
      parms->manual_TRANSITION_intensity=atoi(argv[3]);
      parms->manual_GM_intensity=atoi(argv[4]);
      nargs=3;
    }
  // check one character options -s, -c, -r, -t, -n
  else if (strlen(option) == 1)
    {
      switch (toupper(*option))
        {
        case 'N':
          parms->watershed_analyze=0;
          nargs = 0 ;
          fprintf(stderr,"Mode:          No watershed analyze\n") ;
          break ;
        case 'S':
          if(parms->nb_seed_points>=30)
            Error("\ntoo many seed points\n");
          if (argc < 7)
            Error("\n-s option needs 3 seed points, input, output argument\n");
	  parms->seed_coord[parms->nb_seed_points][0] = atoi(argv[2]);
	  parms->seed_coord[parms->nb_seed_points][1] = atoi(argv[3]);
	  parms->seed_coord[parms->nb_seed_points][2] = atoi(argv[4]);
          if (parms->seed_coord[parms->nb_seed_points][0] < 0)
            Error("\nseed value 'i' out of range 0-255 \n");
          if (parms->seed_coord[parms->nb_seed_points][1] < 0)
            Error("\nseed value 'j' out of range 0-255 \n");
          if (parms->seed_coord[parms->nb_seed_points][2] < 0)
            Error("\nseed value 'k' out of range 0-255 \n");
          nargs=3;
          parms->nb_seed_points++;
          break ;
        case 'C':
          fprintf(stderr,"Mode:          Brain Center manually specified\n") ;
          if (argc < 7)
            Error("\n-c option needs 3 coordinate of the brain center, "
                  "input, and output argument\n");
          parms->cx=atoi(argv[2]);
          parms->cy=atoi(argv[3]);
          parms->cz=atoi(argv[4]);
          nargs=3;
          break ;
        case 'R':
          fprintf(stderr,"Mode:          Brain Radius manually specified\n") ;
          if (argc < 5)
            Error("\n-r needs radius, input, and output argument.\n");
          parms->rb=atoi(argv[2]);
          nargs=1;
          break ;
        case 'H':
          if (argc < 5)
            Error("\n-h needs preflooding height, input, "
                  "and output argument.\n");
          parms->hpf=atoi(argv[2]);
          nargs=1;
          break ;
        case 'T':
          if (argc < 5)
            Error("\n-t needs threshold, input, and output argument.\n");
          parms->threshold_analyze=atoi(argv[2]);
          fprintf(stderr,"Mode:          "
                  "Threshold changed to %d\n",parms->threshold_analyze) ;
          nargs = 1 ;
          break ;
        default:
          printf("Mode:          unknown option %s\n", argv[1]) ;
          usageHelp();
          exit(1) ;
          break ;
        }
    }
  else
    {
      printf("Mode:          unknown option %s\n", argv[1]) ;
      usageHelp();
      exit(1) ;
    }
  return(nargs) ;
} 

/*-----------------------------------------------------
  Parameters:message error
  Returns value:void
  Description: Error routine - stop the prog
  ------------------------------------------------------*/
static void Error(char *string)
{
  fprintf(stderr, "\nmri_watershed Error: %s\n",string) ;
  exit(1) ;
}


void writeSurface(char *fname, MRI_variables *var, STRIP_PARMS *parms)
{
  if (parms->useSRAS)
    var->mris->useRealRAS = 0;
  else
    var->mris->useRealRAS = 1;
  MRISwrite(var->mris, fname);
}

////////////////////////////////////////////////////////////////////////
// main
////////////////////////////////////////////////////////////////////////
int main(int argc, char *argv[])
{
  char  *in_fname, *out_fname;
  int nargs;
  MRI *mri_with_skull, *mri_without_skull=NULL, *mri_mask;

  STRIP_PARMS *parms;
  char cmdline[CMD_LINE_LEN] ;
        
  make_cmd_version_string 
    (argc, argv, 
"$Id: mri_watershed.cpp,v 1.38 2005/11/02 16:29:44 kteich Exp $", "$Name:  $",
     cmdline);

  Progname=argv[0];

  parms=init_parms();

  /************* Command line****************/

  /* rkt: check for and handle version tag */
  nargs = handle_version_option (argc, argv, 
"$Id: mri_watershed.cpp,v 1.38 2005/11/02 16:29:44 kteich Exp $", "$Name:  $");
  if (nargs && argc - nargs == 1)
    exit (0);
  argc -= nargs;

  fprintf(stderr,"\n");

  for ( ; argc > 1 && ISOPTION(*argv[1]) ; argc--, argv++)
    {
      nargs = get_option(argc, argv,parms) ;
      argc -= nargs ;
      argv += nargs ;
    }

  if(argc<3)
    {
      usageHelp();
      exit(1);
    };

  in_fname = argv[argc-2];  
  out_fname = argv[argc-1];

  fprintf(stderr,"\n*********************************************************"
          "\nThe input file is %s"
          "\nThe output file is %s"
          "\nIf this is incorrect, please exit quickly the program "
          "(Ctl-C)\n\n",in_fname,out_fname);

  
  /*************** PROG *********************/

  /* initialisation */
  // readin input volume
  mri_with_skull = MRIread(in_fname) ;
  if (!mri_with_skull)
    Error("read failed\n");
  MRIaddCommandLine(mri_with_skull, cmdline) ;

  if (mri_with_skull->type!=MRI_UCHAR)
    {
      MRI *mri_tmp ;
    
      type_changed = 1 ; old_type = mri_with_skull->type ;
      printf("changing type of input volume to 8 bits/voxel...\n") ;
      mri_tmp = MRIchangeType(mri_with_skull, MRI_UCHAR, 0.0, 0.999, FALSE) ;
      MRIfree(&mri_with_skull) ; 
      mri_with_skull = mri_tmp ;
    }
  else
    type_changed = 0 ;
  
  /* Main routine *********************/
  // mri_with_skull is UCHAR volume, mri_without_skull = NULL at this time
  mri_without_skull=MRIstripSkull(mri_with_skull, mri_without_skull,parms);
  if (mri_without_skull == NULL)
    {
      printf("\n**********************************************************\n");
      printf("         MRIstripSkull failed.\n");
      printf("************************************************************\n");
      free(parms);
      return -1;
    }
  if (type_changed)  /* make output volume the same type as input volume */
    {
      mri_with_skull = MRIread(in_fname) ;
      if (!mri_with_skull)
        Error("read failed\n");
      mri_mask = mri_without_skull ;
      mri_without_skull = MRImask(mri_with_skull, mri_mask, NULL, 0, 0) ;
    }
  else
    mri_mask = mri_without_skull ;


  fprintf(stderr,"\n\n******************************\nSave...");

  MRIwrite(mri_without_skull,out_fname);
  MRIfree(&mri_with_skull) ;
     
  fprintf(stderr,"done\n");
  
  if (nmask_volumes > 0)
    {
      int i ;
    
      for (i = 0 ; i < nmask_volumes ; i++)
        {
          mri_with_skull = MRIread(mask_in_fnames[i]) ;
          if (!mri_with_skull)
            ErrorExit(ERROR_NOFILE, "%s: could not read volume %s",
                      Progname, mask_in_fnames[i]) ;
          mri_without_skull = MRImask(mri_with_skull, mri_mask, NULL, 0, 0) ;
          printf("writing skull stripped volume to %s...\n",
                 mask_out_fnames[i]) ;
          MRIwrite(mri_without_skull, mask_out_fnames[i]) ;
          MRIfree(&mri_with_skull) ; 
          MRIfree(&mri_without_skull) ;
        }
    }
  
  MRIfree(&mri_mask) ;

  free(parms);

  return 0;
}


/*-----------------------------------------------------
  Parameters:void

  Returns value:STRIP_PARMS*

  Description: allocate the structure containing some parameters 
  necessary for the program
  ------------------------------------------------------*/

static STRIP_PARMS* init_parms(void)
{
  STRIP_PARMS* sp=(STRIP_PARMS*)calloc(1,sizeof(STRIP_PARMS));
  
  /*preflooding height used in the watershed segmentation*/
  sp->hpf=25;  
   
  /* no writing out of the different surfaces into the ouput volume
     usefull for understanding where the errors come from*/
  sp->surf_dbg=0;   
  /* no brain bem surface saving*/
  sp->brainsurf=0;     
  /*no bem surfaces saving*/
  sp->surf=0;     
  /*no shrank bem brain surface*/
  sp->h_shk=0;  
  /*level of surface: shrunk:-1, normal:0 or expanded:1 */
  sp->skull_type=0;  
  /*no labelization of the tissue into the output volume*/
  sp->label=0; 
  /*  mode post-watershed analyze turned on*/
  sp->watershed_analyze=1;  
  /*post-watershed analyze threshold set to 100%*/
  sp->threshold_analyze=100;
  /*no seed points*/
  sp->nb_seed_points=0;
  /*mode template deformation on*/
  sp->template_deformation=1;
  /*surface outfile name NULL*/
  sp->surfname=NULL;

  /*no manual parameters entered*/
  sp->manual_params=0;
  /*no atlas analysis*/
  sp->atlas=0;

  /*no T1 volume normalization*/
  sp->T1=0;
  /* don't do T1 analysis */
  sp->noT1analysis = 0;

  sp->dark_iter=10;

  /*no input brain parms*/
  sp->cx=-1;
  sp->rb=-1;

  // not to use surface RAS
  sp->useSRAS = 0;

  // default values
  myWorldToVoxel = MRIworldToVoxel;
  myVoxelToWorld = MRIvoxelToWorld; 

  return(sp);
}

/*-----------------------------------------------------
  Parameters: MRI *mri_with_skull (input image)

  Returns value:MRI_variables *MRI_Var

  Description: allocate the structure containing some important 
  variables necessary for the program 
  ------------------------------------------------------*/

static MRI_variables* init_variables(MRI *mri_with_skull)
{
  MRI_variables* v=(MRI_variables*)calloc(1,sizeof(MRI_variables));

  v->mris=NULL;
  v->mris_curv=NULL;
  v->mris_var_curv=NULL;
  v->mris_dCOG=NULL;
  v->mris_var_dCOG=NULL;
  v->mrisphere=NULL;

  v->i_global_min=0;
  v->estimated_size=0;
  v->main_basin_size=0;
  v->brain_size=0;

  v->width=mri_with_skull->width;
  v->height=mri_with_skull->height;
  v->depth=mri_with_skull->depth;
  
  v->mri_orig=mri_with_skull;
  
  v->T1Table=NULL;
  v->T1nbr=0;

  v->Imax = 0;
  v->WM_intensity=0;
  v->WM_VARIANCE=0;
  v->WM_HALF_MAX = 0;
  v->WM_HALF_MIN = 0;
  v->WM_MAX=0;
  v->WM_MIN=0;
  v->CSF_intensity=0;
  v->CSF_HALF_MAX=0;
  v->CSF_MAX = 0;
  v->CSF_MIN = 0;
  v->GM_MIN = 0;
  v->GM_intensity = 0;
  v->TRANSITION_intensity=0;
  v->dark_iter=10;

  return v;

}

/*-----------------------------------------------------
  FUNCTION MRIstripSkull
  
  Parameters:
  MRI *mri_with_skull:orig input image (orig or T1 T1-weigthed volume) 
  MRI *mri_without_skull: output volume (could be NULL)
  STRIP_PARMS *parms: coudl be NULL

  Returns value:
  MRI *mri_without_skull: (if input NULL, after allocation)
    
  Description: strip the skull from the input image 
  ------------------------------------------------------*/

MRI *MRIstripSkull(MRI *mri_with_skull, MRI *mri_without_skull, 
                   STRIP_PARMS *parms)
{
  char fname[512];
  MRI_variables *MRI_var;
  MRI *mri_tp;
  double vol_elt;

  if(mri_with_skull==NULL)
    Error("\nNULL input volume !\n");
  if(mri_with_skull->type!=0)
    Error("\nThe type of the input file is not 0 : UCHAR\n"); 

  // cache the original input
  mri_tp=MRIclone(mri_with_skull,NULL);
  mri_tp=MRIcopy(mri_with_skull,NULL);

  if(!mri_without_skull)
    mri_without_skull=MRIclone(mri_with_skull,NULL); // just copy to output
  else if(mri_without_skull->type!=mri_with_skull->type)
    Error("\ndifferent types of mri structures...\n");

  // mri_orig = mri_with_skull
  MRI_var=init_variables(mri_with_skull);
  MRI_var->verbose_mode=parms->surf_dbg;

  // mri_src = mri_with_skull, mri_dst = mri_with_skull
  MRI_var->mri_src=mri_tp;
  MRI_var->mri_dst=mri_without_skull;

  /*watershed process*/
  if (Watershed(parms,MRI_var)==-1)
    {
      // free(mri_tp);
      MRIfree(&mri_tp);
      return NULL;  
    }
  /*Only the watershed algorithm*/
  if(!parms->template_deformation)
    {
      /*in case the src volume was modified (scaling of the intensity)*/
      // free(mri_tp);
      MRIfree(&mri_tp);
      mri_tp=MRIclone(mri_with_skull,NULL);
      mri_tp=MRIcopy(mri_with_skull,NULL);
      MRI_var->mri_src = mri_tp ;

      Save(MRI_var);
      FreeMem(MRI_var);
    }
  else                   /*template deformation process*/
    {
      if (parms->surf_dbg)
        {
          MRI_var->mri_dst=MRIclone(mri_with_skull,NULL);
          MRI_var->mri_dst=MRIcopy(mri_with_skull,NULL);
        }
      ////////////////////////////////////////////////////////////////////
      // gaussian step 
      // MRI *gkernel = MRIgaussian1d(16.f, 50);
      // MRI *smooth = MRIconvolveGaussian(MRI_var->mri_src, NULL, gkernel);
      // MRI_var->mri_src = smooth;
      // MRIfree(&gkernel);
    
      /*template process*/
      ////////////////////////////////////////////////////////////////////
      //make sure that the global minimum will not influence the analysis
      if(MRI_var->decision){
        int i,j,k;
        float scale=MRI_var->scale;
        for(k=0;k<MRI_var->depth;k++)
          for(j=0;j<MRI_var->height;j++)
            for(i=0;i<MRI_var->width;i++)
              MRIvox(MRI_var->mri_src,i,j,k)=
                (unsigned short)MIN(255,scale*MRIvox(mri_with_skull,i,j,k));
      }else{
        int i,j,k;
        for(k=0;k<MRI_var->depth;k++)
          for(j=0;j<MRI_var->height;j++)
            for(i=0;i<MRI_var->width;i++)
              MRIvox(MRI_var->mri_src,i,j,k)=MRIvox(mri_with_skull,i,j,k);
      }


      Template_Deformation(parms,MRI_var); 
      // MRIfree(&smooth);

      /*in case the src volume was modified (scaling of the intensity)*/
      // free(mri_tp);
      MRIfree(&mri_tp);
      mri_tp=MRIclone(mri_with_skull,NULL);
      mri_tp=MRIcopy(mri_with_skull,NULL);
      MRI_var->mri_src = mri_tp ;

      MRI_var->brain_size=MRISpeelBrain(0,MRI_var->mri_src,MRI_var->mris,0);
      // mri_src is modified (0 outside of the surface).
      vol_elt
        =MRI_var->mri_src->xsize*MRI_var->mri_src->ysize*
        MRI_var->mri_src->zsize;
      fprintf(stderr,"\n\nBrain Size = %ld voxels, voxel volume = %2.3f mm3\n"
              ,MRI_var->brain_size,(float)vol_elt);
      fprintf(stderr,"           = %.0f mmm3 = %.3f cm3\n"
              ,MRI_var->brain_size*vol_elt,
              (float)MRI_var->brain_size/1000.*vol_elt);

      /*save the surface of the brain*/
      if(parms->brainsurf)
        {

          if(parms->surf || parms->h_shk)
            MRISsmooth_surface(MRI_var->mris,5); // smooth 5 times
      
          if(parms->h_shk != 0)
            MRISshrink_surface(MRI_var->mris,parms->h_shk);
           
          /*writing out the surface*/
          sprintf(fname,parms->surfname);
          strcat(fname,"_brain_surface");
          //MRISwrite(MRI_var->mris,fname);
          writeSurface(fname, MRI_var, parms);
        }
    }
 
  /*find and write out the surfaces of the inner skull, scalp and outer skull*/
  if(parms->template_deformation && parms->surf)
    {
      // so far we got the brain surface
      //inner skull
      MRISshrink_surface(MRI_var->mris,-3); // goes outward by 3 mm
      MRISsmooth_surface(MRI_var->mris,5);  // smooth surface 5 times 
    
      if (parms->surf_dbg)
        write_image(MRI_var); 

      /*writing out the inner skull surface*/
      sprintf(fname,parms->surfname);
      strcat(fname,"_inner_skull_surface");
      // MRISwrite(MRI_var->mris,fname);
      writeSurface(fname, MRI_var, parms);

      //scalp  
      MRISfree(&MRI_var->mris);
      read_geometry(1,MRI_var,NULL);

      init_surf_to_image(1.8*MRI_var->rad_Brain,1.8*
                         MRI_var->rad_Brain,1.8*MRI_var->rad_Brain,MRI_var);

      fprintf(stderr,"\n      outer skin surface matching...");
      MRISshrink_Outer_Skin(MRI_var,mri_with_skull);

      MRISsmooth_surface(MRI_var->mris,3);
    
      if (parms->surf_dbg)
        write_image(MRI_var); 

      /*writing out the surface*/
      sprintf(fname,parms->surfname);
      strcat(fname,"_outer_skin_surface");
      // MRISwrite(MRI_var->mris,fname);
      writeSurface(fname, MRI_var, parms);

      //outer skull
      MRISsmooth_surface(MRI_var->mris,3); // smooth 3 times
      MRISshrink_surface(MRI_var->mris,3); // shrink 3 mm 
      MRISsmooth_surface(MRI_var->mris,5); // smoth 5 times

      if (parms->surf_dbg)
        write_image(MRI_var); 

      /*writing out the outer skull surface*/
      sprintf(fname,parms->surfname);
      strcat(fname,"_outer_skull_surface");
      // MRISwrite(MRI_var->mris,fname);
      writeSurface(fname, MRI_var, parms);

      if(parms->label)
        label_voxels(parms,MRI_var,mri_with_skull);
     
    }
  /*save the volume with the surfaces written in it*/
  /*used to visualize the surfaces -> debuging */
  if(parms->template_deformation && parms->surf_dbg)
    mri_without_skull=MRIcopy(MRI_var->mri_dst,NULL);
  /*normal mode saving*/
  else
    mri_without_skull=MRIcopy(MRI_var->mri_src,NULL);


  MRIVfree(MRI_var);
  return mri_without_skull;
}
/******************************************************
 ******************************************************
 *****************************************************/
/*-----------------------------------------------------
  FUNCTION Watershed
  
  Parameters:
  STRIP_PARMS *:contains the parameters for the prog 
  MRI_variables *: contains the variables 

  Returns value:void

  Description: watershed segment the input volume *mri_src 
  ------------------------------------------------------*/

static int Watershed(STRIP_PARMS *parms,MRI_variables *MRI_var)
{
 
  fprintf(stderr,
          "\n*************************WATERSHED**************************");

  if(parms->nb_seed_points)
    fprintf(stderr, "\n%d more seed points created",parms->nb_seed_points);
  fprintf(stderr,"\npreflooding height equal to %d percent", parms->hpf) ;

  fprintf(stderr,"\nSorting...");
  if (!parms->noT1analysis)
    AnalyzeT1Volume(parms,MRI_var);
  Allocation(MRI_var);
  if (Pre_CharSorting(parms,MRI_var)==-1)
    return -1;
    
  CharSorting(MRI_var);  
  fprintf(stderr,"\ndone");

  fprintf(stderr,"\nAnalyze\n");
  Analyze(parms,MRI_var);
  fprintf(stderr,"\ndone");

  fprintf(stderr,"\nPostAnalyze...");
  PostAnalyze(parms,MRI_var);
  fprintf(stderr,"done\n");

  return 0;
}


static void AnalyzeT1Volume(STRIP_PARMS *parms,MRI_variables *MRI_var)
{
  /*intensity between 100 and 119*/
  int i,j,k;
  long T1number[20]; 
  double average;
  BUFTYPE *pb;

  for(k=0;k<20;k++)
    T1number[k]=0;

  /*detect if we are in a T1 volume*/
  for(k=2;k<MRI_var->depth-2;k++)
    for(j=2;j<MRI_var->height-2;j++)
      {
        pb=&MRIvox(MRI_var->mri_src,0,j,k);
        pb+=2;
        for(i=2;i<MRI_var->width-2;i++)
          {
            if (((*pb)>99) && ((*pb)<120))
              {
                T1number[(*pb)-100]++;
              }
            pb++;
          }
      }

  /*look if intensity=110 > average around*/
  average=0;
  for(k=0;k<10;k++)
    average+=T1number[k];
  for(k=11;k<20;k++)
    average+=T1number[k];
  average/=19;
  if(T1number[10]>5*average)
    parms->T1=1;
    
  // if T1 is set
  if(parms->T1)
    {
      fprintf(stderr,"\n      T1-weighted MRI image");
      if(parms->hpf==25)
        {
          parms->hpf=15;
          fprintf(stderr,
                  "\n      modification of the "
                  "preflooding height to %d percent", parms->hpf) ;
        }
      FindMainWmComponent(MRI_var);
    }
}

static void FindMainWmComponent(MRI_variables *MRI_var)
{
  MRI_SEGMENTATION *mri_segmentation;
  MRI_SEGMENT *mri_seg;
  int k,m,max;
  long maxarea;
  int wmcount = 0;
  fprintf(stderr,"\n      Count how many 110 voxels are present : ");
  for (int k=0; k < MRI_var->mri_orig->depth; ++k)
    for (int j=0; j < MRI_var->mri_orig->height; ++j)
      for (int i=0; i < MRI_var->mri_orig->width; ++i)
        if (MRIvox(MRI_var->mri_orig, i, j, k) == WM_CONST)
          wmcount++;
  fprintf(stderr," %d\n", wmcount);

  fprintf(stderr,"\n      Find the largest 110-component...");
  mri_segmentation=MRImaxsegment(MRI_var->mri_orig,WM_CONST,WM_CONST);
  fprintf(stderr,"done"
          "\n      And identify it as the main brain basin...");
  
  // find the region with the largest area
  maxarea=-1;
  max=-1;
  for(k=0;k<mri_segmentation->max_segments;k++)
    if(mri_segmentation->segments[k].nvoxels>maxarea)
      {
        max=k;
        maxarea=mri_segmentation->segments[k].nvoxels;
      }

  // assign to T1table
  mri_seg=&mri_segmentation->segments[max];
  MRI_var->T1Table=(Coord*)calloc(maxarea,sizeof(Coord));
  MRI_var->T1nbr=maxarea;

  if(!MRI_var->T1Table)
    Error("\nCould not allocate coord table");
  for(m=0;m<maxarea;m++)
    {
      MRI_var->T1Table[m][0]=mri_seg->voxels[m].x;
      MRI_var->T1Table[m][1]=mri_seg->voxels[m].y;
      MRI_var->T1Table[m][2]=mri_seg->voxels[m].z;
    }
  fprintf(stderr,"done");
  fprintf(stderr,"\n      Main component: %ld voxels",maxarea);
  if(MRIsegmentFree(&mri_segmentation)!=NO_ERROR)
    Error("\nCouldn't free the memory allocated during MRI_segmentation"); 
}



/*-----------------------------------------------------  
  Parameters: 

  Returns value:void

  Description: Allocation of a table of basins
  ------------------------------------------------------*/
static void Allocation(MRI_variables *MRI_var)
{
  int k,j;

  MRI_var->Basin=(Cell ***)malloc(MRI_var->depth*sizeof(Cell **));
  if (!MRI_var->Basin)
    Error("first allocation  failed\n");
  
  for(k=0;k<MRI_var->depth;k++)
    {
      MRI_var->Basin[k]=(Cell **)malloc(MRI_var->height*sizeof(Cell*));
      if (!MRI_var->Basin[k])
        Error("second allocation  failed\n");
      for(j=0;j<MRI_var->height;j++)
        {
          MRI_var->Basin[k][j]=(Cell*)calloc(MRI_var->width,sizeof(Cell));
          if (!MRI_var->Basin[k][j])
            Error("third alloc failed\n");
        }  
    }

  for(k=0;k<256;k++)
    {
      MRI_var->tabdim[k]=0;
      MRI_var->sqrdim[k]=0;
      MRI_var->count[k]=0;
      MRI_var->intbasin[k]=k;
      MRI_var->gmnumber[k]=0;
    }
}

//
// calculate CSF_intensity
// 
static int calCSFIntensity(MRI_variables *MRI_var)
{
  int i, j, k;
  int n;
  double intensity_percent[256];
  BUFTYPE *pb;

  /*First estimation of the CSF */
  for(k=0;k<256;k++)
    intensity_percent[k]=0;
  
  n=0; // counts non-zero grey voxels
  // create a histogram
  for(k=2;k<MRI_var->depth-2;k++)
    for(j=2;j<MRI_var->height-2;j++)
      {
        pb=&MRIvox(MRI_var->mri_src,0,j,k);
        pb+=2;
        for(i=2;i<MRI_var->width-2;i++)
          {
            if (*pb) // non-zeo
              {
                n++;
                intensity_percent[*pb]++;
              }
            pb++;
          }
      }

  DebugCurve(intensity_percent, 256, "\nHistogram of grey values\n");

  // accumulate histogram 
  for(k=1;k<256;k++)
    {
      intensity_percent[k]+=intensity_percent[k-1];
      /*  fprintf(stderr," %d %f ",k,intensity_percent[k]*100/n);*/
      // the max grey value to be 99.8% of the population
      // CSF_intensity to be 10% of that value.
      if (intensity_percent[k]*100<=99.8*n)
        MRI_var->CSF_intensity=k/10+1;
    }
  return 0;
}

// COG in terms of voxel coordinates
static int calCOGMAX(MRI_variables *MRI_var, 
                     STRIP_PARMS *parms, 
                     int *x, int *y, int *z)
{
  int i, j, k;
  int n, m;
  double intensity_percent[256];
  BUFTYPE *pb;
  int T1 = parms->T1;

  /*Ignore everything which is bellow CSF_intensity
    Then first estimation of the COG coord 
    Find a cube in which it will search for WM */
  for(k=0;k<256;k++)
    {
      intensity_percent[k]=0;
    }

  n=0; // keeps track of non-zero voxels
  m=0; // keeps track of the center of gravity voxel
  MRI_var->xCOG = MRI_var->yCOG = MRI_var->zCOG = 0;
  int maxGrey =0;
  for(k=2;k<MRI_var->depth-2;k++)
    for(j=2;j<MRI_var->height-2;j++)
      {
        pb=&MRIvox(MRI_var->mri_src,0,j,k);
        pb+=2;
        for(i=2;i<MRI_var->width-2;i++)
          {
            if (*pb>MRI_var->CSF_intensity)
              {
                n++; 
                intensity_percent[*pb]++;
                if (!T1) // not T1 volume
                  {
                    MRI_var->xCOG+=i;
                    MRI_var->yCOG+=j;
                    MRI_var->zCOG+=k;
                    m++;
                  }
                else 
                  {
                    // this is done to avoid COG becoming too low
                    // due to the large neck area
                    if (*pb == 110) // T1 volume
                      {
                        MRI_var->xCOG+=i;
                        MRI_var->yCOG+=j;
                        MRI_var->zCOG+=k;
                        m++;
                      }
                  }
                if (*pb > maxGrey)
                  maxGrey = *pb;
              }
            pb++;
          }
      }
  if(m==0)
    Error("\n Problem in the COG calculation ");

  MRI_var->xCOG/=m;  // m is used here
  MRI_var->yCOG/=m;
  MRI_var->zCOG/=m;

  *x=(int)(MRI_var->xCOG+0.5);
  *y=(int)(MRI_var->yCOG+0.5);
  *z=(int)(MRI_var->zCOG+0.5);

  // calculate Imax
  MRI_var->Imax=0;
  for (k=1;k<256;k++)
    {
      intensity_percent[k]+=intensity_percent[k-1];  
      if (intensity_percent[k]*100<=n*MAX_INT)  // n is used here
        MRI_var->Imax=k;
    } 
  return 0;
}

// using voxels whose value > CSF_intensity to calculate brain radius
// again in voxel unit
static int calBrainRadius(MRI_variables *MRI_var)
{
  int m=0;
  int i,j,k;
  BUFTYPE *pb;

  m=0; 
  MRI_var->rad_Brain=0;
  for(k=2;k<MRI_var->depth-2;k++)
    for(j=2;j<MRI_var->height-2;j++)
      {
        pb=&MRIvox(MRI_var->mri_src,0,j,k);
        pb+=2;
        for(i=2;i<MRI_var->width-2;i++)
          {
            if((*pb)>=MRI_var->Imax) 
              *pb=MRI_var->Imax-1;
            if (*pb)      /*don't care about 0 intensity voxel*/
              MRI_var->tabdim[*pb]++; // histogram of non-zero voxels
            if(*pb>MRI_var->CSF_intensity)
              {
                m++;
                MRI_var->rad_Brain+=SQR(i-MRI_var->xCOG)+
                  SQR(j-MRI_var->yCOG)+SQR(k-MRI_var->zCOG);
              }
            pb++;
          }
      }

  if(m==0)
    Error("\n Problem with the radius calculation ");

  MRI_var->rad_Brain=sqrt(MRI_var->rad_Brain/m);

  return 0;
}
/*-----------------------------------------------------  
  int Pre_CharSorting

  Parameters: 

  Returns value:int

  Description: Calculate some rough statistics
  Estimate the white matter parameters
  Create a global minimum in the WM
  ------------------------------------------------------*/
static int Pre_CharSorting(STRIP_PARMS *parms,MRI_variables *MRI_var)
{
  int retVal;
  int i,j,k,n,m,u,v;
  int ig,jg,kg;
  BUFTYPE *pb,*pbc[3][3];
  unsigned long wmint=0,wmnb=0;
  unsigned long number[256];
  double intensity_percent[256];
  float ***mean_val,mean,min,max;
  float ***var_val,var;
  float ***mean_var;
  int x,y,z,r;
  int xmin,xmax,ymin,ymax,zmin,zmax,mint;
  float tmp;
  MRI *mri_tmp;

  // calculate CSF_intensity from voxel values (CSF_intensity)
  calCSFIntensity(MRI_var);
  
  // calculate initial estimate of COG coords and 
  // Imax (xCOG, yCOG, zCOG, and Imax)
  // in voxel unit
  calCOGMAX(MRI_var, parms, &x, &y, &z);

  // calculate intitial estimate of brain radius (rad_Brain)
  // in voxel unit
  calBrainRadius(MRI_var);

  // set r to be the half the brain size
  r=(int)(MRI_var->rad_Brain/2);

  fprintf(stderr,
          "\n      first estimation of the COG coord: x=%d y=%d z=%d r=%d",
          (int)MRI_var->xCOG,(int)MRI_var->yCOG,(int)MRI_var->zCOG,
          (int)MRI_var->rad_Brain);
 
  // option set by user (-c i j k)
  if(parms->cx!=-1)
    {
      MRI_var->xCOG=parms->cx;
      MRI_var->yCOG=parms->cy;
      MRI_var->zCOG=parms->cz;
      fprintf(stderr,"\n      modification of the brain COG: x=%d y=%d z=%d",
              (int)MRI_var->xCOG,(int)MRI_var->yCOG,(int)MRI_var->zCOG);
      x=(int)(MRI_var->xCOG+0.5); 
      y=(int)(MRI_var->yCOG+0.5);
      z=(int)(MRI_var->zCOG+0.5);
    }
  // option set by user (-r radius)
  if(parms->rb!=-1)
    {
      MRI_var->rad_Brain=parms->rb;
      r=(int)(MRI_var->rad_Brain/2);
      fprintf(stderr,"\n      modification of the brain radius to %d",
              (int)MRI_var->rad_Brain);
    }

  MRI_var->estimated_size=(unsigned long)(4.19*pow(MRI_var->rad_Brain,3));

  fprintf(stderr,"\n      first estimation of the main basin volume: "
          "%ld voxels",MRI_var->estimated_size);


  mint=MIN(MRI_var->width,MIN(MRI_var->height,MRI_var->width));
  if(20*r>=mint*9){
    fprintf(stderr,"\n main radius too high");
    //    Error("\n main radius too high");
  }

  /*allocate the Cube memory: mean intensity, variance, mean variance */

  // now the r is the brain radius
  r*=2;
  // array size of (brain radius)^3
  mean_val=(float***)malloc(r*sizeof(float**));
  var_val=(float***)malloc(r*sizeof(float**));
  mean_var=(float***)malloc(r*sizeof(float**));
  for (k=0;k<r;k++)
    {
      mean_val[k]=(float**)malloc(r*sizeof(float*));
      var_val[k]=(float**)malloc(r*sizeof(float*));
      mean_var[k]=(float**)malloc(r*sizeof(float*));
      for (j=0;j<r;j++)
        {
          mean_val[k][j]=(float*)malloc(r*sizeof(float));
          var_val[k][j]=(float*)malloc(r*sizeof(float));
          mean_var[k][j]=(float*)malloc(r*sizeof(float));  
        }
    }
  // now back to the half again
  r/=2;
  
  // set the brain cube using the half the brain radius
  // note (x,y,z) is the COG
  xmin=MAX(2,x-r-1);
  xmax=MIN(MRI_var->width-2,xmin+2*r);
  xmin=xmax-2*r;

  ymin=MAX(2,y-2*r-1);// subtraction (2*r) is bigger than 
                      // (r) -> trying to remove neck portion   
  ymax=MIN(MRI_var->height-2,ymin+2*r);
  ymin=ymax-2*r;

  zmin=MAX(2,z-r-1);
  zmax=MIN(MRI_var->depth-2,zmin+2*r);
  zmin=zmax-2*r;
    
  /*Calculate the mean intensity and the variance (mean = 27 voxels) */
  // within min, max range 
  for(k=0;k<256;k++)
    {
      number[k]=0;
      intensity_percent[k]=0;
    }

  for(k=zmin;k<zmax;k++)
    for(j=ymin;j<ymax;j++)
      {
        for(u=0;u<3;u++)
          for(v=0;v<3;v++)
            {
              pbc[u][v]=&MRIvox(MRI_var->mri_src,0,j+u,k+v);
              pbc[u][v]+=xmin;
            }
        for(i=xmin;i<xmax;i++)
          {
            mean=0;
            for(u=0;u<3;u++)
              for(v=0;v<3;v++)
                for(n=0;n<3;n++)
                  mean+=(*(pbc[u][v]+n));
            mean/=27;
            mean_val[k-zmin][j-ymin][i-xmin]=mean;

            if (mean>2*MRI_var->CSF_intensity && mean<MRI_var->Imax-1)
              {
                var=0;
                for(u=0;u<3;u++)
                  for(v=0;v<3;v++)
                    for(n=0;n<3;n++)
                      var+=SQR((*(pbc[u][v]+n))-mean);      
          
                var/=27;
                var_val[k-zmin][j-ymin][i-xmin]=var;

              }
            else
              var_val[k-zmin][j-ymin][i-xmin]=1000;

        
            for(u=0;u<3;u++)
              for(v=0;v<3;v++)
                pbc[u][v]++;  
          }
      }

  /*- Find the mean variance (27 voxels) 
    - And find the mean variance for each intensity 
    divided by the number of voxels of the same intensity 
    -> estimation of the MRI_var->WM_intensity */ 
  // mean_val, var_val are all within the brain rad cube
  r*=2; min=1000;max=0;  
  for(k=0;k<r;k++)
    for(j=0;j<r;j++)
      for(i=0;i<r;i++)
        {
          if (!(i*j*k*(i-r+1)*(j-r+1)*(k-r+1)))
            {
              mean=1000;
            }
          else
            {
              mean=0;
              for(u=-1;u<2;u++)
                for(v=-1;v<2;v++)
                  for(n=-1;n<2;n++)
                    mean+=var_val[k+u][j+v][i+n];
          
              mean/=27;
              if (min>=mean)
                {
                  min=mean;
                  if (mean==0)
                    {
                      wmint+=(int)(mean_val[k][j][i]+0.5);
                      wmnb++;
                    }
                }
              if (max<mean)
                max=mean;
              number[(int)(mean_val[k][j][i]+0.5)]++;
              // accumulate
              intensity_percent[(int)(mean_val[k][j][i]+0.5)]+=
                var_val[k][j][i];
            }
          mean_var[k][j][i]=mean;
        }
  if(wmnb)
    wmint/=wmnb;

  /*  fprintf(stderr," min=%f max=%f wmint=%ld",min,max,wmint);*/

  tmp=0;
  for(k=0;k<256;k++)
    if (number[k]<100)
      intensity_percent[k]=0;
    else
      {
        intensity_percent[k]=SQR(number[k])/intensity_percent[k];
        if (intensity_percent[k]>tmp)
          tmp=intensity_percent[k];
      }

  analyseWM(intensity_percent,MRI_var);

  if (MRI_var->WM_intensity < MRI_var->CSF_intensity)
    {
      fprintf(stderr, "\n\n\n\n*******************************************\n");
      fprintf(stderr, "***********************************************\n");
      fprintf(stderr, "***********************************************\n");
      fprintf(stderr, "White matter intensity %d is lower than CSF "
              "intensity %d.\n",
              MRI_var->WM_intensity, MRI_var->CSF_intensity);
      fprintf(stderr, "Please examine input images.  Will terminate ...\n");
      fprintf(stderr, "***********************************************\n");
      fprintf(stderr, "***********************************************\n");
      fprintf(stderr, "***********************************************\n");
      return -1;
    }

  DebugCurve(intensity_percent, 256, "White Matter\n");

  m=0;
  MRI_var->WM_VARIANCE=0;
  for(n=MRI_var->WM_HALF_MIN;n<=MRI_var->WM_HALF_MAX;n++)
    {
      m++;
      MRI_var->WM_VARIANCE+= int(number[n]/intensity_percent[n]);
    }

  if(m==0)
    Error("\n Problem with the variance calculation ");
  
  MRI_var->WM_VARIANCE= int(sqrt(MRI_var->WM_VARIANCE/m));

  MRI_var->WM_MIN=(MRI_var->WM_MIN+MRI_var->WM_VARIANCE)/2;

  if (MRI_var->WM_MIN<=MRI_var->WM_HALF_MIN-3*MRI_var->WM_VARIANCE/2)
    MRI_var->WM_MIN=MRI_var->WM_HALF_MIN-3*MRI_var->WM_VARIANCE/2;

  
  /////////////////////////////////////////////////////////////////////
  // if T1 image, the values are fixed
  if(parms->T1)
    {
      MRI_var->WM_intensity=WM_CONST;
      MRI_var->WM_HALF_MAX=MRI_var->WM_HALF_MIN=WM_CONST;
      MRI_var->WM_VARIANCE=5;
      MRI_var->WM_MAX=WM_CONST;
      MRI_var->WM_MIN=WM_CONST;
    }
  if((fabs(MRI_var->WM_intensity-WM_CONST)<=2) 
     && (MRI_var->WM_VARIANCE<3))
    {
      if(fabs(MRI_var->WM_HALF_MAX-WM_CONST)<=2)
        {
          MRI_var->WM_MIN=MIN(MRI_var->WM_MIN,WM_CONST);
          MRI_var->WM_HALF_MIN=MIN(MRI_var->WM_HALF_MIN,WM_CONST);
          MRI_var->WM_intensity=WM_CONST;        
          MRI_var->WM_HALF_MAX=WM_CONST;
          MRI_var->WM_MAX=MAX(MRI_var->WM_MIN,WM_CONST);
        }
    }

  ///////////////////////////////////////////////////////////////////
  retVal = Decision(parms,MRI_var);
  // r is the half the brain radius
  if (retVal > 0)
    {
      /*find the WM coord */
      tmp=max;
      for(k=1;k<r-1;k++)
        for(j=1;j<r-1;j++)
          for(i=1;i<r-1;i++)
            if(mean_val[k][j][i]==MRI_var->WM_HALF_MAX)
              if (mean_var[k][j][i]<tmp)
                {
                  tmp=mean_var[k][j][i];
                  x=i;
                  y=j;
                  z=k;
                }  
      // mean_val array exists only the square volume of radius size
      // thus actual voxel index is to add (xmin,ymin,zmin)
      i=xmin+1+x;
      j=ymin+1+y;
      k=zmin+1+z;

      /*Create the global minimum*/
      if (MRI_var->i_global_min)
        {
          i=MRI_var->i_global_min;
          j=MRI_var->j_global_min;
          k=MRI_var->k_global_min;
        }
      // change the histogram of this particular i,j,k grey value
      // note that tabdim is the histogram of grey values
      MRI_var->tabdim[MRIvox(MRI_var->mri_src,i,j,k)]--;
      MRI_var->int_global_min=MRIvox(MRI_var->mri_src,i,j,k);
      // change grey value to be Imax
      MRIvox(MRI_var->mri_src,i,j,k)=MRI_var->Imax;
      MRI_var->i_global_min=i;
      MRI_var->j_global_min=j;
      MRI_var->k_global_min=k;
      // so the histogram of Imax column should be increased.
      MRI_var->tabdim[MRI_var->Imax]++;

      // this particular one base labeled as 3
      MRI_var->Basin[k][j][i].type=3;
      MRI_var->Basin[k][j][i].next=
        (BasinCell*)CreateBasinCell(MRI_var->Imax,1,0);
 
      ig=MRI_var->i_global_min;
      jg=MRI_var->j_global_min;
      kg=MRI_var->k_global_min;

      // get the number of seed points
      n=parms->nb_seed_points;
      while(n)
        {
          i=parms->seed_coord[n-1][0];
          j=parms->seed_coord[n-1][1];
          k=parms->seed_coord[n-1][2];
          parms->seed_coord[n-1][3]=MRIvox(MRI_var->mri_src,i,j,k);
          // decrease the tabdim histogram column relevant to the grey value
          MRI_var->tabdim[parms->seed_coord[n-1][3]]--;
          // set that voxel to be the Imax
          MRIvox(MRI_var->mri_src,i,j,k)=MRI_var->Imax;
          // increase the tabdim histogram column at Imax to reflect setting 
          MRI_var->tabdim[MRI_var->Imax]++;
          // 
          if(MRI_var->Basin[k][j][i].type!=3)
            {
              // change the basin type to 1 for the seed point
              MRI_var->Basin[k][j][i].type=1;
              // next points to the global min Basin
              MRI_var->Basin[k][j][i].next=
                (Cell*)(&MRI_var->Basin[kg][jg][ig]);
              // increase the global min Basin size
              ((BasinCell*)(MRI_var->Basin[kg][jg][ig].next))->size++;
            }
          n--;
        }
      /////////////////////////////////////////////////////
      if(parms->T1)
        {
          /*allocate temp MRI struct and init to zero*/
          mri_tmp=MRIclone(MRI_var->mri_src,NULL);
          for(k=0;k<MRI_var->depth;k++)
            for(j=0;j<MRI_var->height;j++)
              {
                pb=&MRIvox(mri_tmp,0,j,k);
                for(i=0;i<MRI_var->width;i++)
                  {
                    (*pb)=0;
                    pb++;
                  }
              }
          /*set Voxel=WM to 1*/
          for(n=0;n<MRI_var->T1nbr;n++)
            {
              i=MRI_var->T1Table[n][0];
              j=MRI_var->T1Table[n][1];
              k=MRI_var->T1Table[n][2];
              MRIvox(mri_tmp,i,j,k)=1;
            }
          free(MRI_var->T1Table);
          MRI_var->T1Table=NULL;
          MRI_var->T1nbr=0;

          /*go through the whole temp struct and keep the WM inside voxels*/
          for(k=3;k<MRI_var->depth-3;k++)
            for(j=3;j<MRI_var->height-3;j++)
              for(i=3;i<MRI_var->width-3;i++)
                {
                  r=1;
                  for(u=-1;u<2;u++)
                    for(v=-1;v<2;v++)
                      for(m=-1;m<2;m++)
                        {
                          r*=MRIvox(mri_tmp,i+u,j+v,k+m);
                          if(r==0)
                            break;
                        }
                  if(r)
                    {
                      MRI_var->tabdim[MRIvox(MRI_var->mri_src,i,j,k)]--;
                      MRIvox(MRI_var->mri_src,i,j,k)=MRI_var->Imax;
                      MRI_var->tabdim[MRI_var->Imax]++;
                      if(MRI_var->Basin[k][j][i].type!=3)
                        {
                          MRI_var->Basin[k][j][i].type=1;
                          MRI_var->Basin[k][j][i].next=
                            (Cell*)(&MRI_var->Basin[kg][jg][ig]);
                  ((BasinCell*)(MRI_var->Basin[kg][jg][ig].next))->size++;
                        }
                    }
                }
          MRIfree(&mri_tmp);
        }
     
    
#if 0   /*find automatically many basin: not very relevant*/
      n=0;
      for(k=1;k<r-1;k++)
        for(j=1;j<r-1;j++)
          for(i=1;i<r-1;i++)
            if(mean_val[k][j][i]==MRI_var->WM_HALF_MAX)
              if (mean_var[k][j][i]<=tmp*2)
                {
                  n++;
                  MRI_var->tabdim[MRIvox(MRI_var->mri_src,
                                         xmin+1+i,
                                         ymin+1+j,
                                         zmin+1+k)]--;
                  MRIvox(MRI_var->mri_src,xmin+1+i,ymin+1+j,zmin+1+k)=
                    MRI_var->Imax;
                  MRI_var->tabdim[MRI_var->Imax]++;
                  MRI_var->Basin[zmin+1+k][ymin+1+j][xmin+1+i].type=3;
                  MRI_var->Basin[zmin+1+k][ymin+1+j][xmin+1+i].next=
                    CreateBasinCell(MRI_var->Imax,1,0);
                }

      fprintf(stderr,"\n %d basins created", n+parms->nb_seed_points+1);
#endif

      // T1 volume is defined to be only 110 grey values
      if (!min && wmint==110 && MRI_var->WM_MAX==110)
        {
          MRI_var->WM_intensity=110;
          MRI_var->WM_VARIANCE=5;
          MRI_var->WM_MAX=110;
        }

    
      fprintf(stderr,"\n      global maximum in x=%d, y=%d, z=%d, Imax=%d",
              MRI_var->i_global_min,
              MRI_var->j_global_min,
              MRI_var->k_global_min,
              MRI_var->Imax);
    
    
      fprintf(stderr,"\n      CSF=%d, WM_intensity=%d, WM_VARIANCE=%d",
              MRI_var->CSF_intensity,
              MRI_var->WM_intensity,
              MRI_var->WM_VARIANCE); 
      fprintf(stderr,"\n      WM_MIN=%d, WM_HALF_MIN=%d, "
              "WM_HALF_MAX=%d, WM_MAX=%d ",
              MRI_var->WM_MIN,
              MRI_var->WM_HALF_MIN,
              MRI_var->WM_HALF_MAX,
              MRI_var->WM_MAX);

      if(MRI_var->WM_VARIANCE>20)
        {
          fprintf(stderr,"\n Possible problem with the variance too big");
          MRI_var->WM_VARIANCE=15;
        }
    
      if(MRI_var->WM_MIN<=2*MRI_var->CSF_intensity)
        {
          fprintf(stderr,"\n Possible problem with WM_MIN too small");
          MRI_var->WM_MIN= 
            (int) MAX(MRI_var->WM_intensity/2,
                      MRI_var->WM_intensity-1.5*MRI_var->WM_VARIANCE);
          MRI_var->WM_HALF_MIN=
            MAX(MRI_var->WM_HALF_MIN,
                MRI_var->WM_intensity-MRI_var->WM_VARIANCE);
        } 
    }
  // end of if(Decision())

  /*free memory*/
  for (k=0;k<r;k++)
    {
      for (j=0;j<r;j++)
        {
          free(mean_val[k][j]);
          free(var_val[k][j]);
          free(mean_var[k][j]);
        }
      free(mean_val[k]);
      free(var_val[k]);
      free(mean_var[k]);
    }
  free(mean_val);
  free(var_val);
  free(mean_var);  
    
  if (retVal == -1)
    return -1;
  else
    return 0;
}

/*-----------------------------------------------------  
  Parameters: 

  Returns value:void

  Description: Analyze the white matter curve
  ------------------------------------------------------*/
static void analyseWM(double *tab,MRI_variables *MRI_var)
{
  int k,n;
  double mean;
  double a,b,Sxy,Sxx,Sx,Sy;

  /*for(k=0;k<256;k++)
    fprintf(MRI_var->fout, " %f",tab[k]);
    fprintf(MRI_var->fout, "\n");*/

  if(MRI_var->decision)
    for(k=(int)MIN(MRI_var->WM_MAX*MRI_var->scale,256);k<256;k++)
      tab[k]=0;

  DebugCurve(tab, 256, "\nWhiteMatter curve\n");

  // assume that WM_intensity is the most populous intensity
  MRI_var->WM_intensity=0; // int
  // find the max population index
  MRI_var->WM_intensity=findMaxIndex(tab);
  if(MRI_var->WM_intensity>240)
    Error("\nw=White Matter =Intensity too high (>240)...valid input ?");

  // find the half min index
  MRI_var->WM_HALF_MIN = findHalfMin(tab, MRI_var->WM_intensity);
  if(MRI_var->WM_HALF_MIN==1)
    Error("\n WM_HALF_MIN was too low.(1)");

  // get the mean population value less than half-min
  mean=0;
  for(k=0;k<MRI_var->WM_HALF_MIN;k++)
    mean+=tab[k];
  mean/=MRI_var->WM_HALF_MIN;

  // subtract the mean value from the population table to account only the WM
  for(k=0;k<256;k++)
    tab[k]=MAX(0,tab[k]-mean);

  MRI_var->WM_HALF_MAX = findHalfMax(tab, MRI_var->WM_intensity);
  if(MRI_var->WM_HALF_MAX==254)
    Error("\n WM_HALF_MAX was too high (254)");

  // WM_MAX to be the 1/3 of the peak population
  k=MRI_var->WM_HALF_MAX;
  for(n=k;n<255;n++)
    if(tab[n]>=tab[k]/3) 
      MRI_var->WM_MAX=n;
    else
      break;
  
  if(MRI_var->WM_MAX==254)
    Error("\n WM_MAX was too high (254)");

  // WM_HALF_MIN, WM_intensity, WM_HALF_MAX, WM_MAX are set

  ///////////////////////////////////////////////////////////
  // WM_MIN and WM_MAX are estimated by
  // triangle (linear) around WM_intensity
  //////////////////////////////////////////////////////////
  // WM_MAX analysis
  ///////////////////////////////////////////
  /*least-square distance interpolation*/
  if(MRI_var->WM_intensity < MRI_var->WM_MAX)
    {
      MRI_var->WM_MAX+=1;

      n=MRI_var->WM_MAX - MRI_var->WM_intensity + 1;
      Sxy = Sx = Sy = Sxx = 0;
      for(k=MRI_var->WM_intensity;k<=MRI_var->WM_MAX;k++)
        {
          Sxy+=(float)k*tab[k];
          Sx+=k;
          Sy+=tab[k];
          Sxx+=k*k;
        }
      a=(n*Sxy-Sy*Sx)/(n*Sxx-Sx*Sx);
      b=-(a*Sx-Sy)/n;

      if(DZERO(a) || !finite(a))
        Error("\n Interpolation problem in the white matter curve analysis\n");

      MRI_var->WM_MAX=int(-b/a);    
    }
  // WM_MIN analysis
  ///////////////////////////////////////////
  k=MRI_var->WM_intensity;
  for(n=k;n>1;n--)
    if(tab[n]>=tab[k]/2)
      MRI_var->WM_HALF_MIN=n;
    else
      break;

  if(MRI_var->WM_HALF_MIN==2)
    Error("\n Problem with WM_HALF_MIN too small");

  k=MRI_var->WM_HALF_MIN;
  for(n=k;n>=1;n--)
    if(tab[n]>=tab[k]/2)
      MRI_var->WM_MIN=n-1;
    else
      break;

  if(MRI_var->WM_MIN==2)
    Error("\n Problem with WM_MIN too small");

  /*least-square distance interpolation*/
  if(MRI_var->WM_intensity > MRI_var->WM_MIN)
    {
      MRI_var->WM_MIN=3*MRI_var->WM_MIN/2-MRI_var->WM_intensity/2;

      n=MRI_var->WM_intensity-MRI_var->WM_MIN+1;
      Sxy = Sx = Sy = Sxx = 0;
      for(k=MRI_var->WM_MIN;k<=MRI_var->WM_intensity;k++)
        {
          Sxy+=(float)k*tab[k];
          Sx+=k;
          Sy+=tab[k];
          Sxx+=k*k;
        }

      a=(n*Sxy-Sy*Sx)/(n*Sxx-Sx*Sx);
      b=-(a*Sx-Sy)/n;

      if(DZERO(a) || !finite(a))
        Error("\n Interpolation problem in the white matter analysis");

      MRI_var->WM_MIN=int(MAX(0.,-b/a));
    }
}


/*-----------------------------------------------------  
  Parameters: 

  Returns value:BasinCell*

  Description: Allocate and Init a BasinCell
  ------------------------------------------------------*/

static BasinCell* CreateBasinCell(int val, 
                                  unsigned long size, 
                                  unsigned long ambiguous)
{
  BasinCell *bcell;

  bcell=(BasinCell*)malloc(sizeof(BasinCell));
  bcell->depth=(unsigned char)val;
  bcell->size=size;
  bcell->ambiguous=ambiguous;

  return bcell;
}
/*-----------------------------------------------------  
  Parameters: 

  Returns value:int value (success or error)

  Description: Decide if the white matter parameters are valid
  If too low, scale the image intensity 
  such that wm=160
  ------------------------------------------------------*/
static int Decision(STRIP_PARMS *parms,  MRI_variables *MRI_var)
{
  int i,j,k;
  double scale;
  if (MRI_var->WM_intensity+MRI_var->WM_VARIANCE>=80)
    return 1;
  else
    {
      scale= 160.0/((double) MRI_var->WM_MAX);
      // if scaled once, the second time gives the same scale and thus bail out
      if (fabs(scale -1.) < .001) // allow epsilon error of .1%
        return -1;

      MRI_var->decision=1;
      MRI_var->scale= (float) scale;

      for(k=0;k<MRI_var->depth;k++)
        for(j=0;j<MRI_var->height;j++)
          for(i=0;i<MRI_var->width;i++)
            MRIvox(MRI_var->mri_src,i,j,k)=
              (unsigned short)MIN(255,scale*MRIvox(MRI_var->mri_src,i,j,k));
      fprintf(stderr,"\nmean intensity too low !");
      fprintf(stderr,"\nModification of the image: intensity*%2.2f",scale);
      for(k=0;k<256;k++)
        {
          MRI_var->tabdim[k]=0;
          MRI_var->sqrdim[k]=0;
          MRI_var->count[k]=0;
          MRI_var->intbasin[k]=k;
          MRI_var->gmnumber[k]=0;
        }
      return Pre_CharSorting(parms,MRI_var);
    }
}


/*-----------------------------------------------------  
  Parameters: 

  Returns value:

  Description: Sorting of the voxel in an ascending order
  ------------------------------------------------------*/
static int CharSorting(MRI_variables *MRI_var)
{
  int i,j,k,u,v;
  int l;
  ldiv_t ld;
  BUFTYPE *pb;
  unsigned char val;

  /*allocating an 256 table of Coord** in order to process the Sorting*/
  
  for(k=1;k<MRI_var->Imax+1;k++)
    {    
      l=sqroot(MRI_var->tabdim[k]); // sqrt of the population at grey=k
      MRI_var->sqrdim[k]=l;
      MRI_var->Table[k]=(Coord**)malloc(l*sizeof(Coord*));
      // each Table is a 2d array of Coord
      if(!MRI_var->Table[k]) Error("Allocation first Table Echec");
      // array is l x l 
      for(u=0;u<l;u++)
        {
          MRI_var->Table[k][u]=(Coord*)calloc(l,sizeof(Coord)); 
          if(!MRI_var->Table[k][u]) Error("Allocation second Table Echec");
        }
    }

  /*Sorting itself*/
  // each Table[k] (k=grey value) is a 2D array.  each element of 2D array has
  // the coordinate of a voxel.
  // filling 2d array with quot and rem of count
  // why 2d array?  Maybe this programmer thought that the length of column is
  // too big to fit in the linear array.  Unnecessary
  for(k=2;k<MRI_var->depth-2;k++)
    for(j=2;j<MRI_var->height-2;j++)
      {
        pb=&MRIvox(MRI_var->mri_src,0,j,k);
        pb+=2;
        for(i=2;i<MRI_var->width-2;i++)
          {
            val=*pb++;
            if(val)
              {
                l=MRI_var->count[val]++;   
                // count[] is a histogram of non-zero grey values
                ld=ldiv(l,MRI_var->sqrdim[val]);
                u=ld.quot;
                v=ld.rem;
                MRI_var->Table[val][u][v][0]=i;
                MRI_var->Table[val][u][v][1]=j;
                MRI_var->Table[val][u][v][2]=k;
              }
          }
      }
  return 0;
}

/*sqrt routine*/
static int sqroot(int r)
{
  int i;
  i=(int)sqrt(r);
  if(i*i<r) return (i+1);
  else return i;
}

/*******************************ANALYZE****************************/

/*routine that analyzes all the voxels sorted in an descending order*/
static int Analyze(STRIP_PARMS *parms,MRI_variables *MRI_var)
{
  int pos;
  int u,v,n,d;
  int l;
  ldiv_t ld;
  double vol_elt;

  MRI_var->basinnumber=0;
  MRI_var->basinsize=0;


  n=MRI_var->sqrdim[MRI_var->Imax]; // the sqrt of the population at Imax
  for(u=0;u<n;u++)
    free(MRI_var->Table[MRI_var->Imax][u]);
  free(MRI_var->Table[MRI_var->Imax]);

  for(pos=MRI_var->Imax-1;pos>0;pos--)
    {
      l=0;
      d=MRI_var->tabdim[pos];  // the population at pos
      n=MRI_var->sqrdim[pos];  // the sqrt of the d
      while(l<d)
        {
          ld=ldiv(l,n);
          u=ld.quot;
          v=ld.rem;
          Test(MRI_var->Table[pos][u][v],parms,MRI_var);
          l++;
        }
      for(u=0;u<n;u++)
        free(MRI_var->Table[pos][u]);
      free(MRI_var->Table[pos]);

      fprintf(stderr,"\r      %3d%%... %8ld basins; main size = %8ld         ",
              (MRI_var->Imax-pos)*100/(MRI_var->Imax-1),
              MRI_var->basinnumber,MRI_var->basinsize);
    }


  MRI_var->main_basin_size+=((BasinCell*)MRI_var->Basin
                             [MRI_var->k_global_min]
                             [MRI_var->j_global_min]
                             [MRI_var->i_global_min].next)->size;
  for(n=0;n<parms->nb_seed_points;n++)
    MRI_var->main_basin_size+=((BasinCell*)MRI_var->Basin
                               [parms->seed_coord[n][2]]
                               [parms->seed_coord[n][1]]
                               [parms->seed_coord[n][0]].next)->size;
              
  vol_elt=
    MRI_var->mri_src->xsize*
    MRI_var->mri_src->ysize*
    MRI_var->mri_src->zsize;      
  fprintf(stderr,"\n      main basin size=%8ld voxels, voxel volume =%.3f ",
          MRI_var->main_basin_size,(float)vol_elt);
  fprintf(stderr,"\n                     = %.0f mmm3 = %.3f cm3"
          ,MRI_var->main_basin_size*vol_elt,
          (float)MRI_var->main_basin_size/1000.*vol_elt);
  MRIvox(MRI_var->mri_src,
         MRI_var->i_global_min,
         MRI_var->j_global_min,
         MRI_var->k_global_min)
    =MRI_var->int_global_min;
  for(n=0;n<parms->nb_seed_points;n++)
    MRIvox(MRI_var->mri_src,
           parms->seed_coord[n][0],
           parms->seed_coord[n][1],
           parms->seed_coord[n][2])
      =parms->seed_coord[n][3];


  return 0;
}

/*looking at a voxel, finds the corresponding basin*/
static Cell* FindBasin(Cell *cell)
{
  cell= (Cell *) cell->next;
  while(cell->type==1)
    cell=(Cell *) cell->next;
  return cell;
}

/*main routine for the merging*/
static int Lookat(int i,int j,int k,
                  unsigned char val,
                  int *dpt,Cell* *admax,
                  int *nb,Cell * adtab[27],
                  STRIP_PARMS *parms, 
                  MRI_variables *MRI_var)
{
  int t,n;
  unsigned char d,hp=parms->hpf;
  Cell *add;

  add=&MRI_var->Basin[k][j][i];
  
  /*looks if the basin has already been processing*/

  if(add->type)
    {
      if(add->type==1)
        {
          add=FindBasin(add);     

          d=((BasinCell*)(add->next))->depth;

          if(d>*dpt)
            {
              *admax=add;
              *dpt=d;
            }
          if (100*(d-val)<hp*MRI_var->Imax)
            {
              t=0;
              for(n=0;n<*nb;n++)
                if(add==adtab[n])
                  t=1;
              if(!t)
                {
                  adtab[*nb]=add;
                  (*nb)++;
                }
            }
        }
      else
        {
          d=((BasinCell*)(add->next))->depth;
     

          if(d>*dpt)
            {
              *admax=add;
              *dpt=d;
            }
          if (100*(d-val)<hp*MRI_var->Imax)
            {
              t=0;
              for(n=0;n<*nb;n++)
                if(add==adtab[n])
                  t=1;
              if(!t)
                {
                  adtab[*nb]=add;
                  (*nb)++;
                }
            }
        }
      return 0;
    }
  else if (100*(val-MRIvox(MRI_var->mri_src,i,j,k))<hp*MRI_var->Imax)
    return 1;       
  
  return 0;
}


/*tests a voxel, merges it or creates a new basin*/
static int Test(Coord crd,STRIP_PARMS *parms,MRI_variables *MRI_var)
{
  int n,nb=0,dpt=-1,tst[6];
  unsigned char val;
  int mean,var,tp=0;
  int a,b,c;

  int i=crd[0],j=crd[1],k=crd[2];

  Cell  *adtab[27],*admax=&MRI_var->Basin[k][j][i];

  val=MRIvox(MRI_var->mri_src,i,j,k);

  tst[0]=Lookat(i,j,k-1,val,&dpt,&admax,&nb,adtab,parms,MRI_var);
  tst[1]=Lookat(i,j,k+1,val,&dpt,&admax,&nb,adtab,parms,MRI_var);
  tst[2]=Lookat(i,j-1,k,val,&dpt,&admax,&nb,adtab,parms,MRI_var);
  tst[3]=Lookat(i,j+1,k,val,&dpt,&admax,&nb,adtab,parms,MRI_var);
  tst[4]=Lookat(i-1,j,k,val,&dpt,&admax,&nb,adtab,parms,MRI_var);
  tst[5]=Lookat(i+1,j,k,val,&dpt,&admax,&nb,adtab,parms,MRI_var);


  /* does not seem to be relevant 
     enables to look 2 voxels away if the next voxel neighbour is 
     lower but "not too much"...

     only cares if one of the 6 neighbours belong to the main basin,
     ie type==3
  

     if (admax->type==3)    
     {
     if(tst[0])
     Lookat(i,j,k-2,val,&dpt,&admax,&nb,adtab);
     if(tst[1])
     Lookat(i,j,k+2,val,&dpt,&admax,&nb,adtab);
     if(tst[2])
     Lookat(i,j-2,k,val,&dpt,&admax,&nb,adtab);
     if(tst[3])
     Lookat(i,j+2,k,val,&dpt,&admax,&nb,adtab);
     if(tst[4])
     Lookat(i-2,j,k,val,&dpt,&admax,&nb,adtab);
     if(tst[5])
     Lookat(i+2,j,k,val,&dpt,&admax,&nb,adtab);
     }
  
  */

  /*does not seem to be relevant : 26 neighbours instead of 6...

  only cares if one of the 6 neighbours belong to the main basin,
  ie type==3
  

  if (admax->type==3)
  {
  
  Lookat(i-1,j-1,k,val,&dpt,&admax,&nb,adtab);
  Lookat(i-1,j-1,k-1,val,&dpt,&admax,&nb,adtab);
  Lookat(i-1,j-1,k+1,val,&dpt,&admax,&nb,adtab);
  Lookat(i-1,j+1,k,val,&dpt,&admax,&nb,adtab);
  Lookat(i-1,j+1,k-1,val,&dpt,&admax,&nb,adtab);
  Lookat(i-1,j+1,k+1,val,&dpt,&admax,&nb,adtab);
  Lookat(i-1,j,k-1,val,&dpt,&admax,&nb,adtab);
  Lookat(i-1,j,k+1,val,&dpt,&admax,&nb,adtab);

  Lookat(i,j-1,k-1,val,&dpt,&admax,&nb,adtab);
  Lookat(i,j-1,k+1,val,&dpt,&admax,&nb,adtab);
  Lookat(i,j+1,k-1,val,&dpt,&admax,&nb,adtab);
  Lookat(i,j+1,k+1,val,&dpt,&admax,&nb,adtab);

  Lookat(i+1,j-1,k,val,&dpt,&admax,&nb,adtab);
  Lookat(i+1,j-1,k-1,val,&dpt,&admax,&nb,adtab);
  Lookat(i+1,j-1,k+1,val,&dpt,&admax,&nb,adtab);
  Lookat(i+1,j+1,k,val,&dpt,&admax,&nb,adtab);
  Lookat(i+1,j+1,k-1,val,&dpt,&admax,&nb,adtab);
  Lookat(i+1,j+1,k+1,val,&dpt,&admax,&nb,adtab);
  Lookat(i+1,j,k-1,val,&dpt,&admax,&nb,adtab);
  Lookat(i+1,j,k+1,val,&dpt,&admax,&nb,adtab);
  }

  */

  /*creates a new basin*/

  if (parms->watershed_analyze)
    {
      mean=0;
      var=0;
      for( a = -1 ; a<2 ; a++)    
        for( b = -1 ; b<2 ; b++)
          for( c = -1 ; c<2 ; c++)
            {
              // i > 0, j > 0, k > 0  
              tp=MRIvox(MRI_var->mri_src,i+a,j+b,k+c);  
              mean+=tp;
              var+=SQR(tp);
            }
      mean/=27;
      var=var/27-SQR(mean);
      if (mean>=MRI_var->WM_MIN && 
          mean<=MRI_var->WM_MAX &&
          var<=MRI_var->WM_VARIANCE)
        tp=1;
      else tp=0;
    }

  if (dpt==-1)
    {
      MRI_var->Basin[k][j][i].type=2;
      if(!tp)
        MRI_var->Basin[k][j][i].next=(BasinCell*)CreateBasinCell(val,1,0);
      else
        MRI_var->Basin[k][j][i].next=(BasinCell*)CreateBasinCell(val,1,1);
      MRI_var->basinnumber++;
      return 0;
    };

  /*Merging*/
  if(admax->type==3 && val<=MRI_var->WM_MIN && val>0)
    MRI_var->gmnumber[val]++;

  MRI_var->Basin[k][j][i].type=1;
  MRI_var->Basin[k][j][i].next=(Cell*)admax;
  ((BasinCell*)(admax->next))->size++;
  if (tp && admax->type!=3)
    ((BasinCell*)(admax->next))->ambiguous++;

  if (!tp || admax->type==3)
    {
      for(n=0;n<nb;n++)
        if(adtab[n]!=admax)
          {
            adtab[n]->type=1;
            ((BasinCell*)(admax->next))->size+=
              ((BasinCell*)(adtab[n]->next))->size;
            free((BasinCell*)(adtab[n]->next));
            adtab[n]->next=(Cell*)admax;
            MRI_var->basinnumber--;
          }
    }else
      {
        for(n=0;n<nb;n++)
          if(adtab[n]!=admax)
            {
              adtab[n]->type=1;
              ((BasinCell*)(admax->next))->size+=
                ((BasinCell*)(adtab[n]->next))->size;
              ((BasinCell*)(admax->next))->ambiguous+=
                ((BasinCell*)(adtab[n]->next))->ambiguous;
              free((BasinCell*)(adtab[n]->next));
              adtab[n]->next=(Cell*)admax;
              MRI_var->basinnumber--;
            }
      }
  

  if (((BasinCell*)(admax->next))->size>MRI_var->basinsize)
    {
      MRI_var->basinsize=((BasinCell*)(admax->next))->size;
    }

  return 0;
}

/**************POST_ANALYZE***********************/

/*looks if the voxel basin is the main one, ie type==3*/
static Cell* TypeVoxel(Cell *cell)
{
  Cell* cell1=cell;

  while(cell1->type==1)
    cell1=(Cell *) cell1->next;

  return cell1;

}


/*Looks if the voxel is a border from the segmented brain*/ 
static int AroundCell(unsigned char i,unsigned char j,unsigned char k,
                      MRI_variables *MRI_var)
{
  int val=0,n=0;
  
  if(MRI_var->Basin[k][j][i-1].type)
    {
      val+=MRIvox(MRI_var->mri_src,i-1,j,k);
      n++;
    }
  if(MRI_var->Basin[k][j][i+1].type)
    {
      val+=MRIvox(MRI_var->mri_src,i+1,j,k);
      n++;
    }
  if(MRI_var->Basin[k][j-1][i].type)
    {
      val+=MRIvox(MRI_var->mri_src,i,j-1,k);
      n++;
    }
  if(MRI_var->Basin[k][j+1][i].type)
    {  
      val+=MRIvox(MRI_var->mri_src,i,j+1,k);
      n++;
    }
  if(MRI_var->Basin[k-1][j][i].type)
    {
      val+=MRIvox(MRI_var->mri_src,i,j,k-1);
      n++;
    }
  if(MRI_var->Basin[k+1][j][i].type)
    {
      val+=MRIvox(MRI_var->mri_src,i,j,k+1);
      n++;
    }
  if (6-n)
    return ((val+MRIvox(MRI_var->mri_src,i,j,k))/(n+1));
  else 
    return 0;
}


/*Merge voxels which intensity is near the intensity of border voxels*/
static int MergeRoutine(unsigned char i,unsigned char j,unsigned char k,
                        int val,int *n,MRI_variables *MRI_var)
{
  int cond=15*val;
  Bound *buff;
  Cell cell=MRI_var->Basin[k][j][i];

  if(cell.type<2)
    if(abs(MRIvox(MRI_var->mri_src,i,j,k)-val)*100<cond)
      {
        if(cell.type==1)
          {
            ((Bound*)cell.next)->val+=val;
            ((Bound*)cell.next)->val/=2;
          }
        else
          {
            cell.type=1;
            buff=(Bound*)malloc(sizeof(Bound));
            buff->x=i;
            buff->y=j;
            buff->z=k;
            buff->val=val;
            cell.next=(Bound*)buff;
            (*n)++;
            buff->next=MRI_var->Bound2;
            MRI_var->Bound2=buff;
          }
      }
  return 0;
}


static int Merge(unsigned char i,unsigned char j,unsigned char k,
                 int val,int *n,MRI_variables *MRI_var)
{
  
  MergeRoutine(i-1,j,k,val,n,MRI_var);
  MergeRoutine(i+1,j,k,val,n,MRI_var);
  MergeRoutine(i,j-1,k,val,n,MRI_var);
  MergeRoutine(i,j+1,k,val,n,MRI_var);
  MergeRoutine(i,j,k-1,val,n,MRI_var);
  MergeRoutine(i,j,k+1,val,n,MRI_var);

  MergeRoutine(i-1,j-1,k-1,val,n,MRI_var);
  MergeRoutine(i-1,j-1,k,val,n,MRI_var);
  MergeRoutine(i-1,j-1,k+1,val,n,MRI_var);
  MergeRoutine(i-1,j,k-1,val,n,MRI_var);
  MergeRoutine(i-1,j,k+1,val,n,MRI_var);
  MergeRoutine(i-1,j+1,k-1,val,n,MRI_var);
  MergeRoutine(i-1,j+1,k,val,n,MRI_var);
  MergeRoutine(i-1,j+1,k+1,val,n,MRI_var);

  
  MergeRoutine(i+1,j-1,k-1,val,n,MRI_var);
  MergeRoutine(i+1,j-1,k,val,n,MRI_var);
  MergeRoutine(i+1,j-1,k+1,val,n,MRI_var);
  MergeRoutine(i+1,j,k-1,val,n,MRI_var);
  MergeRoutine(i+1,j,k+1,val,n,MRI_var);
  MergeRoutine(i+1,j+1,k-1,val,n,MRI_var);
  MergeRoutine(i+1,j+1,k,val,n,MRI_var);
  MergeRoutine(i+1,j+1,k+1,val,n,MRI_var);

  MergeRoutine(i,j-1,k-1,val,n,MRI_var);
  MergeRoutine(i,j-1,k+1,val,n,MRI_var);
  MergeRoutine(i,j+1,k-1,val,n,MRI_var);
  MergeRoutine(i,j+1,k+1,val,n,MRI_var);

  return 0;
}

static int AddVoxel(MRI_variables *MRI_var)
{
  int n=0,p=0;
  Bound *bound=MRI_var->Bound1;
  MRI_var->Bound2=NULL;
  while(bound)
    {
      p++;
      Merge(bound->x,bound->y,bound->z,bound->val,&n,MRI_var);
      bound=bound->next;
    }
  while(MRI_var->Bound1)
    {
      bound=MRI_var->Bound1;
      MRI_var->Basin[bound->z][bound->y][bound->x].type=4;
      MRI_var->Bound1=MRI_var->Bound1->next;
      free(bound);
    }
  MRI_var->Bound1=MRI_var->Bound2;
  
  return n; 
}
#if 0
static int Mediane(int i,int j,int k,int rang)
{
  int u,v,w,p,q,r;
  static unsigned char tab[27];

  p=0;
  for (u=-1;u<2;u++)
    for (v=-1;v<2;v++)
      for (w=-1;w<2;w++)
        tab[p++]=MRIvox(MRI_var->mri_src,i+u,j+v,k+w);
    
  for(q=26;q>1;q--)
    for(p=0;p<q;p++)
      if(tab[p]>tab[p+1])
        {
          r=tab[p+1];
          tab[p+1]=tab[p];
          tab[p]=r;
        }
  MRIvox(MRI_var->mri_src,i,j,k)=tab[rang];  

  return 0;
}
#endif

#if 0
static int Ambiguous(Cell* cell)
{

  if(!parms->watershed_analyze)
    return 0;

  /*Add some code here if you want to take 
    into account the ambiguous number*/ 

  return 1;  
}
#endif

static int TRY(int i,int j, int k,MRI_variables *MRI_var)
{
  unsigned char val;
  int a,b,c,n;

  n=0;
  for (a=-1;a<=1;a++)
    for (b=-1;b<=1;b++)
      for (c=-1;c<=1;c++)
        if(MRI_var->Basin[k+c][j+b][i+a].type==4 || 
           (MRI_var->Basin[k+c][j+b][i+a].next==
            MRI_var->Basin[k][j][i].next && 
            MRI_var->Basin[k+c][j+b][i+a].type==7) 
           || (MRI_var->Basin[k+c][j+b][i+a].next==
               MRI_var->Basin[k][j][i].next && 
               MRI_var->Basin[k+c][j+b][i+a].type==8))
          {
            val=MRIvox(MRI_var->mri_src,i+a,j+b,k+c);
            if (val>=MRI_var->WM_HALF_MIN  && val<=MRI_var->WM_HALF_MAX)
              n++;
          }

  if(n>=9)
    return 1;
  else
    return 0;
}

static int TRYMERGE(int i,int j, int k,MRI_variables *MRI_var)
{
  int a,b,c,n;
  
  n=0;
  for (a=-1;a<=1;a++)
    for (b=-1;b<=1;b++)
      for (c=-1;c<=1;c++)
        if(MRI_var->Basin[k+c][j+b][i+a].next==MRI_var->Basin[k][j][i].next)
          if(MRI_var->Basin[k+c][j+b][i+a].type==7 || 
             MRI_var->Basin[k+c][j+b][i+a].type==8)
            n++;

  if(n>=6)
    return 1;
  else
    return 0;
}


static int PostAnalyze(STRIP_PARMS *parms,MRI_variables *MRI_var)
{
  int i,j,k,p,q;
  BUFTYPE *pb;
  Cell *cell,*cell1=NULL,*celltp=NULL;
  Bound* buff;
  BasinCell* bcell;
  int val;unsigned long n=0,added_size=0;

  /*if main_basin_size<estimated_size/5 
    find a complement limited to 2*estimated size...*/


  if (MRI_var->main_basin_size < MRI_var->estimated_size/5)
    {
      fprintf(stderr,"\n      Main basin size probably too small..."
              "looking for complement...");
      p= int(MRI_var->rad_Brain/5.);
      n=0;
      for(i=int(MRI_var->xCOG-p);i< int(MRI_var->xCOG+p);i++)
        for(j=int(MRI_var->yCOG-2*p);j< int(MRI_var->yCOG);j++)
          for(k=int(MRI_var->zCOG-p);k< int(MRI_var->zCOG+p);k++)
            {
              cell=(Cell*)MRI_var->Basin[k][j][i].next;
              if(cell)
                {
                  if(cell->type==1)
                    cell1=TypeVoxel(cell);
                  else
                    cell1=cell;
                  if (cell1->type==2 && 
                      ((BasinCell*)cell1->next)->size>n && 
                      ((BasinCell*)cell1->next)->size<
                      MRI_var->estimated_size*2)
                    {
                      n=((BasinCell*)cell1->next)->size;
                      celltp=cell1;
                    }
                }
            }
      if(n)             
        {
          celltp->type=3;   /*label this basin type 3*/
          fprintf(stderr,"OK\n");
          MRI_var->main_basin_size+=((BasinCell*)celltp->next)->size;
          fprintf(stderr,"      Corrected main basin size = %3ld\n",
                  MRI_var->main_basin_size); 
        }
      else
        fprintf(stderr,"Could not find a correcting basin\n");
    }


  /*Post-analyze: depends if the mode watershed_analyze is on
    if yes: analyze all the type 2 basins
    if no: all the type 2 basin are labelled type 0 and freed */


  for(k=2;k<MRI_var->depth-2;k++)
    for (j=2;j<MRI_var->height-2;j++)
      {
        pb=&MRIvox(MRI_var->mri_src,0,j,k);
        pb+=2;
        for (i=2;i<MRI_var->width-2;i++)
          {
            cell=&MRI_var->Basin[k][j][i];

            if(cell->type==1)
              cell1=TypeVoxel(cell);
            else
              cell1=cell;


            /*right now: only type 0="empty", 1=node to 2 or 3,
              2=auxiliar basin ,3=main basin to be freed */

            switch (cell1->type)
              {
                /*cases brain basin*/
              case 3:
                cell->type=4;
                cell1->type=4;
                free((BasinCell*)cell1->next);     
                cell->next=(unsigned char*) MRI_var->intbasin+*pb;
                break ;
              case 4:
                cell->type=4;
                cell->next=(unsigned char*) MRI_var->intbasin+*pb;
                break ;
                /*case non brain basin*/
              case 0:
                cell->type=0;
                cell->next=(unsigned char*) MRI_var->intbasin+*pb;
                break;
                /*case ambiguous basin*/ 
              case 2:
                if (parms->watershed_analyze)
                  {
                    if(1)      /*Ambiguous(cell1) instead of 1 
                                 if you want to use the ambiguous variable*/
                      {
                        if (cell!=cell1)
                          {
                            cell->type=5;
                            cell->next=(Cell*)cell1;
                          };
                        cell1->type=6;
                        ((BasinCell*)cell1->next)->ambiguous=0;
                      }else
                        {
                          cell->type=0;
                          cell1->type=0;
                          free((BasinCell*)cell1->next);
                          cell->next=(unsigned char*) MRI_var->intbasin+*pb;
                        }
                  }else /*case non watershed_analyze*/
                    {
                      cell->type=0;
                      cell1->type=0;
                      free((BasinCell*)cell1->next);
                      cell->next=(unsigned char*) MRI_var->intbasin+*pb;
                    }
                break;
              case 5:  /*necessary distinction between type 5 and 6 
                         to post free the type 6 basin */
                cell->type=5;
                cell->next=(Cell*)cell1->next;
                break;
              case 6:
                if (cell!=cell1)
                  {
                    cell->type=5;
                    cell->next=(Cell*)cell1;
                  }
                break;
              }
            pb++;
          }
      }


  /*right now: only type 0="empty", 4= main basin but "empty"
    , 5=node to 6, 6=auxiliar basin to be freed*/


  if(parms->watershed_analyze) /*type 4, type 5 & 6, and type 0 */
    {
      n=0;q=0;
      do{
        p=0;
        for(k=2;k<MRI_var->depth-2;k++)
          for (j=2;j<MRI_var->height-2;j++)
            for (i=2;i<MRI_var->width-2;i++)
              {
                cell=&MRI_var->Basin[k][j][i];
                if(cell->type==5)
                  {
                    cell1=(Cell*)cell->next;
                    bcell=(BasinCell*)cell1->next;
                    if(TRY(i,j,k,MRI_var))
                      cell->type=7;
                  }else if(cell->type==6)
                    {
                      bcell=(BasinCell*)cell->next;
                      if(TRY(i,j,k,MRI_var))
                        cell->type=8;
                    }
              }

        /*right now: type 0 "empty", 4 "empty", 5 node to 6 or 8, 
          6 to be freed , 7 node to 6 or 8 , 8 to be freed */

        for(k=2;k<MRI_var->depth-2;k++)
          for (j=2;j<MRI_var->height-2;j++)
            for (i=2;i<MRI_var->width-2;i++)
              {
                cell=&MRI_var->Basin[k][j][i];
                if(cell->type==7)
                  {
                    cell1=(Cell*)cell->next;
                    if (cell1->type==9)
                      cell->type=9;
                    else
                      {      
                        /* cell 7 is pointing on type 6 or 8*/
                        bcell=(BasinCell*)cell1->next;
                        if(TRYMERGE(i,j,k,MRI_var))
                          {
                            bcell->ambiguous++;
                            if(bcell->ambiguous>=parms->threshold_analyze
                               *pow(bcell->size,1.0/3.0)/100)
                              {
                                fprintf(stderr,"\n      ambiguous basin, "
                                        "merged: at least %ld ambiguous"
                                        " voxels; size: %ld voxels",  
                                        bcell->ambiguous, bcell->size);
                                p++;
                                n++;
                                cell->type=9;
                                cell1->type=9;
                                added_size+=bcell->size;
                                free(bcell);
                              }
                          }
                      }
                  }else if (cell->type==8)
                    {
                      bcell=(BasinCell*)cell->next;
                      if(TRYMERGE(i,j,k,MRI_var))
                        {
                          bcell->ambiguous++;
                          if(bcell->ambiguous>=
                             parms->threshold_analyze*
                             pow(bcell->size,1.0/3.0)/100)
                            {
                              fprintf(stderr,"\n      ambiguous basin, "
                                      "merged: at least %ld ambiguous "
                                      "voxels; size: %ld voxels", 
                                      bcell->ambiguous, bcell->size);
                              p++;
                              n++;
                              cell->type=9;
                              added_size+=bcell->size;
                              free(bcell);
                            }        
                        }
                    }
              }      
        q++;
      }while(p);

      fprintf(stderr,"\n      ***** %d basin(s) merged in %d iteration(s)"
              "\n      ***** %ld voxel(s) added to the main basin\n",
              (int)n,q,added_size);
    }    
    

  for(k=2;k<MRI_var->depth-2;k++)
    for (j=2;j<MRI_var->height-2;j++)
      for (i=2;i<MRI_var->width-2;i++)
        {
          switch(MRI_var->Basin[k][j][i].type)
            {
            case 4:
              MRI_var->Basin[k][j][i].type=3;
              break;
            case 5:
              cell1=(Cell*)MRI_var->Basin[k][j][i].next;
              if(cell1->type==3 || cell1->type==9)
                MRI_var->Basin[k][j][i].type=3;
              else
                MRI_var->Basin[k][j][i].type=0;
              break;
            case 6:
              MRI_var->Basin[k][j][i].type=0;
              if(((BasinCell*)MRI_var->Basin[k][j][i].next)->ambiguous)
                fprintf(stderr,"      ambiguous basin, non merged: "
                        "%ld ambiguous voxels; size: %ld voxels\n",
                        ((BasinCell*)MRI_var->Basin[k][j][i].next)->ambiguous, 
                        ((BasinCell*)MRI_var->Basin[k][j][i].next)->size);
              free((BasinCell*)MRI_var->Basin[k][j][i].next);
              break;
            case 7:
              cell1=(Cell*)MRI_var->Basin[k][j][i].next;
              if(cell1->type==3 || cell1->type==9)
                MRI_var->Basin[k][j][i].type=3;
              else
                MRI_var->Basin[k][j][i].type=0;
              break;
            case 8:
              MRI_var->Basin[k][j][i].type=0;
              fprintf(stderr,"      ambiguous basin, non merged: "
                      "%ld ambiguous voxels; size: %ld voxels\n",
                      ((BasinCell*)MRI_var->Basin[k][j][i].next)->ambiguous, 
                      ((BasinCell*)MRI_var->Basin[k][j][i].next)->size);
              free((BasinCell*)MRI_var->Basin[k][j][i].next);
            case 9:
              MRI_var->Basin[k][j][i].type=3;
              break;

            }
        }
  
  /* seems to be useless in this combined approach. 
     However, could be relevant in a single watershed process.*/

  if(!parms->template_deformation)
    {
      MRI_var->Bound1=NULL;
      n=0;
      for(k=2;k<MRI_var->depth-2;k++)
        for (j=2;j<MRI_var->height-2;j++)
          for (i=2;i<MRI_var->width-2;i++)
            {
              cell=&MRI_var->Basin[k][j][i];
              if (cell->type)   
                {
                  val=AroundCell(i,j,k,MRI_var);
                  if (val)
                    {
                      n++;
                      cell->type=4;
                      cell->next=(unsigned char*) MRI_var->intbasin+val;
                      if (val>MRI_var->CSF_intensity)
                        {
                          buff=(Bound*)malloc(sizeof(Bound));
                          buff->x=i;
                          buff->y=j;
                          buff->z=k;
                          buff->val=val; 
                          buff->next=MRI_var->Bound1;
                          MRI_var->Bound1=buff;
                        }
                    }
                }
            }
    
      for (i=0;i<3;i++)
        AddVoxel(MRI_var); 
    }
  
  return 0;
}


/*free the allocated Basin (in the routine Allocation)*/
static int FreeMem(MRI_variables *MRI_var)
{
  int k,j;

  for(k=0;k<MRI_var->depth;k++)
    {
      for(j=0;j<MRI_var->height;j++)
        free(MRI_var->Basin[k][j]);
      free(MRI_var->Basin[k]);
    }
  free(MRI_var->Basin);
  return 0;
}

/*not used in this hybrid approach*/
static int Save(MRI_variables *MRI_var)
{
  int i,j,k;
  BUFTYPE *pb;

  for(k=0;k<MRI_var->depth;k++)
    for (j=0;j<MRI_var->height;j++)
      {
        pb=&MRIvox(MRI_var->mri_src,0,j,k);
        for (i=0;i<MRI_var->width;i++)
          {
            if (!(MRI_var->Basin[k][j][i].type))
              *pb=0;
            pb++;          
          }
      }
  return 0;
}


/************************************************************************
 ***********************************************************************
 ************************************************************************/
/*-----------------------------------------------------
  FUNCTION Template_Deformation
  
  Parameters:
  STRIP_PARMS *:contains the parameters for the prog 
  MRI_variables *: contains the variables 

  Returns value:void

  Description: the different template deformations 
  ------------------------------------------------------*/
static void Template_Deformation(STRIP_PARMS *parms,MRI_variables *MRI_var)
{
#ifndef __OPTIMIZE__
  int brainsize;
#endif
  fprintf(stderr,"\n****************TEMPLATE DEFORMATION****************");
                    
  read_geometry(0,MRI_var,NULL);
  brain_params(MRI_var);
  ////////////////////////////////////////////////////////////////////////
  if(parms->cx!=-1)
    {
      MRI_var->xCOG=parms->cx;
      MRI_var->yCOG=parms->cy;
      MRI_var->zCOG=parms->cz;
      fprintf(stderr,"\n      modification of the brain COG: x=%d y=%d z=%d",
              (int)MRI_var->xCOG,(int)MRI_var->yCOG,(int)MRI_var->zCOG);
    }
  if(parms->rb!=-1)
    {
      MRI_var->rad_Brain=parms->rb;
      fprintf(stderr,"\n      modification of the brain radius to %d",
              (int)MRI_var->rad_Brain);
    }
  ////////////////////////////////////////////////////////////////////////
  init_surf_to_image(0.8*MRI_var->rad_Brain,
                     0.8*MRI_var->rad_Brain,
                     0.8*MRI_var->rad_Brain,
                     MRI_var);

  //  fprintf(stderr,"\n      smoothing...");
  //////////////////////////////////////////////////////////////////////
  // using smaller brain radius to expand to the surface
  // MRISshrink1(MRI_var);
  FitShape(MRI_var, 5, 150, calcForce1);
  // MRISwrite(MRI_var->mris, "surface1");
#ifndef __OPTIMIZE__
  brainsize = calcBrainSize(MRI_var->mri_src, MRI_var->mris);
  fprintf(stderr, "\n                  step1 brainsize = %d\n", brainsize);
#endif

  FreeMem(MRI_var);  /*necessary to free the basins previously allocated*/

  //if (parms->surf_dbg)
  write_image(MRI_var);

  if (parms->template_deformation!=2)
    {
      init_direction(MRI_var);

      /*compute some global intensity values*/
      // update the grey scale values for CSF etc.
      local_params(parms,MRI_var);

      DebugCurve((double *) 0,256,"end"); // close fp for debugging output

      /*peel the brain to avoid incorrect segmentations*/
      // modifies the mri_src
      MRISpeelBrain(0,MRI_var->mri_src,MRI_var->mris,0);

      /*initialize the highly-tesselated surface*/
      // note that we start from icosahedron again
      shrinkstep(MRI_var);
      fprintf(stderr,"\n      highly tesselated surface with %d vertices"
              ,MRI_var->mris->nvertices);
   
      if(parms->template_deformation==3) 
        /*use the result of the first template smoothing*/
        { 
          MRIShighlyTesselatedSmoothedSurface(MRI_var);  
#ifndef __OPTIMIZE__
          brainsize = calcBrainSize(MRI_var->mri_src, MRI_var->mris);
          fprintf(stderr, "\n                  step2+ brainsize = %d\n", 
                  brainsize);
#endif
        }
      else
        {
          fprintf(stderr,"\n      matching...");
          // MRISshrink2(MRI_var);
          FitShape(MRI_var, 1, 100, calcForce2);
          // MRISwrite(MRI_var->mris, "surface2");
#ifndef __OPTIMIZE__
          brainsize = calcBrainSize(MRI_var->mri_src, MRI_var->mris);
          fprintf(stderr, "\n                  step2 brainsize = %d\n", 
                  brainsize);
#endif
        }
      fprintf(stderr,
              "\n\n*********************VALIDATION*********************");
      MRI_var->atlas=parms->atlas;
      // check with the atlas for any errors
      MRI_var->validation=ValidationSurfaceShape(MRI_var);
      // if too bad, then validation = 0.  Otherwise validation = 1.

      // we built mris_curv, mris_var_curv, mris_dCOG, and mris_var_dCOG in 
      // ValidationSurfaceShape().
      /*scale the fields to the current map*/
      fprintf(stderr,"\nScaling of atlas fields onto current surface fields");
      MRISsaveVertexPositions(MRI_var->mris, ORIGINAL_VERTICES) ;
      MRISscaleFields(MRI_var->mris,
                      MRI_var->mris_curv,
                      MRI_var->mris_var_curv,
                      CURV_MODE);
      MRISscaleFields(MRI_var->mris,
                      MRI_var->mris_dCOG,
                      MRI_var->mris_var_dCOG,
                      DIST_MODE); 
      // if validation failed and user asked for atlas correction
      if((!MRI_var->validation) && MRI_var->atlas)
        {
          fprintf(stderr,"\nCorrecting the shape of the surface...");
          // change the surface shape using the atlas force
          MRISCorrectSurface(MRI_var);
          //if (parms->surf_dbg)
          //  write_image(MRI_var);
          /*scale the fields to the current map*/
          fprintf(stderr,
                  "\nScaling of atlas fields onto current surface fields");
          MRISsaveVertexPositions(MRI_var->mris, ORIGINAL_VERTICES) ;
          MRISscaleFields(MRI_var->mris,
                          MRI_var->mris_curv,
                          MRI_var->mris_var_curv,
                          CURV_MODE);
          MRISscaleFields(MRI_var->mris,
                          MRI_var->mris_dCOG,
                          MRI_var->mris_var_dCOG,
                          DIST_MODE); 
        }
    
      fprintf(stderr,
              "\n\n********FINAL ITERATIVE TEMPLATE DEFORMATION********");
      /*Compute local intensity values*/
      fprintf(stderr,"\nCompute Local values csf/gray");
      MRISComputeLocalValues(MRI_var);
      /*refine the segmentation based on these local values*/
      fprintf(stderr,"\nFine Segmentation...");

      ////////////////////////////////////////////////////////////////////
      MRISFineSegmentation(MRI_var);
      // MRISwrite(MRI_var->mris, "surface3");
#ifndef __OPTIMIZE__
      brainsize = calcBrainSize(MRI_var->mri_src, MRI_var->mris);
      fprintf(stderr, "\n                  step3 brainsize = %d\n", brainsize);
#endif

      ////////////////////////////////////////////////////////////////////
      MRI_var->dark_iter=parms->dark_iter;
      MRISgoToClosestDarkestPoint(MRI_var);
      // MRISwrite(MRI_var->mris, "surface4");
#ifndef __OPTIMIZE__
      brainsize = calcBrainSize(MRI_var->mri_src, MRI_var->mris);
      fprintf(stderr, "\n                  step4 brainsize = %d\n", brainsize);
#endif
      if (parms->surf_dbg)
        write_image(MRI_var);
    }
}

/*load a geometry from a file into mris*/
static void read_geometry(int type,MRI_variables *MRI_var,char *surf_fname)
{
  char fname[500], *mri_dir; 

  mri_dir = getenv("FREESURFER_HOME");
  if (mri_dir==0)
    Error("\n\nCannot find the environment variable FREESURFER_HOME\n");

  switch(type)
    {
    case 0:
      sprintf(fname,"%s/lib/bem/ic4.tri",mri_dir);
      MRI_var->mris=MRISread(fname);
      break;
    case 1:
      sprintf(fname,"%s/lib/bem/ic5.tri",mri_dir);
      MRI_var->mris=MRISread(fname);
      break;   
    case 2:
      MRI_var->mris=MRISread(surf_fname);
      break;
    }
  if(!MRI_var->mris)
    ErrorExit(ERROR_NOFILE, "Could not open the file %s\n",surf_fname) ;

  MRIScomputeNormals(MRI_var->mris);
}

static void brain_params(MRI_variables *MRI_var)
{
  int i,j,k,xmin,xmax,ymin,zmin,zmax;
  unsigned long n;
  // BUFTYPE *pb;
  double x,y,z,rad_buff;

  x=y=z=0;
  n=0;
  for (k=0;k<MRI_var->depth;k++)
    for(j=0;j<MRI_var->height;j++)
      {
        // pb=&MRIvox(MRI_var->mri_src,0,j,k);
        for(i=0;i<MRI_var->width;i++)
          if (MRI_var->Basin[k][j][i].type)
            {
              x+=i;
              y+=j;
              z+=k;
              n++;
            }
      }

  if(n==0)
    Error("\n Problem of COG calculation");

  MRI_var->xCOG=x/n;
  MRI_var->yCOG=y/n;
  MRI_var->zCOG=z/n;

  rad_buff=0;
  n=0;
  xmin = MRI_var->width;
  xmax = 0;
  ymin = MRI_var->height;
  zmin = MRI_var->depth;
  zmax = 0;
  for (k=0;k<MRI_var->depth;k++)
    for(j=0;j<MRI_var->height;j++)
      {
        // pb=&MRIvox(MRI_var->mri_src,0,j,k);
        for(i=0;i<MRI_var->width;i++)
          if(MRI_var->Basin[k][j][i].type)
            {
              if (xmin>i) xmin=i;
              if (xmax<i) xmax=i;
              if (ymin>j) ymin=j;
              if (zmin>k) zmin=k;
              if (zmax<k) zmax=k;
            }
      }

  xmax=int(MAX(xmax-MRI_var->xCOG,MRI_var->xCOG-xmin));
  zmax=int(MAX(zmax-MRI_var->zCOG,MRI_var->zCOG-zmax));
  ymin=int(MRI_var->yCOG-ymin);

  MRI_var->rad_Brain=MAX(xmax,MAX(zmax,ymin));

  if (MRI_var->rad_Brain<30)
    {
      if (MRI_var->WM_intensity==110 &&
          MRI_var->WM_VARIANCE==5 && 
          MRI_var->WM_MAX==110)
        Error("\n Watershed Error !\n");
      else
        {
          fprintf(stderr,"\n      second estimation of the COG "
                  "coord: x=%d,y=%d, z=%d, r=%d",
                  (int)MRI_var->xCOG,(int)MRI_var->yCOG,(int)MRI_var->zCOG,
                  (int)MRI_var->rad_Brain);
          Error("\n Watershed Error... Try with the T1-weighted volume\n");
        }
    }
  fprintf(stderr,"\n      second estimation of the COG coord: "
          "x=%d,y=%d, z=%d, r=%d",
          (int)MRI_var->xCOG,(int)MRI_var->yCOG,(int)MRI_var->zCOG,
          (int)MRI_var->rad_Brain);
}

static void
init_surf_to_image(float rx, float ry, float rz,MRI_variables *MRI_var)
{
  MRIS *mris;
  int k,nvertices;
  double x,y,z;
  double Rx,Ry,Rz;

  mris=MRI_var->mris;
  nvertices=mris->nvertices;
  
  myVoxelToWorld(MRI_var->mri_src,MRI_var->xCOG,MRI_var->yCOG,MRI_var->zCOG
                 ,&x,&y,&z);
  Rx=rx;Ry=rz;Rz=ry;

  for (k=0;k<nvertices;k++)
    {
      mris->vertices[k].x = Rx*mris->vertices[k].x + x;
      mris->vertices[k].y = Ry*mris->vertices[k].y + y;    
      mris->vertices[k].z = Rz*mris->vertices[k].z + z;
    }
  MRIScomputeNormals(mris);
}


static void write_image(MRI_variables *MRI_var)
{
  int i,j,imnr,k,u,v;
  float x0,y0,z0,x1,y1,z1,x2,y2,z2,d0,d1,d2,dmax;
  float px0,py0,pz0,px1,py1,pz1,px,py,pz;
  int numu,numv;
  double tx,ty,tz;

  MRIS *mris=MRI_var->mris;

  for (k=0;k<mris->nfaces;k++)
    {
      x0 =mris->vertices[mris->faces[k].v[0]].x;    
      y0 =mris->vertices[mris->faces[k].v[0]].y;    
      z0 =mris->vertices[mris->faces[k].v[0]].z;    
      x1 =mris->vertices[mris->faces[k].v[1]].x;    
      y1 =mris->vertices[mris->faces[k].v[1]].y;    
      z1 =mris->vertices[mris->faces[k].v[1]].z;    
      x2 =mris->vertices[mris->faces[k].v[2]].x;    
      y2 =mris->vertices[mris->faces[k].v[2]].y;    
      z2 =mris->vertices[mris->faces[k].v[2]].z;    
      d0 = sqrt(SQR(x1-x0)+SQR(y1-y0)+SQR(z1-z0));
      d1 = sqrt(SQR(x2-x1)+SQR(y2-y1)+SQR(z2-z1));
      d2 = sqrt(SQR(x0-x2)+SQR(y0-y2)+SQR(z0-z2));
      dmax = (d0>=d1&&d0>=d2)?d0:(d1>=d0&&d1>=d2)?d1:d2;
      numu = int(ceil(2.*d0));
      numv = int(ceil(2.*dmax));


      for (v=0;v<=numv;v++)
        {
          px0 = x0 + (x2-x0)*v/numv;
          py0 = y0 + (y2-y0)*v/numv;
          pz0 = z0 + (z2-z0)*v/numv;
          px1 = x1 + (x2-x1)*v/numv;
          py1 = y1 + (y2-y1)*v/numv;
          pz1 = z1 + (z2-z1)*v/numv;
          for (u=0;u<=numu;u++)
            {
              px = px0 + (px1-px0)*u/numu;
              py = py0 + (py1-py0)*u/numu;
              pz = pz0 + (pz1-pz0)*u/numu;
        
              myWorldToVoxel(MRI_var->mri_orig,px,py,pz,&tx,&ty,&tz);
  
              imnr=(int)(tz+0.5);
              j=(int)(ty+0.5);
              i=(int)(tx+0.5);
        
        
              if (i>=0 && i<MRI_var->width && 
                  j>=0 && j<MRI_var->height && 
                  imnr>=0 && imnr<MRI_var->depth)
                MRIvox(MRI_var->mri_dst,i,j,imnr) = 255;
            }
        }
    }
  for (k=0;k<mris->nvertices;k++)
    {
      px=mris->vertices[k].x;
      py=mris->vertices[k].y;
      pz=mris->vertices[k].z;
      
      myWorldToVoxel(MRI_var->mri_orig,px,py,pz,&tx,&ty,&tz);
  
      imnr=(int)(tz+0.5);
      j=(int)(ty+0.5);
      i=(int)(tx+0.5);

      if (i>=0 && i<MRI_var->width && 
          j>=0 && j<MRI_var->height && 
          imnr>=0 && imnr<MRI_var->depth)
        MRIvox(MRI_var->mri_dst,i,j,imnr) = 80;
    }
}

/*Initialize 26 vectors in "each direction" */
static void init_direction(MRI_variables *MRI_var)
{
  int i,j,k,p=0;
  float norm;
  
  // 3 x 3 x 3 directions (except the center (0,0,0))
  for(i=-1;i<2;i++)  // -1, 0, 1
    for(j=-1;j<2;j++) // -1, 0, 1
      for(k=-1;k<2;k++) // -1, 0, 1  
        if(i || j || k)
          {
            norm=sqrt(SQR(i)+SQR(j)+SQR(k));
            MRI_var->direction[p][0]=i/norm;
            MRI_var->direction[p][1]=j/norm;
            MRI_var->direction[p++][2]=k/norm;
          }
}

/* Find 2 normals to a  vector nx, ny, nz */
static void find_normal(const float nx, const float ny, const float nz,
                        float* n1,float *n2,  float direction[26][3])
{
  float ps,ps_buff;
  int p,k;
  
  k=0;
  ps=10;
  for(p=0;p<26;p++)
    {
      ps_buff=direction[p][0]*nx+direction[p][1]*ny+direction[p][2]*nz;
      if (ps_buff<0)
        ps_buff=-ps_buff;
      if (ps_buff<ps)
        {
          ps=ps_buff;
          k=p;
        }
    }
  // n1 = direction[k]
  n1[0]=direction[k][0];
  n1[1]=direction[k][1];
  n1[2]=direction[k][2];

  // n2 = n x n1
  n2[0]=ny*n1[2]-nz*n1[1];
  n2[1]=nz*n1[0]-nx*n1[2];
  n2[2]=nx*n1[1]-ny*n1[0];

  ps=sqrt(SQR(n2[0])+SQR(n2[1])+SQR(n2[2]));
  
  if(ps==0)
    Error("\n Problem in find normal ");

  n2[0]/=ps;
  n2[1]/=ps;
  n2[2]/=ps;

}

//////////////////////////////////////////////////////////////////////
// analyze CSF, GM, values etc using the surface obtained so far
static void local_params(STRIP_PARMS *parms,MRI_variables *MRI_var)
{
  unsigned long CSF_percent[256];
  float CSF_var[256];
  unsigned long int_percent[256];
  int kv,h,i,j,k,rp;
  int val,val_buff,ninside=30;
  unsigned long m,n;
  float tmp;
  int stop;
  float n1[3],n2[3];
  float var;
  int a,b,c=0;
  unsigned char tab[4][9];
  double px,py,pz,tx,ty,tz;
  MRIS* mris;

  mris=MRI_var->mris;

  /////////////////////////////////////////////////////////////////////////
  /*Determination of CSF_intensity*/
  /////////////////////////////////////////////////////////////////////////
  stop=MRI_var->CSF_intensity*3;
  
  // initialize working tmp
  for(k=0;k<256;k++)
    {
      CSF_var[k]=0;
      CSF_percent[k]=0;
      int_percent[k]=0;
    }

  n = 0;
  for (kv=0;kv<mris->nvertices;kv++)
    {
      find_normal(mris->vertices[kv].nx,
                  mris->vertices[kv].ny,
                  mris->vertices[kv].nz,
                  n1,n2,MRI_var->direction);
      //
      mris->vertices[kv].e1x=n1[0];
      mris->vertices[kv].e1y=n1[1];
      mris->vertices[kv].e1z=n1[2];
      //
      mris->vertices[kv].e2x=n2[0];
      mris->vertices[kv].e2y=n2[1];
      mris->vertices[kv].e2z=n2[2];
      //
      val=MRI_var->Imax;
      rp=0;
      for(h=-2;h<2;h++)  // near surface area by h=-2, -1, 0, 1
        {
          px=mris->vertices[kv].x-h*mris->vertices[kv].nx;
          py=mris->vertices[kv].y-h*mris->vertices[kv].ny;
          pz=mris->vertices[kv].z-h*mris->vertices[kv].nz;
          // get the voxel coordinates
          myWorldToVoxel(MRI_var->mri_src,px,py,pz,&tx,&ty,&tz);
          i=(int)(tx+0.5);
          j=(int)(ty+0.5);
          k=(int)(tz+0.5);
          // if inside the volume
          if (!(k<0||k>=MRI_var->depth||
                i<0||i>=MRI_var->width||
                j<0||j>=MRI_var->height))
            {
              // get the voxel value
              val_buff=MRIvox(MRI_var->mri_src,i,j,k);
              // find the min with h value
              if(val>val_buff)
                {
                  rp=h;              // the height where min occurred.
                  val=val_buff;
                }
            }
        }
      //  
      if (val<3*MRI_var->CSF_intensity)  
        {
          m=0;
          for(a=-1;a<2;a++)        // orthogonal to normal direction
            for(b=-1;b<2;b++)      // orthogonal to normal direction
              {
                px=mris->vertices[kv].x -rp*
                  mris->vertices[kv].nx + a*n1[0]+b*n2[0];
                py=mris->vertices[kv].y -rp*
                  mris->vertices[kv].ny + a*n1[1]+b*n2[1];
                pz=mris->vertices[kv].z -rp*
                  mris->vertices[kv].nz + a*n1[2]+b*n2[2];

                myWorldToVoxel(MRI_var->mri_dst,px,py,pz,&tx,&ty,&tz);
                i=(int)(tx+0.5);
                j=(int)(ty+0.5);
                k=(int)(tz+0.5);

                if (!(k<0||k>=MRI_var->depth||
                      i<0||i>=MRI_var->width||
                      j<0||j>=MRI_var->height))
                  {
                    val=MRIvox(MRI_var->mri_src,i,j,k);
                    CSF_percent[val]++; // create histogram
                    n++;
                  }
              }
        }
    }
  //
  analyseCSF(CSF_percent,MRI_var);  
#ifndef __OPTIMIZE__
  fprintf(stderr,"\nanalyseCSF result : "
          "CSF_MIN=%d, CSF_intensity=%d, CSF_MAX=%d ",
          MRI_var->CSF_MIN, MRI_var->CSF_intensity,MRI_var->CSF_MAX);
#endif
  /////////////////////////////////////////////////////////////////////////////
  /*Determination of MRI_var->GM_intensity*/
  /////////////////////////////////////////////////////////////////////////////
  n = 0;
  for (kv=0;kv<mris->nvertices;kv++)
    {
      // get back two normals
      n1[0]=mris->vertices[kv].e1x;
      n1[1]=mris->vertices[kv].e1y;
      n1[2]=mris->vertices[kv].e1z;

      n2[0]=mris->vertices[kv].e2x;
      n2[1]=mris->vertices[kv].e2y;
      n2[2]=mris->vertices[kv].e2z;

      m=0;
      // look inside surface normal by 0 to 26 with 3 x 3 x 4 tuple
      for(h=0;h<ninside-4;h++) // ninside = 30  goes deeper
        {
          for(c=0;c<4;c++)        // 3 x 3 x 4 
            for (a=-1;a<2;a++)    // a and b are orthogonal to normal
              for (b=-1;b<2;b++)
                {
                  px=mris->vertices[kv].x-(h+c)*
                    mris->vertices[kv].nx + a*n1[0]+b*n2[0];
                  py=mris->vertices[kv].y-(h+c)*
                    mris->vertices[kv].ny + a*n1[1]+b*n2[1];
                  pz=mris->vertices[kv].z-(h+c)*
                    mris->vertices[kv].nz + a*n1[2]+b*n2[2];
                  myWorldToVoxel(MRI_var->mri_dst,px,py,pz,&tx,&ty,&tz);
                  i=(int)(tx+0.5);
                  j=(int)(ty+0.5);
                  k=(int)(tz+0.5);
     
                  // if inside the volume region
                  if (!(k<0||k>=MRI_var->depth||
                        i<0||i>=MRI_var->width||
                        j<0||j>=MRI_var->height))
                    tab[c][4+3*a+b]=MRIvox(MRI_var->mri_src,i,j,k);
                  else
                    tab[c][4+3*a+b]=0;
                }
          // if find White Matter (mean and variation 
          // are of white matter), then stop
          if (FindWM(tab,MRI_var))
            break;
        }  
      // now we found at certain depth h where white matter is.
      // if not, then int_percent[] are all zeros.  ... bug
      if(h<ninside-4)   // h is between 0 and ninside-4
        {
          for(;h>=0;h--) // back out and find the non-white matter region
            {
              //             used to be c here (h) <-- bug
              px=mris->vertices[kv].x - h*mris->vertices[kv].nx;
              py=mris->vertices[kv].y - h*mris->vertices[kv].ny;
              pz=mris->vertices[kv].z - h*mris->vertices[kv].nz;
              myWorldToVoxel(MRI_var->mri_dst,px,py,pz,&tx,&ty,&tz);
              i=(int)(tx+0.5);
              j=(int)(ty+0.5);
              k=(int)(tz+0.5);
              // if inside and grey value is less than 
              // (white matter min - variance/2)
              if (!(k<0||k>=MRI_var->depth||
                    i<0||i>=MRI_var->width||
                    j<0||j>=MRI_var->height))
                if (MRIvox(MRI_var->mri_src,i,j,k) < 
                    MRI_var->WM_MIN - MRI_var->WM_VARIANCE/2)
                  break; 
            }
          // use h now
          for (c=h;c>=0;c--) // find the region of CSF_HALF_MAX
            {
              px=mris->vertices[kv].x - c*mris->vertices[kv].nx;
              py=mris->vertices[kv].y - c*mris->vertices[kv].ny;
              pz=mris->vertices[kv].z - c*mris->vertices[kv].nz;
              myWorldToVoxel(MRI_var->mri_dst,px,py,pz,&tx,&ty,&tz);
              i=(int)(tx+0.5);
              j=(int)(ty+0.5);
              k=(int)(tz+0.5);   
              // less than CSF_HALF_MAX
              if (!(k<0||k>=MRI_var->depth||
                    i<0||i>=MRI_var->width||
                    j<0||j>=MRI_var->height))
                if (MRIvox(MRI_var->mri_src,i,j,k) < MRI_var->CSF_HALF_MAX)
                  break;
            }
          // use c now
          // now we find the GM region so create a histogram
          m=0;var=0;tmp=0;
          for(;c<h;c++)
            for(a=-1;a<2;a++)
              for(b=-1;b<2;b++)
                {
                  px=mris->vertices[kv].x - 
                    c*mris->vertices[kv].nx + 
                    a*n1[0] + b*n2[0];
                  py=mris->vertices[kv].y - 
                    c*mris->vertices[kv].ny + 
                    a*n1[1] + b*n2[1];
                  pz=mris->vertices[kv].z - 
                    c*mris->vertices[kv].nz + 
                    a*n1[2] + b*n2[2];
                  myWorldToVoxel(MRI_var->mri_dst,px,py,pz,&tx,&ty,&tz);
                  i=(int)(tx+0.5);
                  j=(int)(ty+0.5);
                  k=(int)(tz+0.5);
                  // if inside the volume
                  if (!(k<0||k>=MRI_var->depth||
                        i<0||i>=MRI_var->width||
                        j<0||j>=MRI_var->height))
                    {
                      val_buff=MRIvox(MRI_var->mri_src,i,j,k);
                      tmp+=val_buff;
                      var+=SQR(val_buff-val);
                      m++;
                      /*modification added on January 29th */
                      if(val_buff< MRI_var->WM_MIN - 
                         MRI_var->WM_VARIANCE/2 && val_buff > 
                         MRI_var->WM_MIN/4)
                        {
                          int_percent[val_buff]++; // histogramming
                          n++;
                        }
                    }
                }
        }
    }

  // analyseGM sets the value for GM_intensity, GM_MIN
  analyseGM(CSF_percent,int_percent,MRI_var);

  // set the TRANSITION_intensity to be at the weighted average 
  // assuming the height of GM_intensity and CSF_intensity are the same
  MRI_var->TRANSITION_intensity=
    MRI_var->CSF_intensity
    + (MRI_var->GM_intensity - MRI_var->CSF_intensity)
    * (MRI_var->CSF_MAX - MRI_var->CSF_intensity)
    / (MRI_var->CSF_MAX + MRI_var->GM_intensity - 
       MRI_var->GM_MIN - MRI_var->CSF_intensity);

  fprintf(stderr,"\n      before analyzing : CSF_MAX=%d, "
          "TRANSITION=%d, GM_MIN=%d, GM=%d",
          MRI_var->CSF_MAX, 
          MRI_var->TRANSITION_intensity,
          MRI_var->GM_MIN,MRI_var->GM_intensity);

  ////////////////////////////////////////////////////////////////////////
  // change CSF_MAX to be smaller of transition intensity or csf max.
  MRI_var->CSF_MAX = MIN(MRI_var->TRANSITION_intensity,MRI_var->CSF_MAX);

  //before the modification
  //MRI_var->GM_MIN=MAX(MRI_var->TRANSITION_intensity,MRI_var->GM_MIN);
  // change GM_MIN to be larger of transition intensity or GM_MIN or WM_MIN/4 
  MRI_var->GM_MIN = 
    MAX(MAX(MRI_var->TRANSITION_intensity,MRI_var->GM_MIN), MRI_var->WM_MIN/4);
  // change GM_intensity to be greater of GM_intensity or GM_MIN
  MRI_var->GM_intensity = MAX(MRI_var->GM_intensity,MRI_var->GM_MIN);

  ///////////////////////////////////////////////////////////////////////
  // further modify transition intensity, csf max, gm intensity
  if (!parms->skull_type) // default
    {
      // change transition intensity 
      MRI_var->TRANSITION_intensity=
        (2*MRI_var->GM_MIN + MRI_var->TRANSITION_intensity)/3;
      // change CSF_HALF_MAX
      MRI_var->CSF_HALF_MAX=
        MIN(MRI_var->CSF_intensity/2 
            + MIN(MRI_var->CSF_MAX, MRI_var->TRANSITION_intensity)/2,
            MRI_var->CSF_HALF_MAX);
      // 
      if (MRI_var->CSF_MAX == MRI_var->TRANSITION_intensity)
        MRI_var->CSF_MAX=
          (MRI_var->CSF_HALF_MAX + MRI_var->TRANSITION_intensity)/2;
    
      MRI_var->GM_intensity=
        (MRI_var->GM_intensity + 3*MRI_var->TRANSITION_intensity)/4;
    }
  else if (parms->skull_type==-1) // less
    {
      if (MRI_var->GM_MIN==MRI_var->TRANSITION_intensity)
        {
          MRI_var->TRANSITION_intensity=
            (3*MRI_var->GM_MIN + MRI_var->GM_intensity)/4;
          MRI_var->GM_intensity        =
            (MRI_var->GM_intensity + MRI_var->GM_MIN)/2;
        }
      else
        {
          MRI_var->TRANSITION_intensity=MRI_var->GM_MIN;
          // weight GM_MIN
          MRI_var->GM_intensity        =
            (MRI_var->GM_intensity+2*MRI_var->GM_MIN)/3;
        };
      MRI_var->CSF_HALF_MAX
        =MIN(MRI_var->CSF_intensity/2 + 
             MIN(MRI_var->CSF_MAX, MRI_var->TRANSITION_intensity)/2,
             MRI_var->CSF_HALF_MAX);
      MRI_var->CSF_MAX=(MRI_var->CSF_HALF_MAX+MRI_var->TRANSITION_intensity)/2;

    }
  else if (parms->skull_type==1) // more 
    {
      if (MRI_var->GM_MIN==MRI_var->TRANSITION_intensity)
        {
          MRI_var->TRANSITION_intensity=
            (MRI_var->CSF_intensity+3*MRI_var->TRANSITION_intensity)/4;
          MRI_var->GM_intensity        = 
            MIN(MRI_var->GM_MIN,
                (MRI_var->GM_intensity+3*MRI_var->TRANSITION_intensity)/4);
        }
      else
        {
          MRI_var->TRANSITION_intensity=
            (MRI_var->GM_MIN + MRI_var->TRANSITION_intensity)/2;
          // weight toward transition intensity
          MRI_var->GM_intensity        =
            (MRI_var->GM_intensity + 3*MRI_var->TRANSITION_intensity)/4;
        }
      MRI_var->CSF_HALF_MAX=
        MIN((MRI_var->CSF_intensity+
             MIN(MRI_var->CSF_MAX,MRI_var->TRANSITION_intensity))/2
                                ,MRI_var->CSF_HALF_MAX);
      if (MRI_var->CSF_MAX>=MRI_var->TRANSITION_intensity)
        MRI_var->CSF_MAX=
          (MRI_var->CSF_HALF_MAX+MRI_var->TRANSITION_intensity)/2;
    };

  fprintf(stderr,"\n      after  analyzing : CSF_MAX=%d, "
          "TRANSITION=%d, GM_MIN=%d, GM=%d",
          MRI_var->CSF_MAX, 
          MRI_var->TRANSITION_intensity, 
          MRI_var->GM_MIN, 
          MRI_var->GM_intensity);

  // if manual options are done, use them
  if(parms->manual_params==1)
    {
      fprintf(stderr,"\n      Modification of the parameters manually");
      MRI_var->CSF_MAX=parms->manual_CSF_MAX;
      MRI_var->TRANSITION_intensity=parms->manual_TRANSITION_intensity;
      MRI_var->GM_intensity=parms->manual_GM_intensity;
      fprintf(stderr,"\n      after  analyzing : "
              "CSF_MAX=%d, TRANSITION_intensity=%d, GM=%d, GM_MIN=%d",
              MRI_var->CSF_MAX, 
              MRI_var->TRANSITION_intensity,
              MRI_var->GM_intensity, 
              MRI_var->GM_MIN);
    }
}

// coming in is the histogram of possible CSF values
static void analyseCSF(unsigned long *CSF_percent,MRI_variables *MRI_var)
{
  int k,n,csf,tp;
  float tmp,buff;
  float a,b,Sxy,Sx,Sy,Sxx;

  tp=MRI_var->CSF_intensity;
  
  lisse(CSF_percent, "CSF_percent");
  
  DebugCurve(CSF_percent, 3*MRI_var->CSF_intensity, "\nCSF_percent\n");

  csf=0;    
  tmp=0;
  for (k=0;k<3*MRI_var->CSF_intensity;k++)
    {    
      buff=CSF_percent[k];
      if(tmp<buff)
        {
          tmp=buff;
          csf=k;
        }
    }

  k=csf;
  for(n=k;n<3*MRI_var->CSF_intensity;n++)
    if(CSF_percent[n]>=CSF_percent[k]/2)
      MRI_var->CSF_HALF_MAX=n;
    else
      break;
  
  k=MRI_var->CSF_HALF_MAX;
  for(n=k;n<3*MRI_var->CSF_intensity;n++)
    if(CSF_percent[n]>=CSF_percent[k]/2)
      MRI_var->CSF_MAX=n+2;
    else
      break;

  MRI_var->CSF_intensity=csf;
  
  n=MRI_var->CSF_MAX-MRI_var->CSF_intensity+1;
  Sxy = Sx = Sy = Sxx = 0;
  for(k=MRI_var->CSF_intensity;k<=MRI_var->CSF_MAX;k++)
    {
      Sxy+=(float)k*CSF_percent[k];
      Sx+=k;
      Sy+=CSF_percent[k];
      Sxx+=k*k;
    }
  // Is this a good approximation?
  // CSF distribution is assumed to be
  // a triangle shape
  //
  // assume linear equation y = b + ax
  //
  // a = (n*Sxy  - Sx*Sy)/(n*Sxx - Sx*Sx)
  // b = (Sxx*Sy - Sx*Sy)/(n*Sxx - Sx*Sx)
  // crossing point at x = -b/a
  //
  // note that n*Sxx - Sx*Sx != 0
  a=(n*Sxy-Sy*Sx)/(n*Sxx-Sx*Sx);
  b=-(a*Sx-Sy)/n;

  if(DZERO(a) || !finite(a))
    fprintf(stderr, "\n Problem with the least square "
            "interpolation for CSF_MAX");
  // don't change the value
  else
    MRI_var->CSF_MAX=int(-b/a);
  

  k=MRI_var->CSF_intensity;
  for(n=k;n>=0;n--)
    if(CSF_percent[n]>=CSF_percent[k]/10)
      MRI_var->CSF_MIN=n-1;
    else
      break;
  MRI_var->CSF_MIN=MAX(0,MRI_var->CSF_MIN);
  

  n=MRI_var->CSF_intensity-MRI_var->CSF_MIN+1;
  Sxy = Sx = Sy = Sxx = 0;
  for(k=MRI_var->CSF_MIN;k<=MRI_var->CSF_intensity;k++)
    {
      Sxy+=(float)k*CSF_percent[k];
      Sx+=k;
      Sy+=CSF_percent[k];
      Sxx+=k*k;
    }
  // min side
  a=(n*Sxy-Sy*Sx)/(n*Sxx-Sx*Sx);
  b=-(a*Sx-Sy)/n;

  if(DZERO(a) || !finite(a))
    fprintf(stderr, "\n Problem with the least square "
            "interpolation for CSF_MIN");
  else
    MRI_var->CSF_MIN= int ((MAX(0,-b/a) + MRI_var->CSF_MIN)/2.);

  if (MRI_var->CSF_MIN < 0)
    MRI_var->CSF_MIN=0;

  a=1/((float)MRI_var->CSF_MAX - 3*tp);
  b=(float)3*tp/(3*tp-MRI_var->CSF_MAX);
  for(k=MRI_var->CSF_MAX;k<=3*tp;k++)
    CSF_percent[k]*= (long unsigned int) (a*k+b);
  for(;k<256;k++)
    CSF_percent[k]=0;
}

static void analyseGM(unsigned long *CSF_percent,
                      unsigned long *int_percent,MRI_variables *MRI_var)
{
  int k,n,gm_int;
  float tmp,buff;
  double tab[256];
  float a,b,Sxy,Sx,Sy,Sxx;

  ///////////////////////////////////////////////////////////
  // tuple analysis GM histogram
  //////////////////////////////////////////////////////////
  // smooth the histogram
  lisse(int_percent, "GM percent");
    
  // get GM population around CSF_intensity area (5 bins)
  tmp=0;
  for(k=-2;k<3;k++) 
    tmp+=(float)int_percent[MRI_var->CSF_intensity+k];
  
  // get CSF population around CSF_intensity
  buff=0;
  for(k=-2;k<3;k++)
    buff+=CSF_percent[MRI_var->CSF_intensity+k];
  
  // null the value up to CSF_intensity
  for(k=0;k<MRI_var->CSF_intensity;k++)
    int_percent[k]=0;

  // above CSF_intensity, subtract the CSF contribution
  for(;k<256;k++)
    int_percent[k]=
      (long unsigned int) (MAX(0, int_percent[k] - tmp*CSF_percent[k]/buff));

  DebugCurve(int_percent, MRI_var->WM_intensity, 
             "\nGM curve from tuple analysis\n");
  
  /////////////////////////////////////////////////////////
  // using the basin analysis GM histogram
  /////////////////////////////////////////////////////////
  // smooth the histogram
  lisse(MRI_var->gmnumber, "GM number");

  // get the GM population around CSF_intensity
  tmp=0;
  for(k=-2;k<3;k++)
    tmp+=(float)MRI_var->gmnumber[MRI_var->CSF_intensity+k];
  
  // get the CSF population around CSF_intensity
  buff=0;
  for(k=-2;k<3;k++)
    buff+=CSF_percent[MRI_var->CSF_intensity+k];
  
  // null the population up to CSF_intensity
  for(k=0;k<MRI_var->CSF_intensity;k++)
    MRI_var->gmnumber[k]=0;

  // above CSF intensity, subtract the CSF contribution
  for(;k<256;k++)
    MRI_var->gmnumber[k]=
      (long unsigned int) (MAX(0,MRI_var->gmnumber[k]-tmp*
                               CSF_percent[k]/buff));

  DebugCurve(MRI_var->gmnumber, MRI_var->WM_intensity, 
             "\nGM curve from basin analysis\n");

  /////////////////////////////////////////////////////////
  // use gmnumber to get the GM values (tosa)
  // in order to combat the frequent cases of tuple analysis failures
  /////////////////////////////////////////////////////////
  MRI_var->GM_intensity= findMaxIndex(MRI_var->gmnumber);
  int gm_half_min = findHalfMin(MRI_var->gmnumber, MRI_var->GM_intensity);
  Sxy = Sx = Sy = Sxx = 0;
  n = 0;
  for(k=gm_half_min;k<=MRI_var->GM_intensity;k++)
    {
      Sxy+=(float) k*MRI_var->gmnumber[k];
      Sx+=k;
      Sy+=MRI_var->gmnumber[k];
      Sxx+=k*k;
      n++;
    }
  a=(n*Sxy-Sy*Sx)/(n*Sxx-Sx*Sx);
  b=-(a*Sx-Sy)/n;
  if(DZERO(a) || !finite(a))
    fprintf(stderr, 
            "\n Problem with the least square interpolation "
            "in GM_MIN calculation.");
  else
    MRI_var->GM_MIN=int(MAX(0,-b/a));
#ifndef __OPTIMIZE__
  fprintf(stderr, "\n      gmnumber lead to GM_intensity = %d, GM_MIN = %d\n", 
          MRI_var->GM_intensity, MRI_var->GM_MIN);
#endif

  // if the original fails, we use these values

  //////////////////////////////////////////////////////////
  // original analysis
  // count the number of voxels in gmnumber[]
  tmp=0;  
  for(k=0;k<256;k++)
    tmp+=MRI_var->gmnumber[k];
  
  // if less than 100,000 then don't count them on for decision
  if(tmp<100000)
    for(k=0;k<256;k++)
      MRI_var->gmnumber[k]=1;

  // multiply int_percent[] and gmnumber[]
  for(k=0;k<256;k++)
    tab[k]=MRI_var->gmnumber[k]*int_percent[k];

  DebugCurve(tab, MRI_var->WM_intensity, 
             "\nTable for gmnumber * int_percent\n");

  // find the index of the peak population
  tmp=0;
  for(k=0;k<MRI_var->WM_MIN;k++)
    if(tab[k]>tmp)
      {
        MRI_var->GM_intensity=k;
        tmp=tab[k];
      }
  gm_int=MRI_var->GM_intensity-1;
  // find the index for the 1/4 of the peak population
  for(k=MRI_var->GM_intensity;k>=0;k--)
    if(tab[k]>=tab[MRI_var->GM_intensity]/4)
      gm_int=k-1;
    else 
      break;
  
  gm_int=MAX(3*gm_int/2 - MRI_var->GM_intensity/2,0);

  n=MRI_var->GM_intensity - gm_int + 1;
  Sxy = Sx = Sy = Sxx = 0;
  for(k=gm_int;k<=MRI_var->GM_intensity;k++)
    {
      Sxy+=(float)k*tab[k];
      Sx+=k;
      Sy+=tab[k];
      Sxx+=k*k;
    }

  a=(n*Sxy-Sy*Sx)/(n*Sxx-Sx*Sx);
  b=-(a*Sx-Sy)/n;
  if(DZERO(a) || !finite(a))
    fprintf(stderr, "\n Problem with the least square "
            "interpolation in GM_MIN calculation.");
  else
    MRI_var->GM_MIN=int(MAX(0,-b/a));
}

/* smooth the 256-tab curve*/
static void lisse(unsigned long *tab, const char *msg)
{
  int k,n,p;
  unsigned long tmp[3];
  unsigned long buff;

  // DebugCurve(tab, 256, "\nBefore averaging\n");

  p=2;
  tmp[0]=0;
  tmp[1]=0;
  for(k=2;k<254;k++)
    {
      buff=0;
      for(n=-2;n<3;n++)   // k-2, k-1, k, k+1, k+2
        buff+=tab[k+n]/5; // average over the 5 values around that point
      tmp[p]=buff;
      p=(p+1)%3;
      tab[k-2]=tmp[p];
    }
  p=(p+1)%3;
  tab[252]=tmp[p];
  p=(p+1)%3;
  tab[253]=tmp[p];
  tab[254]=0;
  tab[255]=0;

  // DebugCurve(tab, 256, "\nAfter averaging\n");
}

static int FindWM(unsigned char tab[4][9],MRI_variables *MRI_var)
{
  int k,p;
  float mean=0,var=0;
  // calculate mean value of the tuple
  for(k=0;k<4;k++)
    for(p=0;p<9;p++)
      mean+=tab[k][p];

  mean/=36;

  // if mean value is less than WM_MIN or WM_MAX, then no whitematter
  if(mean<MRI_var->WM_MIN || mean>MRI_var->WM_MAX)
    return 0;      

  // calculate the variation 
  for(k=0;k<4;k++)
    for(p=0;p<9;p++)
      var+=SQR(tab[k][p]-mean);

  var=sqrt(var/36);
  
  // if the variance is greater than white matter variance, then no whitematter
  // is this correct? 
  if(var>MRI_var->WM_VARIANCE)
    return 0;
  else
    return 1;
}

// expand the surface by "h" and create a volume which has 
// "val" outside of this surface
static unsigned long MRISpeelBrain(float h,
                                   MRI* mri_dst,
                                   MRIS *mris,unsigned char val)
{
  int i,j,k,imnr; 
  float x0,y0,z0,x1,y1,z1,x2,y2,z2,d0,d1,d2,dmax,u,v;
  float px,py,pz,px0,py0,pz0,px1,py1,pz1;
  int numu,numv,totalfilled,newfilled;
  double tx,ty,tz;
  unsigned long brainsize;

  int width, height,depth;
  MRI *mri_buff;

  width=mri_dst->width;
  height=mri_dst->height;
  depth=mri_dst->depth;

  mri_buff= MRIalloc(width, height, depth, MRI_UCHAR) ;

  for (k=0;k<mris->nvertices;k++)
    {
      // cache the values
      mris->vertices[k].tx=mris->vertices[k].x;
      mris->vertices[k].ty=mris->vertices[k].y;
      mris->vertices[k].tz=mris->vertices[k].z;

      // expand by h using normal
      mris->vertices[k].x +=h*mris->vertices[k].nx;
      mris->vertices[k].y +=h*mris->vertices[k].ny;
      mris->vertices[k].z +=h*mris->vertices[k].nz;
    }


  for (k=0;k<mris->nfaces;k++)
    {
      // calculate three vertices
      x0 =mris->vertices[mris->faces[k].v[0]].x;    
      y0 =mris->vertices[mris->faces[k].v[0]].y;    
      z0 =mris->vertices[mris->faces[k].v[0]].z;    
      x1 =mris->vertices[mris->faces[k].v[1]].x;    
      y1 =mris->vertices[mris->faces[k].v[1]].y;    
      z1 =mris->vertices[mris->faces[k].v[1]].z;    
      x2 =mris->vertices[mris->faces[k].v[2]].x;    
      y2 =mris->vertices[mris->faces[k].v[2]].y;    
      z2 =mris->vertices[mris->faces[k].v[2]].z;
      // calculate the sides
      d0 = sqrt(SQR(x1-x0)+SQR(y1-y0)+SQR(z1-z0));
      d1 = sqrt(SQR(x2-x1)+SQR(y2-y1)+SQR(z2-z1));
      d2 = sqrt(SQR(x0-x2)+SQR(y0-y2)+SQR(z0-z2));
      dmax = (d0>=d1&&d0>=d2)?d0:(d1>=d0&&d1>=d2)?d1:d2;
      numu = int(ceil(2*d0));
      numv = int(ceil(2*dmax));

      
      for (v=0;v<=numv;v++)
        {
          px0 = x0 + (x2-x0)*v/numv;
          py0 = y0 + (y2-y0)*v/numv;
          pz0 = z0 + (z2-z0)*v/numv;
          px1 = x1 + (x2-x1)*v/numv;
          py1 = y1 + (y2-y1)*v/numv;
          pz1 = z1 + (z2-z1)*v/numv;
          for (u=0;u<=numu;u++)
            {
              px = px0 + (px1-px0)*u/numu;
              py = py0 + (py1-py0)*u/numu;
              pz = pz0 + (pz1-pz0)*u/numu;

              myWorldToVoxel(mri_dst,px,py,pz,&tx,&ty,&tz);
        
              imnr=(int)(tz+0.5);
              j=(int)(ty+0.5);
              i=(int)(tx+0.5);
              if (i>=0 && i<width && j>=0 && j<height && imnr>=0 && imnr<depth)
                MRIvox(mri_buff,i,j,imnr) = 255;
                                
            }  
        }
    }

  MRIvox(mri_buff,1,1,1)= 64; // starting (1,1,1) means that 
  // the edge voxels not marked as 64
  totalfilled = newfilled = 1;
  while (newfilled>0)
    {
      newfilled = 0;
      for (k=0;k<depth;k++)
        for (j=0;j<height;j++)
          for (i=0;i<width;i++)
            if (MRIvox(mri_buff,i,j,k)==0)
              if (MRIvox(mri_buff,i,j,mri_buff->zi[k-1])==
                  64||MRIvox(mri_buff,i,mri_buff->yi[j-1],k)==64||
                  MRIvox(mri_buff,mri_buff->xi[i-1],j,k)==64)
                {
                  MRIvox(mri_buff,i,j,k)= 64;
                  newfilled++;
                }
      for (k=depth-1;k>=0;k--)
        for (j=height-1;j>=0;j--)
          for (i=width-1;i>=0;i--)
            if (MRIvox(mri_buff,i,j,k)==0)
              if (MRIvox(mri_buff,i,j,mri_buff->zi[k+1])==
                  64||MRIvox(mri_buff,i,mri_buff->yi[j+1],k)==64||
                  MRIvox(mri_buff,mri_buff->xi[i+1],j,k)==64)
                {
                  MRIvox(mri_buff,i,j,k) = 64;
                  newfilled++;
                }
      totalfilled += newfilled;
    }
  // fill all surface boundary voxels to be 64 (there are 6 faces)
  for (k=0; k < depth;k++)
    for (j=0; j < height; j++)
      {
        MRIvox(mri_buff,       0, j, k ) = 64;
        MRIvox(mri_buff, width-1, j, k ) = 64;
      }
  
  for (k=0; k < depth;k++)
    for (i=0; i < width ; i++)
      {
        MRIvox(mri_buff, i,        0, k ) = 64;
        MRIvox(mri_buff, i, height-1, k ) = 64;
      }

  for (i=0; i < width ;i++)
    for (j=0; j < height; j++)
      {
        MRIvox(mri_buff, i, j,      0 ) = 64;
        MRIvox(mri_buff, i, j, depth-1) = 64;
      }

  // modify mri_dst so that outside = 0
  brainsize=0;
  if(val==0)
    for (k=0;k<depth;k++)
      for (j=0;j<height;j++)
        for (i=0;i<width;i++)
          {
            if (MRIvox(mri_buff,i,j,k)==64)
              MRIvox(mri_dst,i,j,k) = 0;
            else
              brainsize++;
          }
  else{
    for (k=0;k<depth;k++)
      for (j=0;j<height;j++)
        for (i=0;i<width;i++)
          {
            if (MRIvox(mri_buff,i,j,k)!=64)
              MRIvox(mri_dst,i,j,k) = val;
            else
              brainsize++;
          }
  }
  // restore the surface
  for (k=0;k<mris->nvertices;k++)
    {
      mris->vertices[k].x=mris->vertices[k].tx;
      mris->vertices[k].y=mris->vertices[k].ty;
      mris->vertices[k].z=mris->vertices[k].tz;
    }
  // calculate the normals
  MRIScomputeNormals(mris);
        
  MRIfree(&mri_buff);
  fprintf(stderr,"\n      mri_strip_skull: done peeling brain");
  return brainsize;
}

// calculate the center of graivty using the current surface
// and replace with the highly tessellated surface at
// the center of gravity
static void shrinkstep(MRI_variables *MRI_var)
{
  int k;
  double x,y,z,rx,ry,rz;
  double tx,ty,tz;
  float xmin=MRI_var->width,\
    xmax=0,ymin=MRI_var->height,ymax=0,zmin=MRI_var->depth,zmax=0;
  MRIS *mris;

  mris=MRI_var->mris;

  MRI_var->xsCOG=MRI_var->ysCOG=MRI_var->zsCOG=0;

  // use the current surface to
  // get the center of gravity and min, max in each direction
  // from the current surface (RAS)
  for (k=0;k<mris->nvertices;k++)
    {
      x=mris->vertices[k].x;
      y=mris->vertices[k].y;
      z=mris->vertices[k].z;

      MRI_var->xsCOG+=x;
      MRI_var->ysCOG+=y;
      MRI_var->zsCOG+=z;

      if (x<xmin)
        xmin=x;
      if (x>xmax)
        xmax=x;
      if (y<ymin)
        ymin=y;
      if (y>ymax)
        ymax=y;
      if (z<zmin)
        zmin=z;
      if (z>zmax)
        zmax=z;
    }
  // get center of gravity
  MRI_var->xsCOG/=mris->nvertices;
  MRI_var->ysCOG/=mris->nvertices;
  MRI_var->zsCOG/=mris->nvertices;
  // radius estimate
  rx=MIN(xmax-MRI_var->xsCOG,MRI_var->xsCOG-xmin);
  ry=MIN(ymax-MRI_var->ysCOG,MRI_var->ysCOG-ymin);
  rz=MIN(zmax-MRI_var->zsCOG,MRI_var->zsCOG-zmin);
  // remove the surface
  MRISfree(&MRI_var->mris);

  ///////////////////////////////////////////////////////////////////
  // read the highly tessellated surface
  read_geometry(1,MRI_var,NULL);
  // set it
  mris=MRI_var->mris;
  // put the icosahedron at the center of gravity
  // icosahedron has radius of 1 and thus multiply by (rx, ry, rz)
  for (k=0;k<mris->nvertices;k++)
    {
      mris->vertices[k].x = rx*mris->vertices[k].x + MRI_var->xsCOG;
      mris->vertices[k].y = ry*mris->vertices[k].y + MRI_var->ysCOG;    
      mris->vertices[k].z = rz*mris->vertices[k].z + MRI_var->zsCOG;
    }
  // get the voxel values
  myWorldToVoxel(MRI_var->mri_src,MRI_var->xsCOG,MRI_var->ysCOG
                 ,MRI_var->zsCOG,&tx,&ty,&tz);
  // set the center of gravity as voxel indices.
  MRI_var->xCOG=tx;
  MRI_var->yCOG=ty;
  MRI_var->zCOG=tz;
}

static void MRIShighlyTesselatedSmoothedSurface(MRI_variables *MRI_var)
{
  float x,y,z,sx,sy,sz,sd,sxn,syn,szn,sxt,syt,szt,nc;
  float force,force1;

  float d,dx,dy,dz,nx,ny,nz;
  VERTEX *v; 
  int iter,k,m,n;
  int it,jt,kt,niter;
  float decay=0.8,update=0.9;

  float val;
  int int_smooth=1;
  
  MRIS *mris;
  //  char surf_fname[500];

  double tx,ty,tz;

  double ml;
  double lm,d10m[3],d10,f1m,f2m,dm,dbuff;  
  float ***dist;
  float cout,pcout=0,coutbuff,varbuff,mean_sd[10],mean_dist[10];
 
  mris=MRI_var->mris;

  MRISsetNeighborhoodSize(mris, 1) ;
  MRIScomputeNormals(mris);

  /////////////////////////////////////////////////////////////////////
  // initialize
  dist = (float ***) malloc( mris->nvertices*sizeof(float**) );

  for( it = 0; it < mris->nvertices; it++ ) 
    {
      dist[it] = (float**) malloc( 4*sizeof(float*) );
      for( jt = 0; jt < 4; jt++ ) 
        {
          dist[it][jt] = (float*) calloc( 3, sizeof(float));
        }  
    }

  for(k=0;k<mris->nvertices;k++)
    for (m=0;m<4;m++)
      for (n=0;n<3;n++)
        dist[k][m][n]=0;

  for (n=0;n<10;n++)
    {
      mean_sd[n]=0;
      mean_dist[n]=0;
    }

  niter =int_smooth;
  force = 0.0f ; 
  pcout=0;

  for (k=0;k<mris->nvertices;k++)
    {
      v = &mris->vertices[k];
      v->odx = 0;
      v->ody = 0;
      v->odz = 0;
    }


  // iteration starts here
  /////////////////////////////////////////////////////////////////////////////
  ml=2;
  for (iter=0;niter;iter++)
    {
      cout = lm = d10 = f1m = f2m = dm = 0;
      for (k=0;k<mris->nvertices;k++)
        {
          v = &mris->vertices[k];
          v->tx = v->x;
          v->ty = v->y;
          v->tz = v->z;
        }

      for (k=0;k<mris->nvertices;k++)
        {
          v = &mris->vertices[k];
          x = v->tx;
          y = v->ty;
          z = v->tz;
          nx = v->nx;
          ny = v->ny;
          nz = v->nz;
          sx=sy=sz=sd=0;
          n=0;
          // calculate a vector points to average neighbor
          for (m=0;m<v->vnum;m++)
            {
              sx += dx =mris->vertices[v->v[m]].tx - x;
              sy += dy =mris->vertices[v->v[m]].ty - y;
              sz += dz =mris->vertices[v->v[m]].tz - z;
              sd += sqrt(dx*dx+dy*dy+dz*dz);
              n++;
            }
          sx = sx/n;
          sy = sy/n;
          sz = sz/n;
          sd = sd/n;
      
          lm+=sd;

          nc = sx*nx+sy*ny+sz*nz;
      
          // Sn normal component
          sxn = nc*nx;
          syn = nc*ny;
          szn = nc*nz;
          // St tangential component
          sxt=sx-sxn;
          syt=sy-syn;
          szt=sz-szn;

          v->nc=nc;

          // force calculation
          // St force
          /////////////////////////////////////////
          force1=0.5;

          f1m+=force1;
 
          // image force
          ////////////////////////////////////////////////
          myWorldToVoxel(MRI_var->mri_orig,x,y,z,&tx,&ty,&tz);
          kt=(int)(tz+0.5);
          jt=(int)(ty+0.5);
          it=(int)(tx+0.5);
      
          // outside of the bounding box  no force
          if ((kt<0||kt>=MRI_var->depth||
               it<0||it>=MRI_var->width||
               jt<0||jt>=MRI_var->height))
            val=0;
          else // get the voxel value
            val=MRIvox(MRI_var->mri_src,it,jt,kt);

          if (!val) // push in for zero voxel value 
            force=-0.25;
          else      // push out for non-zero voxel value
            force=0.25;
      
          f2m+=force;


          force1=force1;
      
          // Delta = 0.8 x St + force1 x Sn + force x Vn
          /////////////////////////////////////////////////////
          dx = sxt*0.8 + force1*sxn + v->nx*force;
          dy = syt*0.8 + force1*syn + v->ny*force;
          dz = szt*0.8 + force1*szn + v->nz*force;
      
          // use previous and current values
          // to avoid drastic change in delta
          dx = decay*v->odx+update*dx;
          dy = decay*v->ody+update*dy;
          dz = decay*v->odz+update*dz;
      
          // too big, then reduce < 1.0
          if ((d=sqrt(dx*dx+dy*dy+dz*dz))>1.0)
            {
              dx /= d;
              dy /= d;
              dz /= d;
            }
          // cache the values
          v->odx = dx;
          v->ody = dy;
          v->odz = dz;

          d=sqrt(dx*dx+dy*dy+dz*dz);
      
          dm+=d;

          dist[k][iter%4][0]=x;
          dist[k][iter%4][1]=y;
          dist[k][iter%4][2]=z;

          d10m[0] = d10m[1] = d10m[2] = 0;

          for(n=0;n<4;n++)
            {
              d10m[0]+=dist[k][n][0]/4;
              d10m[1]+=dist[k][n][1]/4;
              d10m[2]+=dist[k][n][2]/4;
            }
    
          dbuff=0;
          for(n=0;n<4;n++)
            dbuff+=SQR(dist[k][n][0]-d10m[0])+SQR(dist[k][n][1]-d10m[1])+
              SQR(dist[k][n][2]-d10m[2]);

          d10+=dbuff/4;

          v->x += dx;
          v->y += dy;
          v->z += dz;
        }

      lm /=mris->nvertices;
      f1m /=mris->nvertices;
      f2m /=mris->nvertices;
      dm /=mris->nvertices;
      d10 /=mris->nvertices;


      ml=lm;

      mean_sd[iter%10]=lm;
      mean_dist[iter%10]=d10;

      coutbuff=0;
      for(n=0;n<10;n++)
        coutbuff+=mean_sd[n]/10;
    
      varbuff=0;
      for(n=0;n<10;n++)
        varbuff+=SQR(mean_sd[n]-coutbuff);

      cout=varbuff;

      coutbuff=0;
      for(n=0;n<10;n++)
        coutbuff+=mean_dist[n]/10;

      varbuff=0;
      for(n=0;n<10;n++)
        varbuff+=SQR(mean_dist[n]-coutbuff);

      cout+=10*varbuff;

      coutbuff=cout;

      cout=(cout+pcout)/2;

      pcout=coutbuff;
    
      MRIScomputeNormals(mris);

      /*    if ((niter==int_smooth) && !(iter % 5))
            {      
            fprintf(stderr,
            "%d: lm=%5.3f,f1m=%5.3f,f2m=%5.3f,dm=%5.3f,d10m=%5.3f,c=%5.3f\n"
            ,iter,lm,f1m,f2m,dm,d10,100*cout);
            // sprintf(surf_fname,"./test/lh.test2_%d",iter);
            //MRISwrite(mris,surf_fname);
            }*/
      if (niter==int_smooth)
        {
          if(((iter>10)&&(10000*cout<1))||(iter>60)) 
            niter--;
        }else
          niter--;
    }

  MRIScomputeNormals(mris);

  /*free memory*/
  for( it = 0; it < mris->nvertices; it++ ) 
    {
      for( jt = 0; jt < 4; jt++ ) 
        free(dist[it][jt]);   
      free(dist[it]);
    }
  free(dist);
}

//
// 5 point average with center weighted twice
//
static void mean(float tab[4][9],float *moy)
{
  int p;
  for (p=0;p<4;p++)
    moy[p]=(2*tab[p][4]+tab[p][1]+tab[p][3]+tab[p][5]+
            tab[p][7])/6;
  // picking 3*b + a + 4 (a, b are orthogonal directions to the normal
  //  1, 3, 5, 7 -> (a,b) = (0,-1), (-1,0),(1, 0), (0, 1)  
  //  4          ->       = (0, 0)
  // i.e.
  //             X
  //           X C X   center is weighted twice, the rest is once.
  //             X
}

//
//  using the neighboring average to smooth the surface
//  with niter iterations
//
static void MRISsmooth_surface(MRI_SURFACE *mris,int niter)
{
  VERTEX *v;
  int iter,k,m,n;
  float x,y,z;  

#if WRITE_SURFACES
  static int niterations=0;
  char fname[200];
#endif

  for (iter=0;iter<niter;iter++)
    {
      MRIScomputeMetricProperties(mris) ;
      // cache the vertex point values
      for (k=0;k<mris->nvertices;k++)
        {
          v = &mris->vertices[k];
          v->tx = v->x;
          v->ty = v->y;
          v->tz = v->z;
        }

      for (k=0;k<mris->nvertices;k++)
        {
          v = &mris->vertices[k];
          n=0;
          x = y = z = 0;
          for (m=0;m<v->vnum;m++)
            {
              x += mris->vertices[v->v[m]].tx;
              y += mris->vertices[v->v[m]].ty;
              z += mris->vertices[v->v[m]].tz;
              n++;
            }
          // get neighboring points average
          x/=n;
          y/=n;
          z/=n;
          // modify the vertex with itself and neighboring average
          v->x=(v->x + x)/2;  
          v->y=(v->y + y)/2;  
          v->z=(v->z + z)/2;  
        }

#if WRITE_SURFACES
      sprintf(fname,"./rh.smoothing%d",niterations);
      niterations++;
      MRISwrite(mris,fname);
#endif

    }
}

// we go into h in the surface normal direction
static void MRISshrink_surface(MRIS *mris,int h)
{
  int k;

  MRISsaveVertexPositions(mris,TMP_VERTICES);
  for (k=0;k<mris->nvertices;k++)
    {
      mris->vertices[k].x-=h*mris->vertices[k].nx;
      mris->vertices[k].y-=h*mris->vertices[k].ny;
      mris->vertices[k].z-=h*mris->vertices[k].nz;
    }
  MRIScomputeNormals(mris);
}


static void MRIVfree(MRI_variables *MRI_var)
{
  if(MRI_var->mris)
    MRISfree(&MRI_var->mris);
  if(MRI_var->mrisphere)
    MRISfree(&MRI_var->mrisphere);
  if(MRI_var->mris_curv)
    MRISfree(&MRI_var->mris_curv);
  if(MRI_var->mris_var_curv)
    MRISfree(&MRI_var->mris_var_curv);
  if(MRI_var->mris_dCOG)
    MRISfree(&MRI_var->mris_dCOG);
  if(MRI_var->mris_var_dCOG)
    MRISfree(&MRI_var->mris_var_dCOG);

  free(MRI_var);
}


/*to get the Outer Skin*/
static void MRISshrink_Outer_Skin(MRI_variables *MRI_var,MRI* mri_src)
{
  float x,y,z,sx,sy,sz,sd,sxn,syn,szn,sxt,syt,szt,nc;
  float force,force1;

  float d,dx,dy,dz,nx,ny,nz;
  VERTEX *v;
  int iter,k,m,n;
  float samp_mean[4];
  float test_samp[4][9];
  int a,b;
  int it,jt,kt,h,niter;

  float decay=0.8,update=0.9;
  float fzero;

  MRIS *mris;
  double tx,ty,tz;


  float val;
  int int_smooth=1;

  double lm,d10m[3],d10,f1m,f2m,dm,dbuff;  
  float ***dist;
  int nb_GM,nb_TR,nb_GTM;
  float cout,pcout=0,coutbuff,varbuff,mean_sd[10],mean_dist[10];
  float n1[3],n2[3];
 
  mris=MRI_var->mris;

  ///////////////////////////////////////////////////////////////
  // initialization
  dist = (float ***) malloc(mris->nvertices*sizeof(float**) );

  for( it = 0; it < mris->nvertices; it++ ) 
    {
      dist[it] = (float**) malloc( 4*sizeof(float*) );
      for( jt = 0; jt < 4; jt++ ) 
        {
          dist[it][jt] = (float*) calloc( 3, sizeof(float));
        }  
    }
  
  /*should give some correct results*/
  fzero=MIN(MRI_var->WM_intensity/3,MRI_var->CSF_MAX);

  for(k=0;k<mris->nvertices;k++)
    for (m=0;m<4;m++)
      for (n=0;n<3;n++)
        dist[k][m][n]=0;

  for (n=0;n<10;n++)
    {
      mean_sd[n]=0;
      mean_dist[n]=0;
    }

  niter =int_smooth;
  force = 0.0f ; 
  pcout=0;

  for (k=0;k<mris->nvertices;k++)
    {
      v = &mris->vertices[k];
      v->odx = 0;
      v->ody = 0;
      v->odz = 0;
    }

  /////////////////////////////////////////////////////////////////
  // iteration starts here
  for (iter=0;niter;iter++)
    {
      cout = lm = d10 = f1m = f2m = dm = 0;
      for (k=0;k<mris->nvertices;k++)
        {
          v = &mris->vertices[k];
          v->tx = v->x;
          v->ty = v->y;
          v->tz = v->z;
        }

      for (k=0;k<mris->nvertices;k++)
        {
          v = &mris->vertices[k];
          x = v->tx;
          y = v->ty;
          z = v->tz;
          nx = v->nx;
          ny = v->ny;
          nz = v->nz;
          sx=sy=sz=sd=0;
          n=0;
          for (m=0;m<v->vnum;m++)
            {
              sx += dx =mris->vertices[v->v[m]].tx - x;
              sy += dy =mris->vertices[v->v[m]].ty - y;
              sz += dz =mris->vertices[v->v[m]].tz - z;
              sd += sqrt(dx*dx+dy*dy+dz*dz);
              n++;
            }
          // S is the vector points to the mean neighbor point
          sx = sx/n;
          sy = sy/n;
          sz = sz/n;
          // mean distance 
          sd = sd/n;
      
          lm+=sd;

          nc = sx*nx+sy*ny+sz*nz;
      
          // normal component
          sxn = nc*nx;
          syn = nc*ny;
          szn = nc*nz;
          // tangential component
          sxt=sx-sxn;
          syt=sy-syn;
          szt=sz-szn;

          v->nc=nc;

          ///////////////////////////////////////////////////
          // force determination
          force1=0.3;

          f1m+=force1;


          /******************************/

          find_normal(nx,ny,nz,n1,n2,MRI_var->direction);
          for (h=0;h<4;h++)
            for (a=-1;a<2;a++)
              for (b=-1;b<2;b++)
                {
                  // get the RAS value
                  myWorldToVoxel(MRI_var->mri_orig,(x-nx*h+n1[0]*a+n2[0]*b),
                                 (y-ny*h+n1[1]*a+n2[1]*b),
                                 (z-nz*h+n1[2]*a+n2[2]*b),&tx,&ty,&tz);
                  kt=(int)(tz+0.5);
                  jt=(int)(ty+0.5);
                  it=(int)(tx+0.5);
            
                  if ((kt<0||kt>=MRI_var->depth||
                       it<0||it>=MRI_var->width||
                       jt<0||jt>=MRI_var->height))
                    val=0;
                  else
                    val=MRIvox(MRI_var->mri_orig,it,jt,kt);
            
                  test_samp[h][3*b+a+4] = val;
                }
      
          val=test_samp[0][4];
      
          if (!val)  
            force=-0.25;
          else if (val<=fzero/2)
            force=-0.1;
          else 
            {        
              mean(test_samp,samp_mean);
        
              if (samp_mean[0]<fzero && samp_mean[1]<fzero)
                force=-0.2;
              else
                {
                  // ??????????????
                  nb_GM=0;
                  nb_TR=0;
                  nb_GTM=0;
                  for (h=0;h<4;h++)
                    {
                      if (samp_mean[h]>=MRI_var->TRANSITION_intensity)
                        nb_GM++;
                      if (samp_mean[h]<fzero)
                        nb_TR++;
                    }
          
                  if (nb_TR>=3)
                    force=-0.2;
                  else if (nb_GM>=3 && 
                           samp_mean[0]>MRI_var->TRANSITION_intensity)
                    force=0.7;
                  else if (nb_GM==2 && 
                           samp_mean[0]>MRI_var->TRANSITION_intensity)
                    force=0.5; 
                  else if (nb_TR==0)
                    force=0.3;
                  else
                    {
                      nb_GM=0;
                      nb_TR=0;
                      for (h=0;h<4;h++)
                        {
                          for (a=0;a<9;a++)
                            {
                              if (test_samp[h][a]>=
                                  MRI_var->TRANSITION_intensity)
                                nb_GM++;
                              else if (test_samp[h][a]<fzero)
                                nb_TR++;
                              else
                                nb_GTM++;
                            }
                        }
            
                      if (nb_TR>=25)
                        force=-0.3;
                      else if (nb_GM>=18)
                        force=0.5;
                      else if (nb_GM>=15)
                        force=0.3;
                      else
                        {
                          if (nb_GM>9 && nb_TR<9)
                            force=0.5;
                          else if (nb_GTM>30)
                            force=0.1;
                          else
                            force=-0.0;
                        }
                    }
                }
            }
      
          f2m+=force;

          force1=0.5;

          // Delta = .8 x St + force1 x Sn + force X Vn
          ///////////////////////////////////////////////
          dx = sxt*0.8 + force1*sxn+v->nx*force;
          dy = syt*0.8 + force1*syn+v->ny*force;
          dz = szt*0.8 + force1*szn+v->nz*force;

          // modify Detal
          ///////////////////////////////////////////////
          dx = decay*v->odx+update*dx;
          dy = decay*v->ody+update*dy;
          dz = decay*v->odz+update*dz;
      
          // if too much, make Detal < 1
          if ((d=sqrt(dx*dx+dy*dy+dz*dz))>1.0)
            {
              dx /= d;
              dy /= d;
              dz /= d;
            }
      
          // cache the Delta
          v->odx = dx;
          v->ody = dy;
          v->odz = dz;


          d=sqrt(dx*dx+dy*dy+dz*dz);
      
          dm+=d;

          dist[k][iter%4][0]=x;
          dist[k][iter%4][1]=y;
          dist[k][iter%4][2]=z;

          d10m[0] = d10m[1] = d10m[2] = 0;

          for(n=0;n<4;n++)
            {
              d10m[0]+=dist[k][n][0]/4;
              d10m[1]+=dist[k][n][1]/4;
              d10m[2]+=dist[k][n][2]/4;
            }
    
          dbuff=0;
          for(n=0;n<4;n++)
            dbuff+=SQR(dist[k][n][0]-d10m[0])+SQR(dist[k][n][1]-d10m[1])+
              SQR(dist[k][n][2]-d10m[2]);

          d10+=dbuff/4;

          // move the position
          v->x += dx;
          v->y += dy;
          v->z += dz;
        }

      lm /=mris->nvertices;
      f1m /=mris->nvertices;
      f2m /=mris->nvertices;
      dm /=mris->nvertices;
      d10 /=mris->nvertices;

      mean_sd[iter%10]=lm;
      mean_dist[iter%10]=d10;

      coutbuff=0;
      for(n=0;n<10;n++)
        coutbuff+=mean_sd[n]/10;
    
      varbuff=0;
      for(n=0;n<10;n++)
        varbuff+=SQR(mean_sd[n]-coutbuff);

      cout=varbuff;

      coutbuff=0;
      for(n=0;n<10;n++)
        coutbuff+=mean_dist[n]/10;

      varbuff=0;
      for(n=0;n<10;n++)
        varbuff+=SQR(mean_dist[n]-coutbuff);

      cout+=10*varbuff;

      coutbuff=cout;

      cout=(cout+pcout)/2;

      pcout=coutbuff;

      MRIScomputeNormals(mris);

      /*if ((niter==int_smooth) && !(iter % 5))
        fprintf(stderr,
        "%d: lm=%5.3f,f1m=%5.3f,f2m=%5.3f,dm=%5.3f,d10m=%5.3f,c=%5.3f\n"
        ,iter,lm,f1m,f2m,dm,d10,100*cout);*/

      if (niter==int_smooth)
        {
          if(((iter>20)&&(10000*cout<1))||(iter>200))
            niter--;
        }
      else
        niter--;
    }
  fprintf(stderr,"%d iterations",iter);
  MRIScomputeNormals(mris);

  /*free memory*/
  for( it = 0; it < mris->nvertices; it++ ) 
    {
      for( jt = 0; jt < 4; jt++ ) 
        free(dist[it][jt]);   
      free(dist[it]);
    }
  free(dist);
}

static void label_voxels(STRIP_PARMS *parms, 
                         MRI_variables *MRI_var,
                         MRI *mri_with_skull)
{
  char fname[512];
  int i,j,k;
  int A,B;
  unsigned long  volume_skull;
  double vol_elt;

  A=(2*MRI_var->CSF_MAX+MRI_var->TRANSITION_intensity)/3;
  B=(MRI_var->GM_intensity+MRI_var->WM_MIN)/2;
    
  fprintf(stderr,"\n      tissue label process");

  for(i=0;i<MRI_var->width;i++) 
    for(j=0;j<MRI_var->height;j++)
      for(k=0;k<MRI_var->depth;k++)
        MRIvox(MRI_var->mri_src,i,j,k)=0;

  /*loading the outer skin surface*/
  strcpy(fname,parms->surfname);
  strcat(fname,"_outer_skin_surface");
  read_geometry(2,MRI_var,fname);
  MRISpeelBrain(-1,MRI_var->mri_src,MRI_var->mris,1);

  /*loading the outer outer skull surface*/
  strcpy(fname,parms->surfname);
  strcat(fname,"_outer_skull_surface");
  read_geometry(2,MRI_var,fname);
  volume_skull=MRISpeelBrain(-1,MRI_var->mri_src,MRI_var->mris,2);
  volume_skull=MRI_var->depth*MRI_var->width*MRI_var->height-volume_skull;
  vol_elt=
    MRI_var->mri_src->xsize*MRI_var->mri_src->ysize*MRI_var->mri_src->zsize;

  fprintf(stderr,"\n\nSkull Size = %ld voxels, voxel volume = %2.3f mm3\n"
          ,volume_skull,(float)vol_elt);
  fprintf(stderr,"           = %2.0f mmm3 = %2.3f cm3\n"
          ,volume_skull*vol_elt,(float)volume_skull/1000.*vol_elt);
         

  /*loading the inner skull surface*/
  strcpy(fname,parms->surfname);
  strcat(fname,"_inner_skull_surface");
  read_geometry(2,MRI_var,fname);
  MRISpeelBrain(-1,MRI_var->mri_src,MRI_var->mris,3);

  /*loading the brain surface*/
  strcpy(fname,parms->surfname);
  strcat(fname,"_brain_surface");
  read_geometry(2,MRI_var,fname);
  MRISpeelBrain(-1,MRI_var->mri_src,MRI_var->mris,4);

  for(i=0;i<MRI_var->width;i++) 
    for(j=0;j<MRI_var->height;j++)
      for(k=0;k<MRI_var->depth;k++)
        if(MRIvox(MRI_var->mri_src,i,j,k)==4)
          {
            if(MRIvox(mri_with_skull,i,j,k)<A)
              MRIvox(MRI_var->mri_src,i,j,k)=3;
            else if(MRIvox(mri_with_skull,i,j,k)>B)
              MRIvox(MRI_var->mri_src,i,j,k)=5;
          }
        else if (MRIvox(MRI_var->mri_src,i,j,k)==2)
          {
            if(MRIvox(mri_with_skull,i,j,k)>B)
              MRIvox(MRI_var->mri_src,i,j,k)=6;
          };
  
}

#if 0 
static void
normal_face(int f,float *n,MRIS *mris)
{
  float v1[3],v2[3];

  v1[0]=
   mris->vertices[mris->faces[f].v[0]].x-mris->vertices[mris->faces[f].v[1]].x;
  v1[1] =
   mris->vertices[mris->faces[f].v[0]].y-mris->vertices[mris->faces[f].v[1]].y;
  v1[2] =
   mris->vertices[mris->faces[f].v[0]].z-mris->vertices[mris->faces[f].v[1]].z;
  v2[0] =
   mris->vertices[mris->faces[f].v[2]].x-mris->vertices[mris->faces[f].v[1]].x;
  v2[1] =
   mris->vertices[mris->faces[f].v[2]].y-mris->vertices[mris->faces[f].v[1]].y;
  v2[2] =
   mris->vertices[mris->faces[f].v[2]].z-mris->vertices[mris->faces[f].v[1]].z;
  n[0] = v1[1]*v2[2]-v1[2]*v2[1];
  n[1] = v1[2]*v2[0]-v1[0]*v2[2];
  n[2] = v1[0]*v2[1]-v1[1]*v2[0];
}

static int
mrisNormalFace(MRIS *mris, int fac,int n,float norm[])
{
  int     n0,n1, *pv ;
  FACE    *f;
  float   v0[3],v1[3];
  register VERTEX  *v, *vn0, *vn1 ;

  n0 = (n == 0)                   ? VERTICES_PER_FACE-1 : n-1;
  n1 = (n == VERTICES_PER_FACE-1) ? 0                   : n+1;
  f = &mris->faces[fac];
  pv = f->v ;
  vn0 = &mris->vertices[pv[n0]] ;
  vn1 = &mris->vertices[pv[n1]] ;
  v =  &mris->vertices[pv[n]] ;
  v0[0] = v->x - vn0->x; v0[1] = v->y - vn0->y; v0[2] = v->z - vn0->z;
  v1[0] = vn1->x - v->x; v1[1] = vn1->y - v->y; v1[2] = vn1->z - v->z;

  norm[0] = -v1[1]*v0[2] + v0[1]*v1[2];
  norm[1] = v1[0]*v0[2] - v0[0]*v1[2];
  norm[2] = -v1[0]*v0[1] + v0[0]*v1[1];

  return(NO_ERROR) ;
}

static void mrisComputeNormals(MRIS *mris)
{
  int j,k;
  VERTEX *v;
  float n[3],nt[3],d;

  
  for (k=0;k<mris->nvertices;k++)
    {
      v = &mris->vertices[k];
      n[0] = n[1] = n[2] = 0;
      for (j=0;j<v->num;j++)
        {
          mrisNormalFace(mris, v->f[j], (int)v->n[j],n);
          n[0] += nt[0];
          n[1] += nt[1];
          n[2] += nt[2];
        }
      d = sqrt(SQR(n[0])+SQR(n[1])+SQR(n[2]));

      if(d==0)
        Error("\n normal = zero !");

      v->tdx = n[0]/d;
      v->tdy = n[1]/d;
      v->tdz = n[2]/d;
    }
  for (k=0;k<mris->nvertices;k++)
    {
      v = &mris->vertices[k];
      v->nx=v->tdx;
      v->ny=v->tdy;
      v->nz=v->tdz;
    }
}

static float
rtanh(float x)
{
  return (x<0.0)?0.0:tanh(x);
}
#endif

/************************************************************************
 ***********************************************************************
 ************************************************************************/

/*-----------------------------------------------------
  FUNCTION ValidationSurfaceShape
  
  Parameters:
  MRI_variables *: contains the variables 

  Returns value:void

  Description: Validates the shape of the surface 
  and eventually corrects it 
  ------------------------------------------------------*/
static int ValidationSurfaceShape(MRI_variables *MRI_var)
{
  double init_sse,var_init_sse,rot_sse,var_rot_sse;
  char surf_fname[500],*mri_dir;
  MRI_SP *mrisp_template;
  MRIS *mrisphere,*mris_curv,*mris_dCOG;
  INTEGRATION_PARMS parms ;
  int mode=DEFAULT_MODE;
  int validation;

  validation=0;
  /*free the surfaces if non NULL*/
  // remove mrisphere, mris_curv, mris_var_curv, mris_dCOG, mris_var_dCOG
  if(MRI_var->mrisphere)
    {
      MRISfree(&MRI_var->mrisphere);
      MRI_var->mrisphere=NULL;
    }
  if(MRI_var->mris_curv)
    {
      MRISfree(&MRI_var->mris_curv);
      MRI_var->mris_curv=NULL;
    } 
  if(MRI_var->mris_var_curv)
    {  
      MRISfree(&MRI_var->mris_var_curv);
      MRI_var->mris_var_curv=NULL;
    }
  if(MRI_var->mris_dCOG)
    {  
      MRISfree(&MRI_var->mris_dCOG);
      MRI_var->mris_dCOG=NULL;
    }
  if(MRI_var->mris_var_dCOG)
    {
      MRISfree(&MRI_var->mris_var_dCOG);
      MRI_var->mris_var_dCOG=NULL;
    }
  // read in icosahedron data (highly tessellated one)
  mri_dir = getenv("FREESURFER_HOME");
  sprintf(surf_fname,"%s/lib/bem/ic5.tri",mri_dir);
  mrisphere = MRISread(surf_fname) ;
  if (!mrisphere)
    ErrorExit(ERROR_NOFILE, "%s: could not open surface file %s",
              Progname, surf_fname) ;

  //calculating sphere radius and scaling it to 100mm
  MRIScenterCOG(mrisphere);
  mrisphere->radius=MRISradius(mrisphere); 
  MRISscale(mrisphere); // make radius 100. 

  // now mrisphere is the highly tessellated icosahedron
  MRI_var->mrisphere=mrisphere;

  // copy the current surface 
  mris_curv=MRISclone(MRI_var->mris);
  mris_dCOG=MRISclone(MRI_var->mris);

  // calculate curvatures etc.  (defined in this file)
  // mris_curv has curvature info, mris_dCOG has dCOG info in curv member
  // note that vertex positions are from mrisphere 
  MRISinitSurfaces(mris_curv,mris_dCOG,MRI_var->mrisphere);
  
  // smoothing the surfaces for 10 times
  MRISsmooth_surface(MRI_var->mris,ITER_SMOOTH);

  // reading template brain
  sprintf(surf_fname, "%s/average/rigidly_aligned_brain_template.tif",
          mri_dir) ;
  //sprintf(surf_fname, "./rigidly_aligned_brain_template.tif") ;
  mrisp_template=MRISPread(surf_fname);
  if (!mrisp_template)
    ErrorExit(ERROR_NOFILE, "Could not open the file %s\n",surf_fname) ;

  parms.frame_no = 0 ;
  //  parms.mrisp = MRIStoParameterization(mris_curv, NULL, scale, 0) ;
  //if (!parms.mrisp)
  //  ErrorExit(ERROR_NOFILE, "could not parametrize the surface") ;
  parms.mrisp_template = mrisp_template ;
  parms.l_corr = 1.0f ;  

  //calcul of the initial sse
  init_sse=0;
  parms.frame_no = 0 ;
  init_sse=mrisComputeCorrelationError(mris_curv,&parms,1);
  var_init_sse=parms.momentum;
  parms.frame_no = 3 ;
  init_sse+=mrisComputeCorrelationError(mris_dCOG,&parms,1);
  var_init_sse+=parms.momentum;
  parms.frame_no = 0 ;
  init_sse/=2.;
  var_init_sse/=2.;

  //Rigid body Alignment
  if(MRI_var->atlas)
    {
      fprintf(stderr,"\nRigid alignment...");
      mrisRigidBodyAlignGlobal(mris_curv, mris_dCOG, 
                               &parms, mode, 4.0, 32.0, 8) ;
      fprintf(stderr,"\ndone");
    }
  else
    fprintf(stderr,"\nNo Rigid alignment: Atlas Mode Off");
  
  //clone the rotated surfaces and the corresponding parameterizations
  MRI_var->mris_curv     =MRISclone(mris_curv);
  MRI_var->mris_var_curv =MRISclone(mris_curv);
  MRI_var->mris_dCOG     =MRISclone(mris_dCOG); 
  MRI_var->mris_var_dCOG =MRISclone(mris_dCOG);

  // modify the values using template 
  // (MRI_var contains the ones from the template)
  MRI_var->mris_curv     =
    MRISfromParameterization(mrisp_template, MRI_var->mris_curv, 0);  
  MRI_var->mris_var_curv =
    MRISfromParameterization(mrisp_template, MRI_var->mris_var_curv, 1);  
  MRI_var->mris_dCOG     =
    MRISfromParameterization(mrisp_template, MRI_var->mris_dCOG, 3);  
  MRI_var->mris_var_dCOG =
    MRISfromParameterization(mrisp_template, MRI_var->mris_var_dCOG, 4);  

  //Inverse the Rotation for the template surfaces
  // using mrisphere vertices to modify vertex positions
  MRISchangeCoordinates(MRI_var->mris_curv,     mrisphere);
  MRISchangeCoordinates(MRI_var->mris_var_curv, mrisphere);
  MRISchangeCoordinates(MRI_var->mris_dCOG,     mrisphere);
  MRISchangeCoordinates(MRI_var->mris_var_dCOG, mrisphere);

  //calcul of the rotated sse
  rot_sse=0;
  parms.frame_no = 0 ;
  rot_sse=mrisComputeCorrelationError(mris_curv,&parms,1);
  var_rot_sse=parms.momentum;
  parms.frame_no = 3 ;  
  rot_sse+=mrisComputeCorrelationError(mris_dCOG,&parms,1);
  var_rot_sse+=parms.momentum;
  parms.frame_no = 0 ;
  rot_sse/=2.;
  var_rot_sse/=2.;

  fprintf(stderr,"\n      before rotation: sse = %2.2f, sigma = %2.2f"
          "\n      after  rotation: sse = %2.2f, sigma = %2.2f"
          , init_sse,sqrt(var_init_sse),rot_sse,sqrt(var_rot_sse));

  //Validation !!!!
  validation=mrisLocalizeErrors(mris_curv,mris_dCOG,MRI_var,mrisphere) ;

  if(validation==0)
    {
      fprintf(stderr,"\n\n      THE SEGMENTATION IS PROBABLY NOT CORRECT\n");
      if(!MRI_var->atlas)
        {
          fprintf(stderr, "**********************************************\n");
          fprintf(stderr, "**********************************************\n");
          fprintf(stderr,"      If the final segmentation is not valid,"
                  "\n      try using the option '-atlas'\n");
          fprintf(stderr, "**********************************************\n");
          fprintf(stderr, "**********************************************\n");
        }
    }
  else
    fprintf(stderr,"\n      Validation of the shape of the surface done.");
  

  
  MRISPfree(&mrisp_template);
  //  MRISPfree(&parms.mrisp) ;
 
  MRISfree(&mris_dCOG);
  MRISfree(&mris_curv);
  
  return validation;
}


static void MRIScenterCOG(MRI_SURFACE *mris)
{
  int k;
  double x,y,z;
  x=0;
  y=0;
  z=0;
  for(k=0;k<mris->nvertices;k++)
    {
      x+=mris->vertices[k].x;
      y+=mris->vertices[k].y;
      z+=mris->vertices[k].z;
    }
  x/=mris->nvertices;
  y/=mris->nvertices;
  z/=mris->nvertices;
  for(k=0;k<mris->nvertices;k++)
    {
      mris->vertices[k].x-=x;
      mris->vertices[k].y-=y;
      mris->vertices[k].z-=z;
    }
  /*       fprintf(stderr,"\nCOG Centered at x=%f y=%f z=%f",
           (float)x,(float)y,(float)z);*/
}

void MRISscale(MRI_SURFACE *mris)
{
  int k;
  double r;
  r=100./mris->radius;
  for(k=0;k<mris->nvertices;k++)
    {
      mris->vertices[k].x=mris->vertices[k].x*r;
      mris->vertices[k].y=mris->vertices[k].y*r;
      mris->vertices[k].z=mris->vertices[k].z*r;
    }
  mris->radius=100;
}

double MRISradius(MRI_SURFACE *mris)
{
  int k;
  double r;
  r=0;
  for(k=0;k<mris->nvertices;k++)
    {
      r+=mris->vertices[k].x*mris->vertices[k].x;
      r+=mris->vertices[k].y*mris->vertices[k].y;
      r+=mris->vertices[k].z*mris->vertices[k].z;
    }
  return(sqrt(r/(double)mris->nvertices));
}

static void MRISinitSurfaces(MRIS *mris_curv,
                             MRIS *mris_dCOG, 
                             const MRIS *mrisphere)
{
  int iter_smooth;
  int navgs,nbrs;

  iter_smooth=ITER_SMOOTH;
  navgs=NBR_AVGS;
  nbrs=NBR_NGBS;

  //Intialize curvature field Surface
  MRISsmooth_surface(mris_curv,iter_smooth);
  //
  MRISsaveVertexPositions(mris_curv, ORIGINAL_VERTICES) ;
  MRISsetNeighborhoodSize(mris_curv, nbrs) ;
  MRIScomputeMetricProperties(mris_curv) ;
  //
  MRIScomputeSecondFundamentalForm(mris_curv) ;
  MRISuseMeanCurvature(mris_curv) ;
  MRISaverageCurvatures(mris_curv, navgs) ;
  MRISnormalizeCurvature(mris_curv) ;
  //Initialize distance to COG surface
  MRISchangeCoordinates(mris_dCOG, mris_curv); 
  // use the mris_curv vertex positions (smoothed values)
  //
  MRISsaveVertexPositions(mris_dCOG, ORIGINAL_VERTICES) ;
  MRISsetNeighborhoodSize(mris_dCOG, nbrs) ;
  MRIScomputeMetricProperties(mris_dCOG) ;
  //
  MRISdistanceToCOG(mris_dCOG);               
  // store dCOG as vetex.curv member
  MRISaverageCurvatures(mris_dCOG, navgs) ;   
  // so average curvature is actually the average dCOG
  MRISnormalizeCurvature(mris_dCOG) ;         
  // move the value to mean and divide by std
  // change coordinates to sphere coord
  // even though the curvatures, the dCOG are from 
  // smoothed surface, the vertex positions
  // are those of the sphere
  MRISchangeCoordinates(mris_curv,mrisphere);
  MRISchangeCoordinates(mris_dCOG,mrisphere);
}

static void MRISdistanceToCOG(MRI_SURFACE *mris)
{
  int k;
  double r;
  double x,y,z;
  x=0;
  y=0;
  z=0;
  for(k=0;k<mris->nvertices;k++)
    {
      x+=mris->vertices[k].x;
      y+=mris->vertices[k].y;
      z+=mris->vertices[k].z;
    }
  // center of gravity
  x/=mris->nvertices;
  y/=mris->nvertices;
  z/=mris->nvertices;
  // 
  for(k=0;k<mris->nvertices;k++)
    {
      r=0;
      r+=SQR(mris->vertices[k].x-x);
      r+=SQR(mris->vertices[k].y-y);
      r+=SQR(mris->vertices[k].z-z);
      // add all the distances 
      // set curvature to be 
      mris->vertices[k].curv=sqrt(r);
    }
}

#define CORR_THRESHOLD 5.3f

static double
mrisComputeCorrelationError(MRI_SURFACE *mris, INTEGRATION_PARMS *parms,
                            int use_stds)
{
  double   src, target, sse, var_sse,delta, std ;
  VERTEX   *v ;
  int      vno ,count;
  float    x, y, z, l_corr ;

  l_corr = parms->l_corr + parms->l_pcorr ;  /* only one will be nonzero */
  if (FZERO(l_corr))
    return(0.0) ;



  for (sse = 0.0f, var_sse=0.0f, count=0 , vno = 0 ; 
       vno < mris->nvertices ; 
       vno++)
    {
      v = &mris->vertices[vno] ;

      x = v->x ; y = v->y ; z = v->z ;

      src = v->curv ;

      target = 
        MRISPfunctionVal(parms->mrisp_template, mris, x, y, z,
                         parms->frame_no) ;

      std = MRISPfunctionVal(parms->mrisp_template,mris,x,y,z,
                             parms->frame_no+1);
      std = sqrt(std) ;

#define DEFAULT_STD  4.0f

      if (FZERO(std))
        std = DEFAULT_STD /*FSMALL*/ ;
      if (!use_stds)
        std = 1.0f ;

      delta = (src - target) / std ;
      if (!finite(delta))
        continue;

      if(delta>CORR_THRESHOLD)
        continue;

      delta= delta * delta;
      sse += delta ;
      var_sse += delta * delta ;
      count++;
    }
  if(count)
    {
      sse/=(double)count;
      var_sse=var_sse/(double)count-sse*sse;
      /*use the momentum field of parms to save the var of the see*/
      parms->momentum=var_sse;
      return (sse);
    }
  else
    return(1000.0) ;
}

// use orig to change the vertices position and radius
void MRISchangeCoordinates(MRI_SURFACE *mris, const MRI_SURFACE *mris_orig)
{
  int p;
  for(p=0;p<mris->nvertices;p++)
    {
      mris->vertices[p].x=mris_orig->vertices[p].x;
      mris->vertices[p].y=mris_orig->vertices[p].y;
      mris->vertices[p].z=mris_orig->vertices[p].z;
    }
  mris->radius=mris_orig->radius; 
  mris->status=mris_orig->status;
}


static int
mrisRigidBodyAlignGlobal(MRIS *mris_curv, 
                         MRIS *mris_dist,
                         INTEGRATION_PARMS *parms,
                         int mode,
                         float min_degrees, 
                         float max_degrees, 
                         int nangles)
{
  double   alpha, beta, gamma, degrees, delta, mina, minb, ming,
    sse, min_sse ;
  int      curv_old_status = mris_curv->status
    ,dist_old_status=mris_dist->status ;

  //to stop the compilator warnings !
  min_sse=0;
  sse=0;

  min_degrees = RADIANS(min_degrees) ; max_degrees = RADIANS(max_degrees) ;
  mris_curv->status = MRIS_RIGID_BODY ; 
  mris_dist->status = MRIS_RIGID_BODY ; 

  for (degrees = max_degrees ; degrees >= min_degrees ; degrees /= 2.0f)
    {
      mina = minb = ming = 0.0 ;
      switch(mode)
        {
        case DIST_MODE:
          parms->frame_no = 3 ;
          min_sse = mrisComputeCorrelationError(mris_dist, parms, 1) ; 
          break;
        case CURV_MODE:
          parms->frame_no = 0 ;
          min_sse = mrisComputeCorrelationError(mris_curv, parms, 1) ; 
          break;
        case DEFAULT_MODE:
          parms->frame_no = 0 ;
          min_sse = mrisComputeCorrelationError(mris_curv, parms, 1) ; 
          parms->frame_no = 3 ;
          min_sse += mrisComputeCorrelationError(mris_dist, parms, 1) ;
          min_sse/=2.;
          break;
        }

      delta = 2*degrees / (float)nangles ;

      for (alpha = -degrees ; alpha <= degrees ; alpha += delta)
        {
          for (beta = -degrees ; beta <= degrees ; beta += delta)
            {
#if 0
              if (Gdiag & DIAG_SHOW)
                fprintf(stdout, "\r(%+2.2f, %+2.2f, %+2.2f), "
                        "min @ (%2.2f, %2.2f, %2.2f) = %5.1f   ",
                        (float)DEGREES(alpha), (float)DEGREES(beta), (float)
                        DEGREES(-degrees), (float)DEGREES(mina), 
                        (float)DEGREES(minb), 
                        (float)DEGREES(ming),(float)min_sse);
#endif
              for (gamma = -degrees ; gamma <= degrees ; gamma += delta)
                {
                  switch(mode)
                    {
                    case DIST_MODE:
                      MRISsaveVertexPositions(mris_dist, TMP_VERTICES) ;
                      MRISrotate(mris_dist, mris_dist, alpha, beta, gamma) ;
                      parms->frame_no = 3 ;
                      sse = mrisComputeCorrelationError(mris_dist, parms, 1) ;
                      MRISrestoreVertexPositions(mris_dist, TMP_VERTICES) ;
                      break;
                    case CURV_MODE:
                      MRISsaveVertexPositions(mris_curv, TMP_VERTICES) ;
                      MRISrotate(mris_curv, mris_curv, alpha, beta, gamma) ;
                      parms->frame_no = 0 ;
                      sse = mrisComputeCorrelationError(mris_curv, parms, 1) ; 
                      MRISrestoreVertexPositions(mris_curv, TMP_VERTICES) ;
                      break;
                    case DEFAULT_MODE:
                      MRISsaveVertexPositions(mris_curv, TMP_VERTICES) ;
                      MRISrotate(mris_curv, mris_curv, alpha, beta, gamma) ;
                      parms->frame_no = 0 ;
                      sse = mrisComputeCorrelationError(mris_curv, parms, 1) ;
            
                      MRISrestoreVertexPositions(mris_curv, TMP_VERTICES) ;
            
                      MRISsaveVertexPositions(mris_dist, TMP_VERTICES) ;
                      MRISrotate(mris_dist, mris_dist, alpha, beta, gamma) ;
                      parms->frame_no = 3 ;
                      sse += 
                        mrisComputeCorrelationError(mris_dist, parms, 1) ; 
            
                      MRISrestoreVertexPositions(mris_dist, TMP_VERTICES) ;
                      sse/=2.;
                      break;
                    }
          
                  if (sse < min_sse)
                    {
                      mina = alpha ; minb = beta ; ming = gamma ;
                      min_sse = sse ;
                    }
#if 0
                  if (Gdiag & DIAG_SHOW)
                    fprintf(stdout, "\r(%+2.2f, %+2.2f, %+2.2f), "
                            "min @ (%2.2f, %2.2f, %2.2f) = %5.1f  sse = %5.1f",
                            (float)DEGREES(alpha), 
                            (float)DEGREES(beta), 
                            (float)DEGREES(gamma), 
                            (float)DEGREES(mina), 
                            (float)DEGREES(minb), 
                            (float)DEGREES(ming),
                            (float)min_sse,
                            (float)sse);
#endif
          
                }
            }
        }
      // fprintf(stderr,".");
    
#if 1
      fprintf(stderr, "\n      scanning %5.2f degree nbhd, "
              "min sse = %5.2f at (%5.2f, %5.2f, %5.2f)",
              (float)DEGREES(degrees),
              min_sse, (float)DEGREES(mina), (float)DEGREES(minb), 
              (float)DEGREES(ming)) ;
#endif

      if (!FZERO(mina) || !FZERO(minb) || !FZERO(ming))
        {
          MRISrotate(mris_curv, mris_curv, mina, minb, ming) ;
          MRISrotate(mris_dist, mris_dist, mina, minb, ming) ;
          parms->frame_no = 0 ;
          sse = mrisComputeCorrelationError(mris_curv, parms, 1) ; 
          parms->frame_no = 3 ;
          sse += mrisComputeCorrelationError(mris_dist, parms, 1) ; 
          sse/=2;
        }
    }

  mris_curv->status = curv_old_status ; 
  mris_dist->status = dist_old_status ; 

  return(NO_ERROR) ;
}

#define ERROR_THRESHOLD 16.0f

static int mrisLocalizeErrors(MRIS* mris_curv,MRIS *mris_dCOG,
                              MRI_variables *MRI_var,MRIS *mrisphere)
{
  double sse,mean_sse,var_sse;
  int k;
  int nbWrongVertices1,nbWrongVertices2;
  int nvertices=mris_curv->nvertices;

  float validation_percentage;
  float wgpospercentage,wgnegpercentage;

#if WRITE_SURFACES
  char fname[500];
#endif

  fprintf(stderr,"\nLocalization of inacurate regions: "
          "Erosion-Dilation steps");

  nbWrongVertices1=0;
  // go through all vertices
  for(mean_sse=0 , var_sse=0 , k=0; k<nvertices; k++)
    {
      sse=SQR(mris_curv->vertices[k].curv - 
              MRI_var->mris_curv->vertices[k].curv)
        /MRI_var->mris_var_curv->vertices[k].curv;

      sse += SQR(mris_dCOG->vertices[k].curv - 
                 MRI_var->mris_dCOG->vertices[k].curv)
        /MRI_var->mris_var_dCOG->vertices[k].curv; // actually dCOG value

      sse/=2.;
      mean_sse+=sse;
      var_sse+=SQR(sse);
      // mark sphere where the sse > threshold
      if(sse<ERROR_THRESHOLD)
        mrisphere->vertices[k].val=0.;
      else 
        {
          nbWrongVertices1++; 
          mrisphere->vertices[k].val=1.;
        }
      mrisphere->vertices[k].curv=sse;//tanh(sse/ERROR_THRESHOLD);
    }

#if WRITE_SURFACES
  sprintf(fname,"./rh.error");
  MRISwriteCurvature(mrisphere,fname);
#endif

  mean_sse/=nvertices;
  var_sse=var_sse/nvertices-SQR(mean_sse);
 
  MRISsetNeighborhoodSize(mrisphere, 1) ;
  //Erosion step
  MRISaverageVals(mrisphere,3);   
  // change mrisphere with neighboring averaged grey scale (iterated 3 times)
  // less than 1, then 0. (erosion step introduce interpolation)
  for(k=0;k<nvertices;k++)
    if(mrisphere->vertices[k].val<1.)
      mrisphere->vertices[k].val=0.;

  //Dilatation Step
  nbWrongVertices2=0;
  MRISaverageVals(mrisphere,5);   
  // change with neighboring averaged grey scale (iterated 5 times)
  for(k=0;k<nvertices;k++)
    if(mrisphere->vertices[k].val>0.0 
       && mrisphere->vertices[k].curv>ERROR_THRESHOLD) 
      {
        mrisphere->vertices[k].val=1.;
      }
    else
      mrisphere->vertices[k].val=0.;

  MRISaverageVals(mrisphere,3);   // another average iterated 3 times

  wgpospercentage=wgnegpercentage=0; 
  for(k=0;k<nvertices;k++)
    {    
      if(mrisphere->vertices[k].val>0.0)
        {
          nbWrongVertices2++;
          mrisphere->vertices[k].val=1.;
          // using dCOG: curv contains the distance from the COG
          if((mris_dCOG->vertices[k].curv - 
              MRI_var->mris_dCOG->vertices[k].curv)>0)
            wgpospercentage+=1.; // the current surface is larger
          else
            wgnegpercentage+=1.; // the current surfeace is smaller
        }
    }
  
  validation_percentage=100.*(float)nbWrongVertices2/mrisphere->nvertices;

  fprintf(stderr,"\n      the sse mean is %5.2f, its var is %5.2f   "
          "\n      before Erosion-Dilatation %5.2f%% of inacurate vertices"
          "\n      after  Erosion-Dilatation %5.2f%% of inacurate vertices"
          ,mean_sse,sqrt(var_sse),
          100.*(float)nbWrongVertices1/mrisphere->nvertices,
          validation_percentage);

  if(validation_percentage>1.) 
    /*(mean_sse>2) || (mean_sse>1 && sqrt(var_sse)>3))*/
    {
      wgnegpercentage=100.*wgnegpercentage/(float)nbWrongVertices2;
      wgpospercentage=100.*wgpospercentage/(float)nbWrongVertices2;
      fprintf(stderr,"\n            %5.2f%% of 'positive' inacurate vertices"
              "\n            %5.2f%% of 'negative' inacurate vertices"
              ,wgpospercentage,wgnegpercentage);
      return 0;
    }
  else
    return 1;
}


/*-----------------------------------------------------
  FUNCTION MRISscaleFields
  
  Parameters:
  MRIS * mris_src : source surface
  MRIS * mris_curv: the curv field surface 
  MRIS *mris_dCOG : the dCOG field surface
  int whichfield : CURV_MODE or DIST_MODE
  Returns value:void

  Description: Scale the curv and dCOG surfaces to the correct mean 
  and variance of the current mris surface
  ------------------------------------------------------*/
#define THRESHOLD 0.01
static void MRISscaleFields(MRIS *mris_src,MRIS *mris_fdst,
                            MRIS *mris_vdst,int whichfield)
{
  int k;
  double tp;
  double field,sigma,var;
  double oldfield, oldsigma,oldvar;
  MRIS *mris1,*mris2;

  int navgs,nbrs;
  double sse,stop;
  double count;
  int iter;

  nbrs=2;
  navgs=20;


  if(whichfield==CURV_MODE)
    {
      if(VERBOSE_MODE)
        fprintf(stderr,"\n      scaling Curvature Field      ");
      MRISrestoreVertexPositions(mris_src, ORIGINAL_VERTICES) ;
      MRISsetNeighborhoodSize(mris_src, nbrs) ;
      MRIScomputeMetricProperties(mris_src) ;
      MRIScomputeSecondFundamentalForm(mris_src) ;
      MRISuseMeanCurvature(mris_src) ;
      MRISaverageCurvatures(mris_src, navgs) ;
    }
  else
    {
      if(VERBOSE_MODE)
        fprintf(stderr,"\n      scaling 'Distance to Centroid' Field      ");
      //Initialize distance to COG surface
      MRISrestoreVertexPositions(mris_src, ORIGINAL_VERTICES) ;
      MRISsetNeighborhoodSize(mris_src, nbrs) ;
      MRIScomputeMetricProperties(mris_src) ;
      MRISdistanceToCOG(mris_src);
      MRISaverageCurvatures(mris_src, navgs) ;
    }

  field=var=0;
  for(k=0;k<mris_src->nvertices;k++)
    {
      tp=mris_src->vertices[k].curv;
      field+=tp;
      var+=SQR(tp);
    }
  field/=mris_src->nvertices;
  var=var/mris_src->nvertices-SQR(field);
  sigma=sqrt(var);

  if(VERBOSE_MODE)
    fprintf(stderr,"\n         source values: mean = %2.5f sigma = %2.5f  ",
            field,sigma);

  mris1=mris_fdst;
  mris2=mris_vdst;

  /*calculate destination fields*/ 
  oldfield=oldvar=0;
  for(k=0;k<mris1->nvertices;k++)
    {
      tp=mris1->vertices[k].curv;
      oldfield+=tp;
      oldvar+=SQR(tp);
    }
  oldfield/=mris1->nvertices;
  oldvar=oldvar/mris1->nvertices-SQR(oldfield);
  oldsigma=sqrt(oldvar);

  if(VERBOSE_MODE)
    fprintf(stderr,"\n         destination values: mean = "
            "%2.5f sigma = %2.5f  ",oldfield,oldsigma);

  /*scale dst fields to sources values*/
  for(k=0;k<mris_src->nvertices;k++)
    {
      mris1->vertices[k].curv= 
        (mris1->vertices[k].curv-oldfield)*sigma/oldsigma+field;
      mris2->vertices[k].curv*=var/oldvar;
    }

  oldfield=field;
  oldvar=var;
  oldsigma=sigma;
  
  iter=10;stop=0.1;
  while((iter--)&&(stop>THRESHOLD))
    {
      field=var==0;
      count=0.0;
      for(k=0;k<mris_src->nvertices;k++)
        {
          sse=SQR(mris_src->vertices[k].curv-mris1->vertices[k].curv)
            /mris2->vertices[k].curv;
          if(sse<10.)
            {
              tp=mris_src->vertices[k].curv;
              field+=tp;
              var+=SQR(tp);
              count+=1.0;
            }
        }
      field/=count;
      var=var/count-SQR(field);
      sigma=sqrt(var);
    
      for(k=0;k<mris_src->nvertices;k++)
        {
          mris1->vertices[k].curv=
            (mris1->vertices[k].curv-oldfield)*sigma/oldsigma+field;
          mris2->vertices[k].curv*=var/oldvar;
        }
    
      stop=(stop+fabs(oldfield-field)/sigma)/2;
    
      oldfield=field;
      oldvar=var;
      oldsigma=sigma;
    }

  if(VERBOSE_MODE)
    fprintf(stderr,"\n         %d iterations,  %5.1f%% of "
            "vertices used, mean=%2.5f, sigma=%2.5f"
            ,10-iter,100.*count/mris_src->nvertices,field,sigma );
  if(whichfield==DIST_MODE)
    {
      MRIScomputeMetricProperties(mris_src) ;
      MRIScomputeSecondFundamentalForm(mris_src) ;
      MRISuseMeanCurvature(mris_src) ;
    }
}

#if NO_SELF_INTERSECTION
static int mrisRemoveNeighborGradientComponent(MRI_SURFACE *mris, int vno);
static int mrisRemoveNormalGradientComponent(MRI_SURFACE *mris, int vno);
static int mrisLimitGradientDistance(MRI_SURFACE *mris, MHT *mht, int vno);
#endif

#if NO_SELF_INTERSECTION
#define MIN_NBR_DIST  (0.25)
static int mrisRemoveNeighborGradientComponent(MRI_SURFACE *mris, int vno)
{
  VERTEX   *v, *vn ;
  int      n ;
  float    dx, dy, dz, dot, x, y, z, dist ;

  v = &mris->vertices[vno] ;
  
  x = v->x ; y = v->y ; z = v->z ; 
  for (n = 0 ; n < v->vnum ; n++)
    {
      vn = &mris->vertices[v->v[n]] ;
      dx = vn->x - x ; dy = vn->y - y ; dz = vn->z - z ;
      dist = sqrt(dx*dx + dy*dy + dz*dz) ;

      /* too close - take out gradient component in this dir. */
      if (dist <= MIN_NBR_DIST)  
        {
          dx /= dist ; dy /= dist ; dz /= dist ;
          dot = dx*v->odx + dy*v->ody + dz*v->odz ;
          if (dot > 0.0)
            {
              v->odx -= dot*dx ;
              v->ody -= dot*dy ;
              v->odz -= dot*dz ;
            }
        }
    }

  return(NO_ERROR) ;
}

static int mrisRemoveNormalGradientComponent(MRI_SURFACE *mris, int vno)
{
  VERTEX   *v ;
  float    dot ;

  v = &mris->vertices[vno] ;
  
  dot = v->nx*v->odx + v->ny*v->ody + v->nz*v->odz ;
  v->odx -= dot*v->nx ;
  v->ody -= dot*v->ny ;
  v->odz -= dot*v->nz ;

  return(NO_ERROR) ;
}

static int mrisLimitGradientDistance(MRI_SURFACE *mris, MHT *mht, int vno)
{
  VERTEX   *v ;

  v = &mris->vertices[vno] ;

  mrisRemoveNeighborGradientComponent(mris, vno) ;
  if (MHTisVectorFilled(mht, mris, vno, v->odx, v->ody, v->odz))
    {
      mrisRemoveNormalGradientComponent(mris, vno) ;
      if (MHTisVectorFilled(mht, mris, vno, v->odx, v->ody, v->odz))
        {
          v->odx = v->ody = v->odz = 0.0 ;
          return(NO_ERROR) ;
        }
    }

  return(NO_ERROR) ;
}
#endif

static int mrisAverageGradients(MRIS *mris,int niter){
  int vno, vnum,*pnb,vnb;
  float dx,dy,dz,dot,num;
  VERTEX *v,*vn;
        
  while(niter--){
    for (vno = 0 ; vno < mris->nvertices ; vno++) {
      v = &mris->vertices[vno] ;
                        
      dx = v->odx ; dy = v->ody ; dz = v->odz ;
      pnb = v->v ;
                        
      vnum = v->vnum ;
      for (num = 0.0f , vnb = 0 ; vnb < vnum ; vnb++) {
        vn = &mris->vertices[*pnb++] ;    /* neighboring vertex pointer */
                                
        dot = vn->odx * v->odx + vn->ody * v->ody + vn->odz*v->odz ;
        if (dot < 0)
          continue ;  /* pointing in opposite directions */
                                
        num++ ;
        dx += vn->odx ; dy += vn->ody ; dz += vn->odz ; 
      }
      num++ ;
      v->tdx = dx / num ;
      v->tdy = dy / num ;
      v->tdz = dz / num ;
    }
    for (vno = 0 ; vno < mris->nvertices ; vno++) {
      v = &mris->vertices[vno] ;
      v->odx=v->tdx;
      v->ody=v->tdy;
      v->odz=v->tdz;
    }
  }
  return NO_ERROR;
}


//correction of the segmentation by incorporating an atlas-based force
static void MRISCorrectSurface(MRI_variables *MRI_var)
{
  float x,y,z,sx,sy,sz,sd,sxn,syn,szn,sxt,syt,szt,nc;
  float force,force1,force3;
  float ct;

  float d,dx,dy,dz,nx,ny,nz;
  VERTEX *v; 
  int iter,k,m,n;
  int it,jt,niter;
  float decay=0.8,update=0.9;

  int int_smooth=1;
  
  MRIS *mris;

  double ml;
  double lm,d10m[3],d10,f1m,f2m,f3m,dm,dbuff;  
  float ***dist;
  float cout,pcout=0,coutbuff,varbuff,mean_sd[10],mean_dist[10];

#if WRITE_SURFACES
  char fname[500];
  MRIS *mris_tmp;
#endif

  double xCOG,yCOG,zCOG;
  double dCOG;

#if NO_SELF_INTERSECTION
  /* to avoid self-intersection */ 
  MHT    *mht = NULL ;
#endif


  xCOG=yCOG=zCOG=0; /* to stop compilator warnings !*/ 

  mris=MRI_var->mris;


  MRISsetNeighborhoodSize(mris, 1) ;
  MRIScomputeNormals(mris);

  dist = (float ***) malloc( mris->nvertices*sizeof(float**) );

  for( it = 0; it < mris->nvertices; it++ ) 
    {
      dist[it] = (float**) malloc( 4*sizeof(float*) );
      for( jt = 0; jt < 4; jt++ ) 
        {
          dist[it][jt] = (float*) calloc( 3, sizeof(float));
        }  
    }

#if WRITE_SURFACES
  mris_tmp=MRISclone(mris);
#endif

  for(k=0;k<mris->nvertices;k++)
    for (m=0;m<4;m++)
      for (n=0;n<3;n++)
        dist[k][m][n]=0;

  for (n=0;n<10;n++)
    {
      mean_sd[n]=0;
      mean_dist[n]=0;
    }

  niter =int_smooth;
  force = 0.0f ; 
  pcout=0;


  for (k=0;k<mris->nvertices;k++)
    {
      v = &mris->vertices[k];
      v->odx = 0;
      v->ody = 0;
      v->odz = 0;
    }
  
  ml=2;
  ////////////////////////////////////////////////////////////////////////
  // iteration starts here
  for (iter=0;niter;iter++)
    {
#if NO_SELF_INTERSECTION
      /* avoid self-intersection */
      mht = MHTfillTable(mris, mht) ;
#endif

#if WRITE_SURFACES
      sprintf(fname,"./rh.s_c%d",iter);
      MRISwrite(mris,fname);
#endif

      cout = lm = d10 = f1m = f2m = f3m = dm = 0;
      xCOG = yCOG = zCOG = 0;
      for (k=0;k<mris->nvertices;k++)
        {
          v = &mris->vertices[k];
          v->tx = v->x;
          v->ty = v->y;
          v->tz = v->z;
          xCOG+=v->x;
          yCOG+=v->y;
          zCOG+=v->z;
        }
      xCOG/=mris->nvertices;
      yCOG/=mris->nvertices;
      zCOG/=mris->nvertices;

      //****************************************
      //brain atlas force
      for (k=0;k<mris->nvertices;k++)
        {
          mris->vertices[k].val=0;  

#if WRITE_SURFACES  //to write out shape correction information
          mris_tmp->vertices[k].curv=0;
#endif

          //      if((iter<4) && (!MRI_var->mrisphere->vertices[k].val))
          if(!MRI_var->mrisphere->vertices[k].val)
            continue;
            
          // distance from the center of gravity
          dCOG=sqrt(SQR(mris->vertices[k].tx-xCOG)
                    +SQR(mris->vertices[k].ty-yCOG)
                    +SQR(mris->vertices[k].tz-zCOG));

          // difference between a cog distance of a particular vertex from a
          // template vs. a cog distance of a current vertex
          force3=(MRI_var->mris_dCOG->vertices[k].curv-dCOG)/
            sqrt(MRI_var->mris_var_dCOG->vertices[k].curv);
      
          force3=0.5*tanh(2*force3);
      
          mris->vertices[k].val=force3;

#if WRITE_SURFACES
          mris_tmp->vertices[k].curv=force3;
#endif     

          f3m+=force3;
        }
      MRISaverageVals(mris,2);
    

#if WRITE_SURFACES
      sprintf(fname,"./rh.s_cvalues%d",iter);
      MRISwriteCurvature(mris_tmp,fname);
#endif

      for (k=0;k<mris->nvertices;k++){
        v = &mris->vertices[k];
        x = v->tx;
        y = v->ty;
        z = v->tz;
        nx = v->nx;
        ny = v->ny;
        nz = v->nz;
        sx=sy=sz=sd=0;
        n=0;
        for (m=0;m<v->vnum;m++)
          {
            sx += dx =mris->vertices[v->v[m]].tx - x;
            sy += dy =mris->vertices[v->v[m]].ty - y;
            sz += dz =mris->vertices[v->v[m]].tz - z;
            sd += sqrt(dx*dx+dy*dy+dz*dz);
            n++;
          }
        sx = sx/n;
        sy = sy/n;
        sz = sz/n;
        sd = sd/n;
      
        lm+=sd;

        nc = sx*nx+sy*ny+sz*nz;
      
        sxn = nc*nx;
        syn = nc*ny;
        szn = nc*nz;
        sxt=sx-sxn;
        syt=sy-syn;
        szt=sz-szn;

        v->nc=nc;
                      
        //*******************************************
        //Iteration
        force3=v->val;
        if(force3!=0 || (SQR(v->odx)+SQR(v->ody)+SQR(v->odz))!=0)
          {
            ct=0.8;
            force1=0.5;
          }
        else
          {
            force1=0.1;
            ct=0.2;
          }
                     
        f1m+=force1;
                        
        // dX = ct * St + force1 * Sn + force3 * Vn
        dx = sxt*ct + force1*sxn+v->nx*force3;
        dy = syt*ct + force1*syn+v->ny*force3;
        dz = szt*ct + force1*szn+v->nz*force3;
      
        // modify dX with the weighted average of dX calculated and previous dX
        dx = decay*v->odx+update*dx;
        dy = decay*v->ody+update*dy;
        dz = decay*v->odz+update*dz;
      
        // if too big, make it smaller
        if ((d=sqrt(dx*dx+dy*dy+dz*dz))>1.0)
          {
            dx /= d;
            dy /= d;
            dz /= d;
          }
                
        if(FZERO(v->val)){ 
          dx = dy = dz = 0.0;
        }
                
        // cache the current value
        v->odx = dx;
        v->ody = dy;
        v->odz = dz;
      }
        
      // average gradients 
      mrisAverageGradients(mris,3);
                        
      for (k=0;k<mris->nvertices;k++){
        v = &mris->vertices[k];
        x = v->tx;
        y = v->ty;
        z = v->tz;
#if NO_SELF_INTERSECTION
        /* check if we can update the surface */
        MHTremoveAllFaces(mht, mris, &mris->vertices[k]);
        mrisLimitGradientDistance(mris, mht, k) ;
#endif
        dx=v->odx;
        dy=v->ody;
        dz=v->odz;

        d=sqrt(dx*dx+dy*dy+dz*dz);
      
        dm+=d;

        dist[k][iter%4][0]=x;
        dist[k][iter%4][1]=y;
        dist[k][iter%4][2]=z;      d10m[0] = d10m[1] = d10m[2] = 0;

        for(n=0;n<4;n++)
          {
            d10m[0]+=dist[k][n][0]/4;
            d10m[1]+=dist[k][n][1]/4;
            d10m[2]+=dist[k][n][2]/4;
          }
    
        dbuff=0;
        for(n=0;n<4;n++)
          dbuff+=SQR(dist[k][n][0]-d10m[0])+SQR(dist[k][n][1]-d10m[1])+
            SQR(dist[k][n][2]-d10m[2]);

        d10+=dbuff/4;

        // move the vertex position
        v->x += dx;
        v->y += dy;
        v->z += dz;
      }

      lm /=mris->nvertices;
      f1m /=mris->nvertices;
      f2m /=mris->nvertices;
      f3m /=mris->nvertices;
      dm /=mris->nvertices;
      d10 /=mris->nvertices;


      ml=lm;

      mean_sd[iter%10]=lm;
      mean_dist[iter%10]=d10;

      coutbuff=0;
      for(n=0;n<10;n++)
        coutbuff+=mean_sd[n]/10;
    
      varbuff=0;
      for(n=0;n<10;n++)
        varbuff+=SQR(mean_sd[n]-coutbuff);

      cout=varbuff;

      coutbuff=0;
      for(n=0;n<10;n++)
        coutbuff+=mean_dist[n]/10;

      varbuff=0;
      for(n=0;n<10;n++)
        varbuff+=SQR(mean_dist[n]-coutbuff);

      cout+=10*varbuff;

      coutbuff=cout;

      cout=(cout+pcout)/2;

      pcout=coutbuff;

      MRIScomputeNormals(mris);

      /*    if ((niter==int_smooth) && !(iter % 5))
            {
            fprintf(stderr,
            "%d: lm=%5.3f,f1m=%5.3f,f2m=%5.3f,f3m=%5.3f,
            dm=%5.3f,d10m=%5.3f,c=%5.3f\n"
            ,iter,lm,f1m,f2m,f3m,dm,d10,100*cout);
            }*/

      if (niter==int_smooth)
        {
          if(((iter>10)&&(10000*cout<1))||(iter>48))
            niter--;
        }
      else
        niter--;
    }
  fprintf(stderr,"%d iterations",iter);

#if WRITE_SURFACES
  sprintf(fname,"./shape_correction%d",iter);
  MRISwrite(mris,fname);
  MRISfree(&mris_tmp);
#endif

#if NO_SELF_INTERSECTION
  MHTfree(&mht) ;
#endif
        
  MRIScomputeNormals(mris);
}

/*compute local values and store them into mrisphere*/
static void MRISComputeLocalValues(MRI_variables *MRI_var)
{
  MRIS *mris,*mrisphere;
  MRI* mri;
  int k,m,n,total_vertices,nmissing;
  VERTEX *v,*vsphere;
  float dist,distance;
  double gm_val,csf_val,val,x,y,z, xw, yw, zw;
  double xw1,yw1,zw1,xw2,yw2,zw2,nx, ny, nz, mag, max_mag ;
  double csf,gm;
  float mean_csf,mean_gray,mean_trans;
  float var_csf,var_gray,var_trans;
  int ninside,noutside;
  double w1,sse;
  double BOUNDCSF;
  int niter;
  int nvertices;
  int ref;
  
  int posvertices,negvertices;

  BOUNDCSF=MIN(MRI_var->CSF_intensity*2.,MRI_var->CSF_MAX);

  /*stop warnings*/
  n=0;distance=0;

  /*default values*/
  ninside=-10;
  noutside=10;

  mri=MRI_var->mri_orig;
  mrisphere=MRI_var->mrisphere;
  mris=MRI_var->mris;
  
  nvertices=mris->nvertices;
  
  /*compute the local values*/
  var_csf=var_gray=0;nmissing=0;
  mean_csf=0;mean_gray=0;total_vertices=0;

  MRIScomputeNormals(mris);

  if(MRI_var->atlas)
    {
      posvertices=negvertices=0;
      MRISsetNeighborhoodSize(mris,1) ;
      MRIScomputeMetricProperties(mris) ;
      MRISdistanceToCOG(mris);
      MRISaverageCurvatures(mris, 20) ;
      /*use the template information to localize the border of the brain*/
      for(k=0;k<nvertices;k++)
        {
          vsphere = &mrisphere->vertices[k] ;
          vsphere->tx=
            (MRI_var->mris_dCOG->vertices[k].curv-mris->vertices[k].curv);
          vsphere->ty=sqrt(MRI_var->mris_var_dCOG->vertices[k].curv);
          vsphere->tz=vsphere->tx/vsphere->ty;
          if(vsphere->tz>=0)
            negvertices++;
          else
            posvertices++;
        }
      if(VERBOSE_MODE)
        fprintf(stderr,"\n      %5.2f%% of 'positive'vertices"
                "\n      %5.2f%% of 'negative' vertices"
                ,100.*posvertices/nvertices,100.*negvertices/nvertices);
    }

  if(VERBOSE_MODE)
    fprintf(stderr,"\n      first pass on the vertices of the tesselation");

  for(ref=0,k=0;k<nvertices;k++)
    {
      v = &mris->vertices[k] ;
      vsphere = &mrisphere->vertices[k];
      v->marked=0 ;
    
      /*determine the normal direction in Voxel coordinates*/
      x = v->x ; y = v->y ; z = v->z ;
      myWorldToVoxel(mri, x, y, z, &xw, &yw, &zw) ;
      x = v->x + v->nx ; y = v->y + v->ny ; z = v->z + v->nz ;
      myWorldToVoxel(mri, x, y, z, &xw1, &yw1, &zw1) ;
      nx = xw1 - xw ; ny = yw1 - yw ; nz = zw1 - zw ; 

      /* 
         find the distance in the directions parallel and anti-parallel to
         the surface normal in which the gradient is pointing 'inwards'. 
      */
      /* if atlas Mode on, use atlas information to infer the border position*/
      /*    NOT USED RIGHT NOW...
            if(MRI_var->atlas)
            {
            if(vsphere->tz>0.5)
            ref=(int)(MIN(vsphere->ty/2,1.));  
            else
            ref=0;
            }
      */
      noutside=10+ref;
      ninside=-10+ref;
    
      csf = -10.0f ; gm=-10.f;mag = 0.0f ; max_mag = 0.0f ;
      for (dist = noutside ; dist > ninside ; dist -= 0.5)
        {
          /*val at current location*/
          x = v->x + v->nx*dist ; 
          y = v->y + v->ny*dist ; 
          z = v->z + v->nz*dist ; 
          myWorldToVoxel(mri, x, y, z, &xw, &yw, &zw) ;
          MRIsampleVolume(mri, xw, yw, zw, &val) ;
          /*value at next location: potential gm intensity*/
          x = v->x + v->nx*(dist-1) ; 
          y = v->y + v->ny*(dist-1) ; 
          z = v->z + v->nz*(dist-1) ; 
          myWorldToVoxel(mri, x, y, z, &xw1, &yw1, &zw1) ;
          MRIsampleVolume(mri, xw1, yw1, zw1, &gm_val) ;
          /*value at previous location: potential csf value*/
          x = v->x + v->nx*(dist+1) ; 
          y = v->y + v->ny*(dist+1) ; 
          z = v->z + v->nz*(dist+1) ; 
          myWorldToVoxel(mri, x, y, z, &xw2, &yw2, &zw2) ;
          MRIsampleVolume(mri, xw2, yw2, zw2, &csf_val) ;   

          if (csf_val< val && 
              val<gm_val && 
              csf_val<BOUNDCSF && 
              gm_val>MRI_var->CSF_intensity)  
            /* in right range */
            {
              MRIsampleVolumeDerivativeScale(mri, 
                                             xw, yw, zw, 
                                             nx, ny,nz,&mag,1.);
              /* see if we are at a local maximum in the gradient magnitude */
              /*we only consider local negative gradiant*/
              mag=-mag;
              if (mag >max_mag)
                {
                  max_mag = mag ;
                  csf = csf_val ;
                  gm= gm_val;
                  distance=dist;
                }
            }
        }     
      if (max_mag> 5.0f) /*keep that vertex*/
        {
          v->marked = 1 ;
          v->val = csf ;
          v->val2=gm;
          v->mean = distance ;
          mean_csf += csf ; 
          total_vertices++ ;
          mean_gray += gm; 
          var_csf+=SQR(csf);
          var_gray+=SQR(gm);
        }
      else
        {
          nmissing++ ;
          v->val = 0.0f ;
        }    
    }
  mean_gray /= (float)total_vertices ;
  mean_csf /= (float)total_vertices ;
  var_csf=var_csf/(float)total_vertices-mean_csf*mean_csf;
  var_gray=var_gray/(float)total_vertices-mean_gray*mean_gray;
  if(VERBOSE_MODE)
    fprintf(stderr,"\n      %5.2f%% of missing voxels"
            "\n      mean csf=%5.1f, var csf=%5.1f, "
            "mean gm=%5.1f, var gm=%5.1f"
            ,100.*(float)nmissing/nvertices,
            mean_csf,sqrt(var_csf),mean_gray,sqrt(var_gray));

  if(((float)nmissing/nvertices)<0.75)
    {
      MRI_var->CSF_intensity=int((MRI_var->CSF_intensity+mean_csf)/2.);
      MRI_var->GM_intensity=int((MRI_var->GM_intensity+mean_gray)/2.);
    }
  if(VERBOSE_MODE)
    fprintf(stderr,"\n      second pass on the vertices of the tesselation");
  for(nmissing=0,k=0;k<nvertices;k++)
    {
      v = &mris->vertices[k] ;
      vsphere=&mrisphere->vertices[k];
      vsphere->marked=0;
                
      if(v->marked)
        {
          distance=0;n=0;
          for (m=0;m<v->vnum;m++)
            {
              if(mris->vertices[v->v[m]].marked==1)
                {
                  distance+=mris->vertices[v->v[m]].mean;
                  n++;
                }
            }
          if(n)
            distance/=(float)n;
          else distance=1000;
        }
      if(v->marked && ((fabs(distance-v->mean)<3.) || (!n)))
        {
          vsphere->marked=1;
          vsphere->x=v->val;
          vsphere->z=v->val2;
                                
          if(!n)
            {
              vsphere->mean=0.5;
              nmissing++;
            }
          else
            vsphere->mean=1.;
        }
      else
        {
          vsphere->marked=0;
          nmissing++;
          vsphere->x=MRI_var->CSF_intensity;
          vsphere->z=MRI_var->GM_intensity;
          vsphere->mean=0.2;
          /*if Atlas Mode on, we can use the 
            spatial information to infer where the surface is*/
          if(MRI_var->atlas)
            {
              sse=vsphere->tz;
              if(sse>0)
                {   
                  x = v->x ; y = v->y ; z = v->z ; 
                  myWorldToVoxel(mri, x, y, z, &xw, &yw, &zw) ;
                  MRIsampleVolume(mri, xw, yw, zw, &val) ;    
                  if(val>mean_csf && val<MRI_var->WM_intensity)
                    /*probably GM intensity*/
                    {
                      vsphere->z=(val+vsphere->z)/2.;
                      if(SQR(sse)<1.)
                        vsphere->mean=0.5;
                      else
                        vsphere->mean=0.25;
                    }
                }
            }
        }
    }

  /*average the local values twice before computing local variance*/
  niter=2;
  while(niter)
    {
      for(k=0;k<nvertices;k++)
        {
          vsphere = &mrisphere->vertices[k] ;
          vsphere->tx=vsphere->x;
          vsphere->ty=vsphere->mean;
          vsphere->tz=vsphere->z;
        }

      for(k=0;k<nvertices;k++)
        {
          v=&mris->vertices[k];
          vsphere = &mrisphere->vertices[k] ;

          w1=vsphere->ty;
          csf=vsphere->tx*w1;
          gm=vsphere->tz*w1;
          n=1;
          for (m=0;m<v->vnum;m++)
            {
              csf+=
                mrisphere->vertices[v->v[m]].tx*
                mrisphere->vertices[v->v[m]].ty;
              w1+=mrisphere->vertices[v->v[m]].ty;
              gm+=mrisphere->vertices[v->v[m]].tz*
                mrisphere->vertices[v->v[m]].ty;
              n++;
            }
          vsphere->x=csf/w1;
          vsphere->z=gm/w1;
          vsphere->mean=w1/n;
        }
      niter--;
    } 
  MRISsetNeighborhoodSize(mris, 2) ;
  for(k=0;k<nvertices;k++)
    {
      vsphere = &mrisphere->vertices[k] ;
      vsphere->tx=vsphere->x;
      vsphere->tz=vsphere->z;
    }
  for(k=0;k<nvertices;k++)
    {
      v=&mris->vertices[k];
      vsphere = &mrisphere->vertices[k] ;
      /*after two iterations compute the local statistics*/
    
      csf=vsphere->tx;
      gm=vsphere->tz;
      mean_csf=csf;
      var_csf=SQR(csf);
      mean_gray=gm;
      var_gray=SQR(gm);
      n=1;
      for (m=0;m<v->vnum;m++)
        {
          csf=mrisphere->vertices[v->v[m]].tx;
          gm=mrisphere->vertices[v->v[m]].tz;
          mean_csf+=csf;
          mean_gray+=gm;
          var_csf+=SQR(csf);
          var_gray+=SQR(gm);
          n++;
        }
      vsphere->x=mean_csf/(float)n;
      vsphere->z=mean_gray/(float)n;
      vsphere->odx=MAX(0.,var_csf/(float)n-SQR(vsphere->x));
      vsphere->odz=MAX(0.,var_gray/(float)n-SQR(vsphere->z));
      vsphere->odx=MAX(5.,MIN(20.,sqrt(vsphere->odx)));
      vsphere->odz=MAX(5.,MIN(20.,sqrt(vsphere->odz)));
    }

  MRISsetNeighborhoodSize(mris, 1) ;
  /*reaverage the local values*/
  niter=5;
  while(niter)
    {
      for(k=0;k<nvertices;k++)
        {
          vsphere = &mrisphere->vertices[k] ;
          vsphere->tx=vsphere->x;
          vsphere->ty=vsphere->mean;
          vsphere->tz=vsphere->z;
          vsphere->tdx=vsphere->odx;
          vsphere->tdz=vsphere->odz;
        }

      for(k=0;k<nvertices;k++)
        {
          v=&mris->vertices[k];
          vsphere = &mrisphere->vertices[k] ;

          w1=vsphere->ty;
          csf=vsphere->tx*w1;
          gm=vsphere->tz*w1;
          var_csf=vsphere->tdx*w1;
          var_gray=vsphere->tdz*w1;
          n=1;
          for (m=0;m<v->vnum;m++)
            {
              csf+=
              mrisphere->vertices[v->v[m]].tx*mrisphere->vertices[v->v[m]].ty;
              w1+=mrisphere->vertices[v->v[m]].ty;
              gm+=
              mrisphere->vertices[v->v[m]].tz*mrisphere->vertices[v->v[m]].ty;
              var_csf+=
              mrisphere->vertices[v->v[m]].tdx*mrisphere->vertices[v->v[m]].ty;
              var_gray+=
              mrisphere->vertices[v->v[m]].tdz*mrisphere->vertices[v->v[m]].ty;
        
              n++;
            }
          vsphere->x=csf/w1;
          vsphere->z=gm/w1;
          vsphere->odx=var_csf/w1;
          vsphere->odz=var_gray/w1;
          vsphere->mean=w1/n;
        }
      niter--;
    } 

  /*Compute Transition Intensity values*/
  var_csf=var_gray=0;
  mean_csf=0;mean_gray=0;
  mean_trans=0;var_trans=0;
  for(k=0;k<nvertices;k++)
    {
      vsphere=&mrisphere->vertices[k];
      csf=vsphere->x;
      mean_csf+=csf;
      var_csf+=SQR(csf);
      gm=vsphere->z;
      mean_gray+=gm;
      var_gray+=SQR(gm);
      vsphere->y=
        (csf*vsphere->odz+gm*vsphere->odx)/(vsphere->odx+vsphere->odz);
      mean_trans+=vsphere->y;
      var_trans+=SQR(vsphere->y);
    }
  mean_gray /= (float)nvertices ;
  mean_csf /= (float)nvertices ;
  mean_trans/=(float)nvertices ;
  var_csf=var_csf/(float)nvertices-mean_csf*mean_csf;
  var_gray=var_gray/(float)nvertices-mean_gray*mean_gray;
  var_trans=var_trans/(float)nvertices-mean_trans*mean_trans;

  if(VERBOSE_MODE)
    fprintf(stderr,"\n      %5.2f%% of missing voxels"
            "\n      mean csf=%5.1f, var csf=%5.1f, mean gm=%5.1f, "
            "var gm=%5.1f"
            "\n      mean transition=%5.1f, var transition=%5.1f" 
            ,100.*(float)nmissing/nvertices,
            mean_csf,sqrt(var_csf),mean_gray,sqrt(var_gray)
            ,mean_trans,sqrt(var_trans));
  
  if(MRI_var->atlas)
    {
      MRISsetNeighborhoodSize(mris, 2) ;
      MRIScomputeMetricProperties(mris) ;
      MRIScomputeSecondFundamentalForm(mris) ;
      MRISuseMeanCurvature(mris) ;
    }
}

static double estimateNRG(MRI_variables *MRI_var, 
			  double cx, double cy ,double cz);
static void computeGradient(MRI_variables *MRI_var,
                            double cx, double cy ,double cz,
                            double *gx,double *gy, double *gz);
static int computeCOG(MRI_variables *MRI_var,
                      double *xCOG, double *yCOG, double *zCOG,int mode);

/* compute the NRG associated with the center (cx,cy,cz) */
static double estimateNRG(MRI_variables *MRI_var ,
                          double cx, double cy ,double cz){
  int n;
  double NRG;
  double dCOG;
  MRIS *mris;

  mris=MRI_var->mris;

  for(NRG = 0, n = 0 ; n < mris->nvertices ; n++){
    dCOG=sqrt(SQR(mris->vertices[n].x-cx)+SQR(mris->vertices[n].y-cy)
              +SQR(mris->vertices[n].z-cz));
    NRG+=SQR(dCOG-MRI_var->mris_dCOG->vertices[n].curv)/
      MRI_var->mris_var_dCOG->vertices[n].curv;
  }
  return NRG;
}

/* compute the gradient of the energy defined above at location (cx,cy,cz) */
static void computeGradient(MRI_variables *MRI_var,
                            double cx, double cy ,double cz,
                            double *gx,double *gy, double *gz){
  int n;
  double tx,ty,tz,dCOG;
  MRIS *mris;
        
  mris=MRI_var->mris;

  tx=ty=tz=0.0;
  for(tx = 0 , ty = 0 , tz = 0 , n = 0 ; n < mris->nvertices ; n++){
    dCOG=sqrt(SQR(mris->vertices[n].x-cx)+SQR(mris->vertices[n].y-cy)
              +SQR(mris->vertices[n].z-cz));
    tx += (1.0-MRI_var->mris_dCOG->vertices[n].curv/dCOG)/
      MRI_var->mris_var_dCOG->vertices[n].curv
      *(mris->vertices[n].x-cx);
                
    ty += (1.0-MRI_var->mris_dCOG->vertices[n].curv/dCOG)/
      MRI_var->mris_var_dCOG->vertices[n].curv
      *(mris->vertices[n].y-cy);

    tz += (1.0-MRI_var->mris_dCOG->vertices[n].curv/dCOG)/
      MRI_var->mris_var_dCOG->vertices[n].curv
      *(mris->vertices[n].z-cz);

  }
  (*gx)=tx/(double)mris->nvertices;
  (*gy)=ty/(double)mris->nvertices;
  (*gz)=tz/(double)mris->nvertices;
}

/* compute the center of gravity of the surface 
   if more is zero: using normal average
   if more is one: take into account potential errors with average shape */
static int computeCOG(MRI_variables *MRI_var,
                      double *xCOG, double *yCOG, double *zCOG,int mode){
  int k;
  double x,y,z;
  VERTEX *v;
  MRIS *mris;
  int niters;
  /* NRG parameters */
  double NRG,last_NRG;
  /* gradient parameters */
  double dx,dy,dz,d,epsilon;
        
  mris=MRI_var->mris;   
  /* initial guess */
  x=y=z=0.0;
  for (k=0;k<mris->nvertices;k++){
    v = &mris->vertices[k];
    x += v->x;
    y += v->y;
    z += v->z;
  }
  // get the center of gravity of surfaces
  x /= mris->nvertices;
  y /= mris->nvertices;
  z /= mris->nvertices;

  switch(mode){
  case 0:
    (*xCOG)=x;
    (*yCOG)=y;
    (*zCOG)=z;
    break;
  default:
  case 1:
    /* compute the initial NRG */
    NRG=estimateNRG(MRI_var,x,y,z);
    fprintf(stderr,"Computing COG for corrected surface\n"
            "Initial Configuration: NRG=%lf, ( %f , %f , %f )"
            ,NRG, x, y, z); 
    /* iteratively minize the NRG */
    last_NRG=NRG+1.0;
    niters=0;
    while(NRG<last_NRG){
      niters++;
      last_NRG=NRG;
      /* compute the gradient */
      computeGradient(MRI_var,x,y,z,&dx,&dy,&dz);
                        
      /* bound gradient by displacement of 1.0 mm */
      d=sqrt(SQR(dx)+SQR(dy)+SQR(dz));
      if(d>1.0){
        dx/=d;
        dy/=d;
        dz/=d;
      }
      /*   fprintf(stderr,"\n gradient:(%f,%f,%f)",dx,dy,dz); */
    
      epsilon=2.0;
      while(NRG>=last_NRG){
        epsilon/=2.0;
        NRG=estimateNRG(MRI_var,x+epsilon*dx,y+epsilon*dy,z+epsilon*dz);
        d=sqrt(SQR(dx)+SQR(dy)+SQR(dz));
        if(epsilon*d<0.00000000001) 
          //FZERO(epsilon*(SQR(dx)+SQR(dy)+SQR(dz)))) 
          break;
      }
                
      if(NRG<last_NRG){
        x=x+epsilon*dx;
        y=y+epsilon*dy;
        z=z+epsilon*dz;
                                
        /* printf(stderr,"\nNew Minimum found: NRG=%lf,( %f , %f , %f )"
                                        ,NRG , x, y,z);*/ 
      }else
        NRG=estimateNRG(MRI_var,x,y,z);
    }  

    fprintf(stderr,"\nFinal Minimum found: NRG=%lf, ( %f , %f , %f )"
            , estimateNRG(MRI_var,x,y,z), x, y,z); 

    (*xCOG)=x;
    (*yCOG)=y;
    (*zCOG)=z;
                
    break;
  }
  return NO_ERROR;
}

static MRI* generateFinalMRI(MRI_variables *MRI_var){
  MRI *mri,*mri_src,*mri_orig;
  MRIS *mris,*mrisphere;
  int k,u,v,numu,numv,i,j,imnr;
  float x0,y0,z0,x1,y1,z1,x2,y2,z2,d0,d1,d2,dmax,dCOG,val,
    px0,py0,pz0,px1,py1,pz1,px,py,pz;
  FACE *face;
  double tx,ty,tz,xCOG,yCOG,zCOG;
  int changed,new_val;
        
  mri_orig=MRI_var->mri_orig;
  mri_src=MRI_var->mri_src;
  mri=MRIclone(MRI_var->mri_orig,NULL);
        
  mris=MRI_var->mris;
  mrisphere=MRI_var->mrisphere;

  // extract the volume
  MRISpeelBrain(0.0,mri,MRI_var->mris,1);

  //compute COG coord
  computeCOG(MRI_var,&xCOG,&yCOG,&zCOG,1);

        
  //write surface values in the volume
  for( k = 0 ; k < mris->nfaces ; k++){
    face=&mris->faces[k];
    if((mrisphere->vertices[face->v[0]].val==0) &&
       (mrisphere->vertices[face->v[1]].val==0) &&
       (mrisphere->vertices[face->v[2]].val==0))
      continue;

    //compute value for face
    val=0.0;
    for(u=0;u<3;u++){
      dCOG=sqrt(SQR(mris->vertices[face->v[u]].x-xCOG)
                +SQR(mris->vertices[face->v[u]].y-yCOG)+
                SQR(mris->vertices[face->v[u]].z-zCOG));
      val=MAX(val,MRI_var->mris_dCOG->vertices[face->v[u]].curv-dCOG
              +3.*sqrt(MRI_var->mris_var_dCOG->vertices[face->v[u]].curv));
    }
                
    // calculate three vertices
    x0 =mris->vertices[mris->faces[k].v[0]].x;    
    y0 =mris->vertices[mris->faces[k].v[0]].y;    
    z0 =mris->vertices[mris->faces[k].v[0]].z;    
    x1 =mris->vertices[mris->faces[k].v[1]].x;    
    y1 =mris->vertices[mris->faces[k].v[1]].y;    
    z1 =mris->vertices[mris->faces[k].v[1]].z;    
    x2 =mris->vertices[mris->faces[k].v[2]].x;    
    y2 =mris->vertices[mris->faces[k].v[2]].y;    
    z2 =mris->vertices[mris->faces[k].v[2]].z;
    
    // calculate the sides
    d0 = sqrt(SQR(x1-x0)+SQR(y1-y0)+SQR(z1-z0));
    d1 = sqrt(SQR(x2-x1)+SQR(y2-y1)+SQR(z2-z1));
    d2 = sqrt(SQR(x0-x2)+SQR(y0-y2)+SQR(z0-z2));
    dmax = (d0>=d1&&d0>=d2)?d0:(d1>=d0&&d1>=d2)?d1:d2;

    numu = int(ceil(2*d0));
    numv = int(ceil(2*dmax));
      
    for (v=0;v<=numv;v++)
      {
        px0 = x0 + (x2-x0)*v/numv;
        py0 = y0 + (y2-y0)*v/numv;
        pz0 = z0 + (z2-z0)*v/numv;
        px1 = x1 + (x2-x1)*v/numv;
        py1 = y1 + (y2-y1)*v/numv;
        pz1 = z1 + (z2-z1)*v/numv;
        for (u=0;u<=numu;u++)
          {
            px = px0 + (px1-px0)*u/numu;
            py = py0 + (py1-py0)*u/numu;
            pz = pz0 + (pz1-pz0)*u/numu;

            myWorldToVoxel(mri,px,py,pz,&tx,&ty,&tz);
        
            imnr=(int)(tz+0.5);
            j=(int)(ty+0.5);
            i=(int)(tx+0.5);
            if (i>=0 && i<mri->width && 
                j>=0 && j<mri->height && 
                imnr>=0 && imnr<mri->depth)
              MRIvox(mri,i,j,imnr) = 1+(int)(val);
          }  
      }
  }

  //expand volume if values are greater than 1
  changed=1;
  while(changed){
    changed=0;

    for(k = 1 ; k< mri->depth-1 ; k++)
      for(j = 1 ; j < mri->height-1 ; j++)
        for(i = 1 ; i < mri->width-1 ; i++){
          new_val=MRIvox(mri,i,j,k)-1;
          if(new_val>0){
            if(MRIvox(mri,i-1,j,k)<new_val)
              {changed=1;MRIvox(mri,i-1,j,k)=new_val;}
            if(MRIvox(mri,i+1,j,k)<new_val)
              {changed=1;MRIvox(mri,i+1,j,k)=new_val;}
            if(MRIvox(mri,i,j-1,k)<new_val)
              {changed=1;MRIvox(mri,i,j-1,k)=new_val;}
            if(MRIvox(mri,i,j+1,k)<new_val)
              {changed=1;MRIvox(mri,i,j+1,k)=new_val;}
            if(MRIvox(mri,i,j,k-1)<new_val)
              {changed=1;MRIvox(mri,i,j,k-1)=new_val;}
            if(MRIvox(mri,i,j,k+1)<new_val)
              {changed=1;MRIvox(mri,i,j,k+1)=new_val;}
          }
        }
  }

  MRIwrite(mri,"./tmp4");

  //merge mri and mri_src
  for(k = 0 ; k< mri->depth ; k++)
    for(j = 0 ; j < mri->height ; j++)
      for(i = 0 ; i < mri->width ; i++)
        if(MRIvox(mri_src,i,j,k) || MRIvox(mri,i,j,k))
          MRIvox(mri,i,j,k)=MRIvox(mri_orig,i,j,k);

  MRIwrite(mri,"./tmp2");
  MRIwrite(mri_src,"./tmp3");
  fprintf(stderr,"done\n");
  return mri;
}


////////////////////////////////////////////////////////////////////////////
static void MRISFineSegmentation(MRI_variables *MRI_var)
{
  float x,y,z,sx,sy,sz,sd,sxn,syn,szn,sxt,syt,szt,nc;
  float force,force1,force3,force4;

  float d,dx,dy,dz,nx,ny,nz;
  VERTEX *v; 
  int iter,k,m,n;
  float samp_mean[4];
  float test_samp[4][9];
  int a,b;
  int it,jt,kt,h,niter;
  float r,F,E,rmin=3.33,rmax=10.;
  float decay=0.8,update=0.9;
  float fzero;
  float fmax; /*"dangerous" if artifact(s)*/

  float val,prev_val;
  int graditer=1;
  int MRIGRADIENT;

  
  MRIS *mris,*mrisphere;
  //  char surf_fname[500];

  double tx,ty,tz;
  double xw,yw,zw,xw1,yw1,zw1;
  double IntVal,GradVal;

  double ml;
  double lm,d10m[3],d10,f1m,f2m,f3m,f4m,dm,dbuff;  
  float ***dist;
  int nb_GM,nb_TR,nb_GTM;
  float cout,pcout=0,coutbuff,varbuff,mean_sd[10],mean_dist[10];
  float n1[3],n2[3];
 
  int csf,trn,grm;
  MRI *mri;
  int allocation;
  
  float coeff3,coeff4;

  double xCOG,yCOG,zCOG;
  double dCOG;


#if WRITE_SURFACES
  char fname[500];
  MRIS *mris_tmp;
#endif

  /*last 10 iterations -> converge with a sub-voxel accuracy
    to the closest gradient*/
  MRIGRADIENT=0;

  xCOG=yCOG=zCOG=0; /* to stop compilator warnings !*/ 

  mris=MRI_var->mris;
  mrisphere=MRI_var->mrisphere;

#if WRITE_SURFACES
  mris_tmp=MRISclone(mris);
#endif

  MRISsetNeighborhoodSize(mris, 2) ;
  MRIScomputeNormals(mris);

  fmax=MRI_var->WM_MAX + 25.0f ; /* being careful */
  //flo
  // do h = 0 -2 -1 1 2 3 in gotodarkestpoint


  dist = (float ***) malloc( mris->nvertices*sizeof(float**) );

  // if the atlas is not used
  allocation=0;
  if(MRI_var->validation || (MRI_var->atlas==0))
    mri=MRI_var->mri_src;
  else{
    allocation=1;
    mri=generateFinalMRI(MRI_var);
  }

  // if atlas is set
  if((MRI_var->validation==0) && MRI_var->atlas)
    {  
      coeff3=1;
      coeff4=1;
    }
  else if(MRI_var->validation && MRI_var->atlas)
    {
      coeff3=0.25;
      coeff4=0.25;
    }
  // atlas not set
  else
    {
      coeff3=0.;
      coeff4=0.;
    }

  for( it = 0; it < mris->nvertices; it++ ) 
    {
      dist[it] = (float**) malloc( 4*sizeof(float*) );
      for( jt = 0; jt < 4; jt++ ) 
        {
          dist[it][jt] = (float*) calloc( 3, sizeof(float));
        }  
    }

  // force coefficients
  E=(1/rmin+1/rmax)/2;
  F=6/(1/rmin-1/rmax);  

  fzero=MRI_var->CSF_intensity;

  for(k=0;k<mris->nvertices;k++)
    for (m=0;m<4;m++)
      for (n=0;n<3;n++)
        dist[k][m][n]=0;

  for (n=0;n<10;n++)
    {
      mean_sd[n]=0;
      mean_dist[n]=0;
    }

  niter =graditer;
  force = 0.0f ; 
  pcout=0;

  for (k=0;k<mris->nvertices;k++)
    {
      v = &mris->vertices[k];
      v->odx = 0;
      v->ody = 0;
      v->odz = 0;
    }
  
  ml=2;
  // iterations
  for (iter=0;niter;iter++)
    {
      cout = lm = d10 = f1m = f2m = f3m = f4m = dm = 0;
      xCOG = yCOG = zCOG = 0;

#if WRITE_SURFACES
      sprintf(fname,"./rh.finedeformation%d",iter);
      MRISwrite(mris,fname);
#endif

      for (k=0;k<mris->nvertices;k++)
        {
          v = &mris->vertices[k];
          v->tx = v->x;
          v->ty = v->y;
          v->tz = v->z;
          xCOG+=v->x;
          yCOG+=v->y;
          zCOG+=v->z;
        }
      // get the center of gravity of surfaces
      xCOG/=mris->nvertices;
      yCOG/=mris->nvertices;
      zCOG/=mris->nvertices;

      // compute surface properties
      MRIScomputeMetricProperties(mris) ;
      MRIScomputeNormals(mris);
      MRIScomputeSecondFundamentalForm(mris) ;
      MRISuseMeanCurvature(mris) ;
      MRISaverageCurvatures(mris, 20) ;
    
      //brain atlas force
      for (k=0;k<mris->nvertices;k++)
        {   
          /*distance to Centroid force*/
          dCOG=sqrt(SQR(mris->vertices[k].tx-xCOG)
                    +SQR(mris->vertices[k].ty-yCOG)+
                    SQR(mris->vertices[k].tz-zCOG));

          // coeff3
          force3=coeff3*0.25*(MRI_var->mris_dCOG->vertices[k].curv-dCOG)/
            MRI_var->mris_var_dCOG->vertices[k].curv;
      
          mris->vertices[k].val=force3;

#if WRITE_SURFACES
          mris_tmp->vertices[k].curv=
            (MRI_var->mris_dCOG->vertices[k].curv-dCOG)/
            MRI_var->mris_var_dCOG->vertices[k].curv;
#endif
 
          f3m+=force3;

          // coeff4
          force4 = 
            -coeff4*0.0002*
            (MRI_var->mris_curv->vertices[k].curv-mris->vertices[k].curv)
            /MRI_var->mris_var_curv->vertices[k].curv;
      
          mris->vertices[k].val2=force4;
  
          f4m+=force4;
        }
      MRISaverageVals(mris,2);
  
#if WRITE_SURFACES
      sprintf(fname,"./rh.d_err%d",iter);
      MRISwriteCurvature(mris_tmp,fname);
#endif
  
      for (k=0;k<mris->nvertices;k++)
        {
          csf=int(mrisphere->vertices[k].x);
          trn=int(mrisphere->vertices[k].y);
          grm=int(mrisphere->vertices[k].z);
      
          v = &mris->vertices[k];
          // get the vertex coords and normal
          x = v->tx;
          y = v->ty;
          z = v->tz;
          nx = v->nx;
          ny = v->ny;
          nz = v->nz;
          sx=sy=sz=sd=0;
          n=0;
          for (m=0;m<v->vnum;m++)
            {
              sx += dx =mris->vertices[v->v[m]].tx - x;
              sy += dy =mris->vertices[v->v[m]].ty - y;
              sz += dz =mris->vertices[v->v[m]].tz - z;
              sd += sqrt(dx*dx+dy*dy+dz*dz);
              n++;
            }
          // mean distance to the neighbors
          sx = sx/n;
          sy = sy/n;
          sz = sz/n;
          sd = sd/n;
      
          lm+=sd;

          nc = sx*nx+sy*ny+sz*nz;
          // normal component of the mean distance vector
          sxn = nc*nx;
          syn = nc*ny;
          szn = nc*nz;
          // tangential component of the mean distance vector
          sxt=sx-sxn;
          syt=sy-syn;
          szt=sz-szn;

          v->nc=nc;


          // force calculations starts here
          ///////////////////////////////////////////////////////
          force1=0;
          if (nc)
            {
              r= (nc>0) ? nc : -nc;
              r=SQR(sd)/(2*r);
              force1=(1+tanh(F*(1/r-E)))/2;
            }
          else
            Error("\n Problem with normal component being 0");
                        
                        
                        
          /******************************/
          if(!MRIGRADIENT)
            {
              find_normal(nx,ny,nz,n1,n2,MRI_var->direction);
              for (h=0;h<4;h++)
                for (a=-1;a<2;a++)
                  for (b=-1;b<2;b++)
                    {
                      myWorldToVoxel(MRI_var->mri_orig,
                                     (x-nx*h+n1[0]*a+n2[0]*b),
                                     (y-ny*h+n1[1]*a+n2[1]*b),
                                     (z-nz*h+n1[2]*a+n2[2]*b),&tx,&ty,&tz);
                      kt=(int)(tz+0.5);
                      jt=(int)(ty+0.5);
                      it=(int)(tx+0.5);
                                                                
                      if ((kt<0||kt>=MRI_var->depth||
                           it<0||it>=MRI_var->width||
                           jt<0||jt>=MRI_var->height))
                        val=0;
                      else
                        val=MRIvox(mri,it,jt,kt);
                                                                
                      test_samp[h][3*b+a+4] = val;
                    }
                                
              val=test_samp[0][4];
                                
              force=0.0f;
              if (!val)     /*|| val>fmax)*/
                force=-0.25;
              else if (val<=csf)
                force=-0.1;
              else if (val<trn)
                force=0.0;
              else if(val>fmax)
                force=-0.1;
              else 
                {      
                  for(h=1;h<10;h++)
                    {
                      prev_val=val;
                      myWorldToVoxel(MRI_var->mri_orig,
                                     (x-nx*h),(y-ny*h),(z-nz*h),&tx,&ty,&tz);
                      kt=(int)(tz+0.5);
                      jt=(int)(ty+0.5);
                      it=(int)(tx+0.5);
                                                                
                      if ((kt<0||kt>=MRI_var->depth||
                           it<0||it>=MRI_var->width||
                           jt<0||jt>=MRI_var->height))
                        val=0;
                      else 
                        val=MRIvox(mri,it,jt,kt);
                                                                
                      if(MRI_var->validation==0)
                        {
                          if(val<trn && prev_val<trn)
                            {
                              force=-0.1;
                              break;
                            }
                        }
                      if(val>fmax && prev_val>fmax)
                        {
                          force=-0.2;
                          break;
                        }
                      prev_val=val;
                    }
                  if(force==0.0f)
                    {
                      mean(test_samp,samp_mean);
                                                                
                      if (samp_mean[1]<trn  &&  samp_mean[2]<trn)
                        {
                          if (samp_mean[0]*100>samp_mean[1]*90)
                            if (samp_mean[1]*100>samp_mean[2]*90)
                              force=-0.1;
                        }
                      else
                        {
                          nb_GM=0;
                          nb_TR=0;
                          nb_GTM=0;
                          for (h=0;h<4;h++)
                            {
                              if (samp_mean[h]>=grm)
                                nb_GM++;
                              if (samp_mean[h]<trn)
                                nb_TR++;
                            }
                          if (nb_TR>=3)
                            force=-0.2;
                          else if (nb_GM>=3 && samp_mean[0]>trn)
                            force=0.7;
                          else if (nb_GM==2 && samp_mean[0]>trn)
                            force=0.5; 
                          else if (nb_TR==0)
                            force=0.3;
                          else
                            {
                              nb_GM=0;
                              nb_TR=0;
                              for (h=0;h<4;h++)
                                {
                                  for (a=0;a<9;a++)
                                    {
                                      if (test_samp[h][a]>=grm)
                                        nb_GM++;
                                      else if (test_samp[h][a]<trn)
                                        nb_TR++;
                                      else
                                        nb_GTM++;
                                    }
                                }
                                                                
                              if (nb_TR>=18)
                                force=-0.3;
                              else if (nb_GM>=18)
                                force=0.5;
                              else if (nb_GM>=15)
                                force=0.3;
                              else
                                {
                                  if (nb_GM>9 && nb_TR<9)
                                    force=0.5;
                                  else if (nb_GTM>30)
                                    force=0.15;
                                  else
                                    force=-0.0;
                                }
                            }
                        }
                    }
                }
            }
          else
            {
              /*determine the normal direction in Voxel coordinates*/
              x = v->x ; y = v->y ; z = v->z ;
              myWorldToVoxel(mri, x, y, z, &xw, &yw, &zw) ;
              x = v->x + v->nx ; y = v->y + v->ny ; z = v->z + v->nz ;
              myWorldToVoxel(mri, x, y, z, &xw1, &yw1, &zw1) ;
              nx = xw1 - xw ; ny = yw1 - yw ; nz = zw1 - zw ; 
                                        
              /*calculate local values*/
              myWorldToVoxel(mri, x, y, z, &xw, &yw, &zw) ;
              MRIsampleVolume(mri, xw, yw, zw, &IntVal) ;
              MRIsampleVolumeDerivativeScale(mri, xw, yw, zw, 
                                             nx, ny,nz,&GradVal,1.);
                                        
              force=0.020*(trn-IntVal)*GradVal;
            }

          /*brainatlas force*/
          force3=v->val;
          force4=v->val2;
          force1=force1;

          v->curv=force; //test

          f1m+=force1;
          f2m+=force;
          ////////////////////////////////////////////////////////////////

          dx = sxt*0.8 + force1*sxn+v->nx*force + v->nx*force3 + v->nx*force4;
          dy = syt*0.8 + force1*syn+v->ny*force + v->ny*force3 + v->ny*force4;
          dz = szt*0.8 + force1*szn+v->nz*force + v->nz*force3 + v->nz*force4;
      
          dx = decay*v->odx+update*dx;
          dy = decay*v->ody+update*dy;
          dz = decay*v->odz+update*dz;
      
          if ((d=sqrt(dx*dx+dy*dy+dz*dz))>1.0)
            {
              dx /= d;
              dy /= d;
              dz /= d;
            }
      
          v->odx = dx;
          v->ody = dy;
          v->odz = dz;

          d=sqrt(dx*dx+dy*dy+dz*dz);
      
          dm+=d;

          dist[k][iter%4][0]=x;
          dist[k][iter%4][1]=y;
          dist[k][iter%4][2]=z;

          d10m[0] = d10m[1] = d10m[2] = 0;

          for(n=0;n<4;n++)
            {
              d10m[0]+=dist[k][n][0]/4;
              d10m[1]+=dist[k][n][1]/4;
              d10m[2]+=dist[k][n][2]/4;
            }
    
          dbuff=0;
          for(n=0;n<4;n++)
            dbuff+=SQR(dist[k][n][0]-d10m[0])+SQR(dist[k][n][1]-d10m[1])+
              SQR(dist[k][n][2]-d10m[2]);

          d10+=dbuff/4;

          v->x += dx;
          v->y += dy;
          v->z += dz;
        }

      lm /=mris->nvertices;
      f1m /=mris->nvertices;
      f2m /=mris->nvertices;
      f3m /=mris->nvertices;
      f4m/=mris->nvertices;
      dm /=mris->nvertices;
      d10 /=mris->nvertices;


      ml=lm;

      mean_sd[iter%10]=lm;
      mean_dist[iter%10]=d10;

      coutbuff=0;
      for(n=0;n<10;n++)
        coutbuff+=mean_sd[n]/10;
    
      varbuff=0;
      for(n=0;n<10;n++)
        varbuff+=SQR(mean_sd[n]-coutbuff);

      cout=varbuff;

      coutbuff=0;
      for(n=0;n<10;n++)
        coutbuff+=mean_dist[n]/10;

      varbuff=0;
      for(n=0;n<10;n++)
        varbuff+=SQR(mean_dist[n]-coutbuff);

      cout+=10*varbuff;

      coutbuff=cout;

      cout=(cout+pcout)/2;

      pcout=coutbuff;
    

      /*    if (!(iter % 1))
            {      
            fprintf(stderr,
            "\n%d: lm=%5.3f,f1m=%5.3f,f2m=%5.3f,f3m=%5.3f,
            f4m=%5.3f,dm=%5.3f,d10m=%5.3f,c=%5.3f"
            ,iter,lm,f1m,f2m,f3m,f4m,dm,d10,100*cout);
            }*/
      if(MRI_var->atlas && iter && ((iter%10)==0))
        {
          if(iter==30 && MRI_var->validation==0)
            {
              fprintf(stderr,"\n      The shape of the surface was incorrect,"
                      "\n      hence we rigidly realign the "
                      "surface with the template");
              MRI_var->validation=ValidationSurfaceShape(MRI_var);
              /*the sphere surface was freed and realocated*/
              mrisphere=MRI_var->mrisphere;
            }
          if(VERBOSE_MODE)
            fprintf(stderr,
                    "\nScaling of atlas fields onto current surface fields");
          MRISsaveVertexPositions(mris, ORIGINAL_VERTICES) ;
          MRISscaleFields(mris,MRI_var->mris_dCOG,
			  MRI_var->mris_var_dCOG,DIST_MODE);
          MRISscaleFields(mris,MRI_var->mris_curv,
                          MRI_var->mris_var_curv,CURV_MODE);
          if(VERBOSE_MODE)
            fprintf(stderr,"\nCompute Local Values csf/gray");  
          MRISComputeLocalValues(MRI_var);
        }
      if (niter==graditer)
        {
          if(((iter>10)&&(10000*cout<1))||(iter>100))
            // if(((iter>10)&&(10000*cout<.01))||(iter>100))
            { 
              niter--;
              MRIGRADIENT=1;
            }
        }
      else
        niter--;
      // printf("iter = %d, cout = %.6f\n", iter, cout);
      // if (iter%10==0)
      //   printf("brainsize = %d\n", calcBrainSize(mri, mris));

    }
  fprintf(stderr,"%d iterations\n",iter);

#if WRITE_SURFACES
  sprintf(fname,"./rh.finedeformation%d",iter);
  MRISwrite(mris,fname);
#endif

  MRIScomputeNormals(mris);

#if WRITE_SURFACES
  MRISfree(&mris_tmp);
#endif

  if(allocation)
    MRIfree(&mri);


  /*free memory*/
  for( it = 0; it < mris->nvertices; it++ ) 
    {
      for( jt = 0; jt < 4; jt++ ) 
        free(dist[it][jt]);   
      free(dist[it]);
    }
  free(dist);
  
}

static void MRISgoToClosestDarkestPoint(MRI_variables *MRI_var)
{
  float x,y,z,sx,sy,sz,sd,sxn,syn,szn,sxt,syt,szt,nc;
  float force,force1;

  float d,dx,dy,dz,nx,ny,nz;
  VERTEX *v; 
  int iter,k,m,n;

  int h;
  float decay=0.8,update=0.9;

  double val,dist,min_val,distance;
  
  MRIS *mris;
  double xw,yw,zw;

  MRI *mri;

  mris=MRI_var->mris;

  MRISsetNeighborhoodSize(mris, 1) ; // 1-connected neighbors only

  MRIScomputeNormals(mris);

  mri=MRI_var->mri_orig;

  for (k=0;k<mris->nvertices;k++)
    {
      v = &mris->vertices[k];
      v->odx = 0;
      v->ody = 0;
      v->odz = 0;
    }

  int niter = MRI_var->dark_iter;
  for (iter=0;niter;iter++)
    {
      /////////////////////////////////////////////////////////////////////
      // go through all the vertices and get the averaged shortest distance
      // to the darkest point 
      // compute normal direction
      double mdist = 0.;
      MRIScomputeNormals(mris);
      for (k=0;k<mris->nvertices;k++)
        {
          v = &mris->vertices[k];
          v->tx = v->x;
          v->ty = v->y;
          v->tz = v->z;
          nx = v->nx;
          ny = v->ny;
          nz = v->nz;

          /*find the minimum grey value in the normal direction*/
          // and find the distance to that voxel
          min_val=10000.;
          distance=0;
          for(h=-2;h<3;h++)
            //for (h=2; h>-3; h--)
            {
              dist=0.5*(float)h;  // -1.0, -0.5, 0., 0.5, 1.0
              x=v->x + dist*nx;
              y=v->y + dist*ny;
              z=v->z + dist*nz;
              myWorldToVoxel(mri, x, y, z, &xw, &yw, &zw) ;
              // if outside, then
              if((zw<0||zw>=MRI_var->depth||
                  xw<0||xw>=MRI_var->width||
                  yw<0||yw>=MRI_var->height))
                val=1000.0;
              else // sample the volume (tri-linear interpolation
                MRIsampleVolume(mri, xw, yw, zw, &val) ;
              // the following may have probelms in the following way
              // if the nearby voxels are uniform grey, 
              // then the min_val is set to
              // that color and distance is -1 (i.e. push-in farther).
              // can be avoided by going the other way, i.e. h=1 to -1
              if(val<min_val) /* don't move if not sure */ 
                {
                  min_val=val;
                  distance=dist;  
                }
            }
          if(min_val>=1000.0)
            distance=0; 
          v->val=distance; // save the shortest distance 
          // (only vlaues -1 -.5, 0, .5, 1.)
          mdist += distance;
        }
      // printf("mean distance to the darkest point = 
      // %.4f\n", mdist/mris->nvertices);
      // if (iter%10==0)
      //   printf("brainsize = %d\n", calcBrainSize(mri, mris)); 

      // the distance to have the neighboring average (iterated twice)
      MRISaverageVals(mris,2);
      //////////////////////////////////////////////////////////////////////
      for (k=0;k<mris->nvertices;k++)
        {
          v = &mris->vertices[k];
          x = v->tx;
          y = v->ty;
          z = v->tz;
          nx = v->nx;
          ny = v->ny;
          nz = v->nz;
          sx=sy=sz=sd=0;
          n=0;
          for (m=0;m<v->vnum;m++)
            {
              sx += dx =mris->vertices[v->v[m]].tx - x;
              sy += dy =mris->vertices[v->v[m]].ty - y;
              sz += dz =mris->vertices[v->v[m]].tz - z;
              sd += sqrt(dx*dx+dy*dy+dz*dz);
              n++;
            }
          sx = sx/n;
          sy = sy/n;
          sz = sz/n;
          sd = sd/n;
      
          nc = sx*nx+sy*ny+sz*nz;
      
          sxn = nc*nx;
          syn = nc*ny;
          szn = nc*nz;
          sxt=sx-sxn;
          syt=sy-syn;
          szt=sz-szn;

          v->nc=nc;

          force1=0.5;

          force=v->val; // darkest inside < 0, darkest outside > 0
          // force > 0 push out, force < 0 push in
          // bias toward push-in
          if(force<0)  
            force=MAX(-0.5,force); // this means -1.0 or -.5
          else
            force=MIN(0.5,force);  // this means 0. or .5

          force1=force1;
          dx = sxt*0.8 + sxn * force1 + v->nx*force;
          dy = syt*0.8 + syn * force1 + v->ny*force;
          dz = szt*0.8 + szn * force1 + v->nz*force;
      
          dx = decay*v->odx+update*dx;
          dy = decay*v->ody+update*dy;
          dz = decay*v->odz+update*dz;
      
          if ((d=sqrt(dx*dx+dy*dy+dz*dz))>1.0)
            {
              dx /= d;
              dy /= d;
              dz /= d;
            }
      
          v->odx = dx;
          v->ody = dy;
          v->odz = dz;

          d=sqrt(dx*dx+dy*dy+dz*dz);
      
          v->x += dx;
          v->y += dy;
          v->z += dz;
        }
      niter--;
    }
  // note that this has no criteria of convergence niter=10 only
  MRIScomputeNormals(mris);
}

#ifndef __OPTIMIZE__
static int calcBrainSize(const MRI* mri_src, const MRIS *mris)
{
  int i,j,k,imnr; 
  double x0,y0,z0,x1,y1,z1,x2,y2,z2,d0,d1,d2,dmax,u,v;
  double px,py,pz,px0,py0,pz0,px1,py1,pz1;
  int numu,numv,totalfilled,newfilled;
  double tx,ty,tz;
  unsigned long brainsize;

  int width, height,depth;
  MRI *mri_buff;

  width=mri_src->width;
  height=mri_src->height;
  depth=mri_src->depth;

  mri_buff= MRIalloc(width, height, depth, MRI_UCHAR) ;

  for (k=0;k<mris->nfaces;k++)
    {
      // get three vertices
      x0 =mris->vertices[mris->faces[k].v[0]].x;    
      y0 =mris->vertices[mris->faces[k].v[0]].y;    
      z0 =mris->vertices[mris->faces[k].v[0]].z;    
      x1 =mris->vertices[mris->faces[k].v[1]].x;    
      y1 =mris->vertices[mris->faces[k].v[1]].y;    
      z1 =mris->vertices[mris->faces[k].v[1]].z;    
      x2 =mris->vertices[mris->faces[k].v[2]].x;    
      y2 =mris->vertices[mris->faces[k].v[2]].y;    
      z2 =mris->vertices[mris->faces[k].v[2]].z;
      // calculate sides
      d0 = sqrt(SQR(x1-x0)+SQR(y1-y0)+SQR(z1-z0));
      d1 = sqrt(SQR(x2-x1)+SQR(y2-y1)+SQR(z2-z1));
      d2 = sqrt(SQR(x0-x2)+SQR(y0-y2)+SQR(z0-z2));
      dmax = (d0>=d1&&d0>=d2)?d0:(d1>=d0&&d1>=d2)?d1:d2;
      numu = int(ceil(2*d0));
      numv = int(ceil(2*dmax));
      
      for (v=0;v<=numv;v++)
        {
          // px0 spans x0 to x2
          px0 = x0 + (x2-x0)*v/numv;
          py0 = y0 + (y2-y0)*v/numv;
          pz0 = z0 + (z2-z0)*v/numv;
          // px1 spans x1 to x2
          px1 = x1 + (x2-x1)*v/numv;
          py1 = y1 + (y2-y1)*v/numv;
          pz1 = z1 + (z2-z1)*v/numv;
          for (u=0;u<=numu;u++)
            {
              // px spans px0 to px1
              px = px0 + (px1-px0)*u/numu;
              py = py0 + (py1-py0)*u/numu;
              pz = pz0 + (pz1-pz0)*u/numu;

              // calculate voxel value of a point in the triangle
              // C function don't know about const
              myWorldToVoxel(const_cast<MRI *> (mri_src),px,py,pz,&tx,&ty,&tz);
        
              imnr=(int)(tz+0.5);
              j=(int)(ty+0.5);
              i=(int)(tx+0.5);
              // if within the voxel mark it as brain
              if (i>=0 && i<width && j>=0 && j<height && imnr>=0 && imnr<depth)
                MRIvox(mri_buff,i,j,imnr) = 255;
            }  
        }
    }

  MRIvox(mri_buff,1,1,1)= 64;
  totalfilled = newfilled = 1;
  while (newfilled>0)
    {
      newfilled = 0;
      for (k=0;k<depth;k++)
        for (j=0;j<height;j++)
          for (i=0;i<width;i++)
            if (MRIvox(mri_buff,i,j,k)==0)
              if (MRIvox(mri_buff,i,j,mri_buff->zi[k-1])==
                  64||MRIvox(mri_buff,i,mri_buff->yi[j-1],k)==64||
                  MRIvox(mri_buff,mri_buff->xi[i-1],j,k)==64)
                {
                  MRIvox(mri_buff,i,j,k)= 64;
                  newfilled++;
                }
      for (k=depth-1;k>=0;k--)
        for (j=height-1;j>=0;j--)
          for (i=width-1;i>=0;i--)
            if (MRIvox(mri_buff,i,j,k)==0)
              if (MRIvox(mri_buff,i,j,mri_buff->zi[k+1])==
                  64||MRIvox(mri_buff,i,mri_buff->yi[j+1],k)==64||
                  MRIvox(mri_buff,mri_buff->xi[i+1],j,k)==64)
                {
                  MRIvox(mri_buff,i,j,k) = 64;
                  newfilled++;
                }
      totalfilled += newfilled;
    }
  // fill all surface boundary voxels to be 64 (there are 6 faces)
  for (k=0; k < depth;k++)
    for (j=0; j < height; j++)
      {
        MRIvox(mri_buff,       0, j, k ) = 64;
        MRIvox(mri_buff, width-1, j, k ) = 64;
      }
  
  for (k=0; k < depth;k++)
    for (i=0; i < width ; i++)
      {
        MRIvox(mri_buff, i,        0, k ) = 64;
        MRIvox(mri_buff, i, height-1, k ) = 64;
      }

  for (i=0; i < width ;i++)
    for (j=0; j < height; j++)
      {
        MRIvox(mri_buff, i, j,      0 ) = 64;
        MRIvox(mri_buff, i, j, depth-1) = 64;
      }
  brainsize=0;
  for (k=0;k<depth;k++)
    for (j=0;j<height;j++)
      for (i=0;i<width;i++)
        {
          if (MRIvox(mri_buff,i,j,k)!=64)
            brainsize++;
        }
  MRIfree(&mri_buff);
  return brainsize;
}
#endif

void calcForce1(double &fST, double &fSN, double &fN, 
                const double &x, const double &y, const double &z,
                const double &sx, const double &sy, const double &sz, 
                const double &sd, 
                const double &nx, const double &ny, const double &nz,
                MRI_variables *mri_var)
{
  double tx, ty, tz;
  double force2, force3;
  int ninside=15,noutside=10;
  int h;
  int it,jt,kt;

  // neighbor average tangential force
  fST = 0.8;
  ////////////////////////////////////////////////////////////////
  // Sn force coefficient  : fSN
  fSN= 0.7;
  
  ////////////////////////////////////////////////////////////////
  // Vn force calculation  : force
  force2=-1;
  for (h=-noutside;h<0;h++) // up to 15 voxels inside 
    {
      // look at outside side voxels (h < 0) of the current position
      myWorldToVoxel(mri_var->mri_src,(x-nx*h),
                     (y-ny*h),(z-nz*h),&tx,&ty,&tz);
      kt=(int)(tz+0.5);
      jt=(int)(ty+0.5);
      it=(int)(tx+0.5);
      // if inside the bounding box
      if (!(kt<0||kt>=mri_var->depth||
            it<0||it>=mri_var->width||
            jt<0||jt>=mri_var->height))
        // no need to push in
        if (mri_var->Basin[kt][jt][it].type)
          force2=0;
    }
  
  ///////////////////////////////////////////////////////////////
  force3 = 1;
  for (h=1;h<ninside;h++) // 10 voxels outside
    {
      // look at inside voxes (h > 0) of the current position
      myWorldToVoxel(mri_var->mri_src,
                     (x-nx*h),(y-ny*h),(z-nz*h),&tx,&ty,&tz);
      kt=(int)(tz+0.5);
      jt=(int)(ty+0.5);
      it=(int)(tx+0.5);
      // if outside of the bounding box, then force3 = 0
      if (kt<0||kt>=mri_var->depth||
          it<0||it>=mri_var->width||
          jt<0||jt>=mri_var->height)
        force3 = 0;
      // if it is the voxel, push out force3 != 0.  If not, force3 = 0
      else if (!mri_var->Basin[kt][jt][it].type)
        force3=0;
    }
  ///////////////////////////////////////////////////////////////
  // force2 = -1 or 0 (outside region : push-in), 
  // force3 = 1 or 0 (inside-region: push-out)
  // relative strength is heuristic
  fN = 0.2*force2+1.0*(force3-0.1);   // if >0 push-out, <0 push-in
 
}

void calcForce2(double &force0, double &force1, double &force, 
                const double &x, const double &y, const double &z,
                const double &sx, const double &sy, const double &sz, 
                const double &sd,
                const double &nx, const double &ny, const double &nz,
                MRI_variables *MRI_var)
{
  int h;
  int it, kt, jt;
  int a, b;
  float samp_mean[4];
  float test_samp[4][9];
  double tx, ty, tz;
  double val = 0.;
  double r,F,E,rmin=3.33,rmax=10.;
  float n1[3],n2[3];

  ///////////////////////
  force0 = .8;

  E=(1/rmin+1/rmax)/2;
  F=6/(1/rmin-1/rmax);  

  // see S.M.Smith, BET, www.fmrib.ox.ac.uk.
  force1=0;

  double nc = sx*nx+sy*ny+sz*nz; // S.N
  if (nc)
    {
      r= (nc>0) ? nc : -nc;
      r=SQR(sd)/(2*r);
      force1=(1+tanh(F*(1/r-E)))/2;
    }
  else
    Error("\n Problem with the normal component being zero");
  

  // image dependent force calculation
  ///////////////////////////////////////////////////////////////////
  find_normal(nx,ny,nz,n1,n2,MRI_var->direction);
  
  // look at 3x3x4 volume
  for (h=0;h<4;h++)
    for (a=-1;a<2;a++)
      for (b=-1;b<2;b++)
        {
          myWorldToVoxel(MRI_var->mri_orig,(x-nx*h+n1[0]*a+n2[0]*b),
                         (y-ny*h+n1[1]*a+n2[1]*b),
                         (z-nz*h+n1[2]*a+n2[2]*b),&tx,&ty,&tz);
          kt=(int)(tz+0.5);
          jt=(int)(ty+0.5);
          it=(int)(tx+0.5);
        
          // outside the bounding box
          if ((kt<0||kt>=MRI_var->depth||
               it<0||it>=MRI_var->width||
               jt<0||jt>=MRI_var->height))
            val=0;
          else
            val=MRIvox(MRI_var->mri_src,it,jt,kt);
        
          test_samp[h][3*b+a+4] = val;
        }
  
  val=test_samp[0][4];

  force = 0.; // initialize

  if (!val)     /*|| val>fmax)*/
    force=-0.25;
  else if (val<=MRI_var->CSF_MAX)
    force=-0.1;
  else if (val<MRI_var->TRANSITION_intensity)
    force=0.0;
  else 
    {        
      mean(test_samp,samp_mean);
    
      if (samp_mean[1]<MRI_var->TRANSITION_intensity && 
          samp_mean[2]<MRI_var->TRANSITION_intensity)
        {
          if (samp_mean[0]*100>samp_mean[1]*90)
            if (samp_mean[1]*100>samp_mean[2]*90)
              force=-0.1;
        }
      else
        {
          int nb_GM=0;  // number of grey matter voxels
          int nb_TR=0;  // number of transition voxels
          int nb_GTM=0; // none of the above
          for (h=0;h<4;h++)
            {
              if (samp_mean[h]>=MRI_var->GM_intensity)
                nb_GM++;
              if (samp_mean[h]<MRI_var->TRANSITION_intensity)
                nb_TR++;
            }
      
          if (nb_TR>=3)
            force=-0.2;
          else if (nb_GM>=3 && samp_mean[0]>MRI_var->TRANSITION_intensity)
            force=0.7;
          else if (nb_GM==2 && samp_mean[0]>MRI_var->TRANSITION_intensity)
            force=0.5; 
          else if (nb_TR==0)
            force=0.3;
          else
            {
              nb_GM=0;
              nb_TR=0;
              for (h=0;h<4;h++)
                {
                  for (a=0;a<9;a++)
                    {
                      if (test_samp[h][a]>=MRI_var->GM_intensity)
                        nb_GM++;
                      else if (test_samp[h][a]<MRI_var->TRANSITION_intensity)
                        nb_TR++;
                      else
                        nb_GTM++;
                    }
                }
              // set the force depending on the voxel types
              if (nb_TR>=18)
                force=-0.3;
              else if (nb_GM>=18)
                force=0.5;
              else if (nb_GM>=15)
                force=0.3;
              else
                {
                  if (nb_GM>9 && nb_TR<9)
                    force=0.5;
                  else if (nb_GTM>30)
                    force=0.1;
                  else
                    force=-0.0;
                }
            }
        }
    }
  // heuristic
  force += tanh(nc*0.1);
}

static void FitShape(MRI_variables *MRI_var, 
                     const int convLimit, const int maxIter,
                     void (*calcForce)
                     (double &force0, double &force1, double &force, 
                      const double &x, const double &y, const double &z,
                      const double &sx, const double &sy, const double &sz, 
                      const double &sd,
                      const double &nx, const double &ny, const double &nz,
                      MRI_variables *mri_var)
                     )
{
  float x,y,z,sx,sy,sz,sd,sxn,syn,szn,sxt,syt,szt,nc;
  double fN,fST,fSN;
  float d,dx,dy,dz,nx,ny,nz;
  VERTEX *v;
  int iter,k,m,n;

  int it,jt, niter;

  float decay=0.8,update=0.9;
  
  int int_smooth=10;

  MRIS *mris;
  //  char surf_fname[500];


#if WRITE_SURFACES
  char fname[500];
#endif

  double lm,d10m[3],d10,f1m,f2m,dm,dbuff;  
  float ***dist;
  float cout,cout_prec,coutbuff,varbuff,mean_sd[10],mean_dist[10];


  mris=MRI_var->mris;
  MRIScomputeNormals(mris);

  //////////////////////////////////////////////////////////////
  // initialize vars
  dist = (float ***) malloc( mris->nvertices*sizeof(float**) );

  for( it = 0; it < mris->nvertices; it++ ) 
    {
      dist[it] = (float**) malloc( 4*sizeof(float*) );
      for( jt = 0; jt < 4; jt++ ) 
        {
          dist[it][jt] = (float*) calloc( 3, sizeof(float));
        }  
    }

  for(k=0;k<mris->nvertices;k++)
    for (m=0;m<4;m++)
      for (n=0;n<3;n++)
        dist[k][m][n]=0;

  for (n=0;n<10;n++)
    {
      mean_sd[n]=0;
      mean_dist[n]=0;
    }

  niter =int_smooth;
  fN = 0.0f ; 

  cout_prec = 0;

  /* momentum -> 0*/
  for (k=0;k<mris->nvertices;k++)
    {
      v = &mris->vertices[k];
      v->odx = 0;
      v->ody = 0;
      v->odz = 0;
    }

  ///////////////////////////////////////////////////////////////
  // iterations
  for (iter=0;niter;iter++)
    {
      lm = d10 = f1m = f2m = dm = 0;
      for (k=0;k<mris->nvertices;k++)
        {
          v = &mris->vertices[k];
          v->tx = v->x;  // initialize t(mp)
          v->ty = v->y;
          v->tz = v->z;
        }

#if WRITE_SURFACES
      sprintf(fname,"./rh.fitshape%d",iter);
      MRISwrite(mris,fname);
#endif
                

      for (k=0;k<mris->nvertices;k++)
        {
          v = &mris->vertices[k];
          // vertex position
          x = v->tx;
          y = v->ty;
          z = v->tz;
          // normal vector
          nx = v->nx;
          ny = v->ny;
          nz = v->nz;
          sx=sy=sz=sd=0;
          n=0;
          // get the mean position of neighboring vertices
          // try to minimize
          for (m=0;m<v->vnum;m++)
            {
              sx += dx =mris->vertices[v->v[m]].tx - x;
              sy += dy =mris->vertices[v->v[m]].ty - y;
              sz += dz =mris->vertices[v->v[m]].tz - z;
              sd += sqrt(dx*dx+dy*dy+dz*dz);
              n++;
            }
          // S=(sx,sy,sz)  points to the mean position from 
          // this particular vertex
          sx = sx/n;
          sy = sy/n;
          sz = sz/n;
          // mean distance
          sd = sd/n;
      
          // cache
          lm+=sd;

          // inner product of S and N
          nc = sx*nx+sy*ny+sz*nz;
          // Normal component of S
          sxn = nc*nx;
          syn = nc*ny;
          szn = nc*nz;
          // Tangential component of S
          sxt=sx-sxn;
          syt=sy-syn;
          szt=sz-szn;

          v->nc=nc;

          fST = 0.;
          fSN = 0.;
          fN = 0.;

          // force calculation
          calcForce(fST,fSN,fN, x,y,z, sx,sy,sz,sd, nx,ny,nz, MRI_var);

          f1m+=fSN;
          f2m+=fN;

          ///////////////////////////////////////////////////////////////
          // keep tangential vector smaller < 1.0
          if ((d=sqrt(sxt*sxt+syt*syt+szt*szt))>1.0)
            {
              sxt /= d;
              syt /= d;
              szt /= d;
            }
          //////////////////////////////////////////////////////////////
          // move delta 
          //  Delta = fST * St   (+) fSN * Sn (+) fN * Vn
          //          move within    smoothness      surface selection
          //////////////////////////////////////////////////////////////
          dx = fST*sxt + fSN*sxn + fN*v->nx;
          dy = fST*syt + fSN*syn + fN*v->ny;
          dz = fST*szt + fSN*szn + fN*v->nz;
      
          // v->odx (last cached value) 
          // decay = .8, update = .9
          // combining previous values and new values
          // so that no drastic variation from one iteration to the other
          dx = decay*v->odx + update*dx;
          dy = decay*v->ody + update*dy;
          dz = decay*v->odz + update*dz;
     
          // if too big, make it small < 1.0
          if ((d=sqrt(dx*dx+dy*dy+dz*dz))>1.0)
            {
              dx /= d;
              dy /= d;
              dz /= d;
            }
          // cache the value
          v->odx = dx;
          v->ody = dy;
          v->odz = dz;

          // calculate the size of the movement
          d=sqrt(dx*dx+dy*dy+dz*dz);
      
          dm+=d;

          /////////////////////////////////////////////
          dist[k][iter%4][0]=x;
          dist[k][iter%4][1]=y;
          dist[k][iter%4][2]=z;

          d10m[0] = d10m[1] = d10m[2] = 0;

          for(n=0;n<4;n++) // getting the past 4 average vertex position
            {
              d10m[0] +=dist[k][n][0]/4;
              d10m[1] +=dist[k][n][1]/4;
              d10m[2] +=dist[k][n][2]/4;
            }
    
          dbuff=0;
          for(n=0;n<4;n++)
            dbuff+=
              SQR(dist[k][n][0]-d10m[0])+
              SQR(dist[k][n][1]-d10m[1])+
              SQR(dist[k][n][2]-d10m[2]);

          d10+=dbuff/4;

          ////////////////////////////////////////////////////////////
          // now move vertex by (dx, dy, dz)
          v->x += dx;
          v->y += dy;
          v->z += dz;
        }

      lm /=mris->nvertices; 
      f1m /=mris->nvertices;
      f2m /=mris->nvertices;
      dm /=mris->nvertices;
      d10 /=mris->nvertices;

      // put it in the array 
      mean_sd[iter%10]=lm;    
      mean_dist[iter%10]=d10;  

      // get the variance of mean_sd
      ///////////////////////////////////
      // get the mean
      coutbuff=0;
      for(n=0;n<10;n++)
        coutbuff+=mean_sd[n]/10;
      // get the variance (not divided by n)
      varbuff=0;
      for(n=0;n<10;n++)
        varbuff+=SQR(mean_sd[n]-coutbuff);

      cout=varbuff;

      // get the variance of mean_dist
      //////////////////////////////////
      // get the mean
      coutbuff=0;
      for(n=0;n<10;n++)
        coutbuff+=mean_dist[n]/10;
      // get the variance (not divided by n)
      varbuff=0;
      for(n=0;n<10;n++)
        varbuff+=SQR(mean_dist[n]-coutbuff);

      // weight the mean_dis more
      cout+=10*varbuff;

      // cache
      coutbuff=cout;
      // get the average (current and previous)
      cout=(cout_prec+cout)/2;

      // save it for the next time
      cout_prec=coutbuff;

      MRIScomputeNormals(mris);

      /*    if ((niter==int_smooth) && !(iter % 5))
            {
            fprintf(stderr,
            "%d: lm=%5.3f,f1m=%5.3f,f2m=%5.3f,dm=%5.3f,d10m=%5.3f,c=%5.3f\n"
            ,iter,lm,f1m,f2m,dm,d10,100*cout);
            sprintf(surf_fname,"./test/lh.test%d",iter);
            MRISwrite(MRI_var->mris,surf_fname);
            }*/

      /*
        if (iter%10==1)
        {
        char buf[256];
        sprintf(buf,"Iter%2d", iter);
        MRISwrite(MRI_var->mris, buf);
        }
      */

      if (niter==int_smooth)  
        {
          if(((iter>20)&&(10000*cout< convLimit))||(iter> maxIter))
            {
              niter--;
            };
        }
      else
        niter--;
    }
  fprintf(stderr,"%d iterations",iter); 

#if WRITE_SURFACES
  sprintf(fname,"./rh.fitshape%d",iter);
  MRISwrite(mris,fname);
#endif

  /*free memory*/
  for( it = 0; it < mris->nvertices; it++ ) 
    {
      for( jt = 0; jt < 4; jt++ ) 
        free(dist[it][jt]);   
      free(dist[it]);
    }
  free(dist);
}

template <typename T> void DebugCurve(const T *percent, 
                                      const int max, const char *msg)
{
#if DEBUG_CURVE
  T tmp = 0;
  if (percent==0)
    return;
  int k;
  std::cerr << msg;
  // find the max value
  tmp=0;
  for(k=0;k< max;k++)
    if (percent[k]>tmp)
      tmp=percent[k];
  // if no max, then 
  if (tmp == 0)
    {
      std::cerr << "max was 0" << std::endl;
      return;
    }
  // print histogram
  for(k=0;k< max;k++)
    {
      std::cerr << std::setw(5) << k << std::setw(10) << percent[k];
      std::cerr << std::setw(1) << " " ;
      int nmax = int(percent[k]*50/tmp);
      for(int n=0;n < nmax ;n++)
        std::cerr << ".";
      std::cerr << std::endl;
    }
#endif
#if OUTPUT_CURVES
  T tmp1;
  static int inited = 0;
  static std::ofstream of;
  if (!inited)
    {
      of.open("./curves.out", std::ios::out);
      inited = 1;
    }
  if (percent==0)
    {
      of.close();
    }

  int m;

  of << msg;
  // find the max value
  tmp1=0;
  for(m=0;m < max;m++)
    if (percent[m]>tmp1)
      tmp1=percent[m];
  // print histogram
  for(m =0;m < max;m++)
    {
      of << std::setw(5) << m << std::setw(10) << percent[m];
      of << std::setw(1) << " ";
      int nmax = int(percent[m]*50/tmp1);
      for(int n=0;n < nmax;n++)
        of << ".";
      of << std::endl;
    }
#endif
}

template <typename T> int findMaxIndex(const T *tab)
{
  T tmp=0;
  T buff;
  int maxIndex=0;
  for(int k=0;k<256;k++)
    {

      buff=tab[k];
      if (buff>tmp)
        {
          tmp=buff;
          maxIndex=k;
        }
    }
  return maxIndex;
}

template <typename T> int findHalfMin(const T *tab, int maxIndex)
{
  int halfMin = 0;
  for(int n=maxIndex;n>0;n--)
    if(tab[n]>=tab[maxIndex]/2)
      halfMin=n;
    else
      break;
  return halfMin;
}

template <typename T> int findHalfMax(const T *tab, int maxIndex)
{
  int halfMax = 0;
  for(int n=maxIndex;n < 255;n++) // not 256 but 255!
    if(tab[n]>=tab[maxIndex]/2)
      halfMax=n;
    else
      break;
  return halfMax;
}

