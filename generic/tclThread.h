/*
 * --------------------------------------------------------------------------
 * thread.h --
 *
 * 	Global header file for the thread extension.
 * 
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tclThread.h,v 1.2 2001/09/05 23:02:01 davygrvy Exp $
 * ---------------------------------------------------------------------------
 */

/*
 * thread extension version numbers are not stored here because this
 * isn't a public export file.
 */

#ifndef _TCL_THREAD_H_
#define _TCL_THREAD_H_

#include <tcl.h>

#ifdef __WIN32__
#include <string.h>
#endif

/*
 * Functions from threadCmd.c file.
 */

#undef TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLEXPORT

EXTERN int	Thread_Init _ANSI_ARGS_((Tcl_Interp *interp));
EXTERN int	Thread_SafeInit _ANSI_ARGS_((Tcl_Interp *interp));

Tcl_ObjCmdProc	ThreadCreateObjCmd;
Tcl_ObjCmdProc	ThreadSendObjCmd;
Tcl_ObjCmdProc	ThreadJoinObjCmd;
Tcl_ObjCmdProc	ThreadUnwindObjCmd;
Tcl_ObjCmdProc	ThreadExitObjCmd;
Tcl_ObjCmdProc	ThreadKillObjCmd;
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

/*
 *  Platform specific functions.
 */

void ThreadKill _ANSI_ARGS_((long id));


/*
 * end of header
 * reset TCL_STORAGE_CLASS to DLLIMPORT.
 */
#undef TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLIMPORT

#endif /* _TCL_THREAD_H_ */
