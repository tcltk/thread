/*
 * thread.h --
 *
 * 	Global header file for the thread extension.
 * 
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: thread.h,v 1.8 2001/04/27 01:43:50 davygrvy Exp $ 
 */

/* remember to change win/vc/makefile.vc as well when these change */
#define THREAD_MAJOR_VERSION	2
#define THREAD_MINOR_VERSION	2
#define THREAD_VERSION		"2.2"

/* this is the version provided when loaded into an 8.3 core */
#define THREAD_VERSION_SUBSET83	"2.1.1"

#include "tcl.h"

#ifdef __WIN32__
#include <string.h>
#endif



/*
 * Functions from threadCmd.c file.
 */

#undef TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLEXPORT

EXTERN int	Thread_Init _ANSI_ARGS_((Tcl_Interp *interp));

Tcl_ObjCmdProc	ThreadCreateObjCmd;
Tcl_ObjCmdProc	ThreadSendObjCmd;
Tcl_ObjCmdProc	ThreadJoinObjCmd;
Tcl_ObjCmdProc	ThreadUnwindObjCmd;
Tcl_ObjCmdProc	ThreadWaitObjCmd;
Tcl_ObjCmdProc	ThreadIdObjCmd;
Tcl_ObjCmdProc	ThreadNamesObjCmd;
Tcl_ObjCmdProc	ThreadErrorProcObjCmd;
Tcl_ObjCmdProc	ThreadTransferObjCmd;
Tcl_ObjCmdProc	ThreadExistsObjCmd;

/*
 * Functions from threadSvCmd.c file.
 */

Tcl_ObjCmdProc	ThreadSvSetObjCmd;
Tcl_ObjCmdProc	ThreadSvGetObjCmd;
Tcl_ObjCmdProc	ThreadSvIncrObjCmd;
Tcl_ObjCmdProc	ThreadSvAppendObjCmd;
Tcl_ObjCmdProc	ThreadSvArrayObjCmd;
Tcl_ObjCmdProc	ThreadSvUnsetObjCmd;
void	Initialize_Sv _ANSI_ARGS_((Tcl_Interp *interp));

/*
 * Functions from threadSpCmd.c file.
 */

Tcl_ObjCmdProc	ThreadMutexObjCmd;
Tcl_ObjCmdProc	ThreadCondObjCmd;
void	Initialize_Sp _ANSI_ARGS_((Tcl_Interp *interp));
