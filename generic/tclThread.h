/*
 * --------------------------------------------------------------------------
 * tclthread.h --
 *
 * Global header file for the thread extension.
 * 
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tclThread.h,v 1.4 2002/01/19 23:18:25 vasiljevic Exp $
 * ---------------------------------------------------------------------------
 */

/*
 * Thread extension version numbers are not stored here
 * because this isn't a public export file.
 */

#ifndef _TCL_THREAD_H_
#define _TCL_THREAD_H_

#include <tcl.h>
#include <string.h> /* For memset and friends */

#undef  TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLEXPORT

/*
 * Exported from threadCmd.c file.
 */

EXTERN int  Thread_Init     _ANSI_ARGS_((Tcl_Interp *interp));
EXTERN int  Thread_SafeInit _ANSI_ARGS_((Tcl_Interp *interp));

/*
 * Exported from threadSvCmd.c file.
 */

EXTERN void Sv_Init _ANSI_ARGS_((Tcl_Interp *interp));

/*
 * Exported from threadSpCmd.c file.
 */

EXTERN void Sp_Init _ANSI_ARGS_((Tcl_Interp *interp));

/*
 *  Platform specific functions.
 */


/*
 * Utility macros
 */ 

#define TCL_CMD(a,b,c) \
  Tcl_CreateObjCommand((a), (b), (c), (ClientData)NULL, NULL);

#define OPT_CMP(a,b) \
  ((a) && (b) && (*(a)==*(b)) && (*(a+1)==*(b+1)) && (!strcmp((a),(b))))

#ifndef TCL_TSD_INIT
#define TCL_TSD_INIT(keyPtr) \
  (ThreadSpecificData*)Tcl_GetThreadData((keyPtr),sizeof(ThreadSpecificData))
#endif


#undef  TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLIMPORT

#endif /* _TCL_THREAD_H_ */
