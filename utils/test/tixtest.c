//
// tixtest.c
//

#include <stdio.h>
#include <stdlib.h>

#include <tcl.h>
#include <tk.h>
#include <tix.h>

extern int Tix_Init ( Tcl_Interp* interp );
extern int Itcl_Init(Tcl_Interp* interp);
extern int Itk_Init(Tcl_Interp* interp);

int main(int argc, char *argv[])
{
  int        eTcl                              = TCL_OK;
  Tcl_Interp *interp=0;

  setenv("TCL_LIBRARY", "/usr/lib", 1);
  setenv ("TK_LIBRARY", "/usr/lib", 1);

  interp = Tcl_CreateInterp();

  /* read tcl/tk internal startup scripts */
  eTcl = Tcl_Init( interp );
  if( TCL_OK != eTcl ) 
  {
    fprintf(stderr, "Tcl_Init returned %d: %s\n", (int)eTcl, interp->result);
    return -1;
  }
  eTcl = Tk_Init(interp);
  if( TCL_OK != eTcl ) 
  {
    fprintf(stderr, "Tcl_Init returned %d: %s\n", (int)eTcl, interp->result);
    return -1;
  }
  eTcl = Tix_Init( interp );
  if( TCL_OK != eTcl ) 
  {
    fprintf(stderr, "Tix_Init returned %d: %s\n", (int)eTcl, interp->result);

    fprintf(stderr, "Try adding Itcl_Init() and Itk_Init() before Tix_Init()\n");
    eTcl = Itcl_Init(interp);
    if( TCL_OK != eTcl ) 
    {
      fprintf(stderr, "Itlc_Init returned %d: %s\n", (int)eTcl, interp->result);
    }
    eTcl = Itk_Init(interp);
    if( TCL_OK != eTcl ) 
    {
      fprintf(stderr, "Itk_Init returned %d: %s\n", (int)eTcl, interp->result);
    }
    eTcl = Tix_Init( interp );
    if( TCL_OK != eTcl ) 
    {
      printf("even after all these tix initialization still failed\n");
    }
    else
      printf("worked fine with Itcl and Itk init before tix init\n");
  }
  else
    printf("no need to use Itcl and Itk\n");

  // cleanup
  Tcl_DeleteInterp(interp);

  return 0;
}
