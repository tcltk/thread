/*
 * thread.h --
 * 
 * RCS: @(#) $Id: thread.h,v 1.4 2000/08/20 08:00:41 davidg Exp $ 
 */

/* remember to change win/vc/makefile.vc as well when these change */
#define THREAD_MAJOR_VERSION  2
#define THREAD_MINOR_VERSION  0
#define THREAD_VERSION        "2.0"

#include "tcl.h"

#ifdef __WIN32__
#include <string.h>
#endif


/*
 * Functions from threadCmd.c file.
 */

DLLEXPORT int	Thread_Init _ANSI_ARGS_((Tcl_Interp *interp));

int	ThreadCreateObjCmd _ANSI_ARGS_((ClientData clientData, 
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
int	ThreadSendObjCmd _ANSI_ARGS_((ClientData clientData,
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
int	ThreadJoinObjCmd _ANSI_ARGS_((ClientData clientData,
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
int	ThreadExitObjCmd _ANSI_ARGS_((ClientData clientData,
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
int	ThreadWaitObjCmd _ANSI_ARGS_((ClientData clientData,
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
int	ThreadIdObjCmd _ANSI_ARGS_((ClientData clientData,
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
int	ThreadNamesObjCmd _ANSI_ARGS_((ClientData clientData,
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
int	ThreadErrorProcObjCmd _ANSI_ARGS_((ClientData clientData,
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
int	ThreadTransferObjCmd _ANSI_ARGS_((ClientData clientData,
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
int	ThreadExistsObjCmd _ANSI_ARGS_((ClientData clientData,
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));

/*
 * Functions from threadSvCmd.c file.
 */

int	ThreadSvSetObjCmd _ANSI_ARGS_((ClientData clientData,
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
int	ThreadSvGetObjCmd _ANSI_ARGS_((ClientData clientData,
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
int	ThreadSvIncrObjCmd _ANSI_ARGS_((ClientData clientData,
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
int	ThreadSvAppendObjCmd _ANSI_ARGS_((ClientData clientData,
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
int	ThreadSvArrayObjCmd _ANSI_ARGS_((ClientData clientData,
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
int	ThreadSvUnsetObjCmd _ANSI_ARGS_((ClientData clientData,
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
void	Initialize_Sv _ANSI_ARGS_((Tcl_Interp *interp));

/*
 * Functions from threadSpCmd.c file.
 */

int	ThreadMutexObjCmd _ANSI_ARGS_((ClientData clientData,
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
int	ThreadCondObjCmd _ANSI_ARGS_((ClientData clientData,
	    Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[]));
void	Initialize_Sp _ANSI_ARGS_((Tcl_Interp *interp));
