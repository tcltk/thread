/*
 * --------------------------------------------------------------------------
 * tclthread.h --
 *
 * Global header file for the thread extension.
 *
 * Copyright (c) 2002 ActiveState Corporation.
 * 
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tclThread.h,v 1.6 2002/03/20 01:33:03 hobbs Exp $
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

#define TCL_CMD1(a,b,c,d) \
  Tcl_CreateObjCommand((a), (b), (c), (ClientData)(d), NULL);

#define OPT_CMP(a,b) \
  ((a) && (b) && (*(a)==*(b)) && (*(a+1)==*(b+1)) && (!strcmp((a),(b))))

#ifndef TCL_TSD_INIT
#define TCL_TSD_INIT(keyPtr) \
  (ThreadSpecificData*)Tcl_GetThreadData((keyPtr),sizeof(ThreadSpecificData))
#endif

/*
 * Some functionality within the package depends on the Tcl version.  We
 * require at least 8.3, and more functionality is available for newer
 * versions, so make compatibility defines to compile against 8.3 and work
 * fully in 8.4+.
 */
#if (TCL_MAJOR_VERSION == 8) && (TCL_MINOR_VERSION == 3)

/* 412 */
typedef int thread_JoinThread _ANSI_ARGS_((Tcl_ThreadId id, int* result));
/* 413 */
typedef int thread_IsChannelShared _ANSI_ARGS_((Tcl_Channel channel));
/* 414 */
typedef int thread_IsChannelRegistered _ANSI_ARGS_((Tcl_Interp* interp,
	Tcl_Channel channel));
/* 415 */
typedef void thread_CutChannel _ANSI_ARGS_((Tcl_Channel channel));
/* 416 */
typedef void thread_SpliceChannel _ANSI_ARGS_((Tcl_Channel channel));
/* 417 */
typedef void thread_ClearChannelHandlers _ANSI_ARGS_((Tcl_Channel channel));
/* 418 */
typedef int thread_IsChannelExisting _ANSI_ARGS_((CONST char* channelName));

/*
 * Write up some macros hiding some very hackish pointer arithmetics to get
 * at these fields. We assume that pointer to functions are always of the
 * same size.
 */

#define STUB_BASE   ((char*)(&(tclStubsPtr->tcl_CreateThread))) /* field 393 */
#define procPtrSize (sizeof (Tcl_DriverBlockModeProc *))
#define IDX(n)      (((n)-393) * procPtrSize)
#define SLOT(n)     (STUB_BASE + IDX(n))

#define Tcl_JoinThread		(*((thread_JoinThread**)	 (SLOT(412))))
#define Tcl_IsChannelShared	(*((thread_IsChannelShared**)	 (SLOT(413))))
#define Tcl_IsChannelRegistered	(*((thread_IsChannelRegistered**)(SLOT(414))))
#define Tcl_CutChannel		(*((thread_CutChannel**)	 (SLOT(415))))
#define Tcl_SpliceChannel	(*((thread_SpliceChannel**)	 (SLOT(416))))
#define Tcl_ClearChannelHandlers (*((thread_ClearChannelHandlers**)(SLOT(417))))
#define Tcl_IsChannelExisting	(*((thread_IsChannelExisting**)	 (SLOT(418))))

#endif /* 8.3 compile compatibility */

#undef  TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLIMPORT

#endif /* _TCL_THREAD_H_ */
