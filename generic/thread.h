/*
 * thread.h --
 * 
 * RCS: @(#) $Id: thread.h,v 1.3 2000/07/03 18:47:59 zoran Exp $ 
 */

#define THREAD_MAJOR_VERSION  2
#define THREAD_MINOR_VERSION  0
#define THREAD_VERSION        "2.0"

#include "tcl.h"

#ifdef __WIN32__
#include <string.h>
#endif

#undef TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLEXPORT

/*
 * Functions exported from threadCmd.c file.
 */

EXTERN int	Thread_Init _ANSI_ARGS_((Tcl_Interp *interp));
EXTERN int	ThreadCreateObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
EXTERN int	ThreadSendObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
EXTERN int	ThreadJoinObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
EXTERN int	ThreadExitObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
EXTERN int	ThreadWaitObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
EXTERN int	ThreadIdObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
EXTERN int	ThreadNamesObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
EXTERN int	ThreadErrorProcObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
EXTERN int	ThreadTransferObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
EXTERN int	ThreadExistsObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));

/*
 * Functions exported from threadSvCmd.c file.
 */

EXTERN int	ThreadSvSetObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
EXTERN int	ThreadSvGetObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
EXTERN int	ThreadSvIncrObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
EXTERN int	ThreadSvAppendObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
EXTERN int	ThreadSvArrayObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
EXTERN int	ThreadSvUnsetObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
EXTERN void     Initialize_Sv _ANSI_ARGS_((Tcl_Interp *interp));

/*
 * Functions exported from threadSpCmd.c file.
 */

EXTERN int	ThreadMutexObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
EXTERN int	ThreadCondObjCmd _ANSI_ARGS_((ClientData clientData,
	Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
EXTERN void     Initialize_Sp _ANSI_ARGS_((Tcl_Interp *interp));

#undef  TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLIMPORT
