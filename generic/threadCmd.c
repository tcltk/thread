/* 
 * threadCmd.c --
 *
 *	This file implements the Tcl thread commands that allow script
 *	level access to threading.  This is a standard extension and
 *	will not load into a core that was not compiled for thread support.
 *
 *	see http://dev.ajubasolutions.com/doc/howto/thread_model.html
 *
 *	Some of this code is based on work done by Richard Hipp on behalf of
 *	Conservation Through Innovation, Limited, with their permission.
 *
 * Copyright (c) 1998 by Sun Microsystems, Inc.
 * Copyright (c) 1999,2000 by Scriptics Corporation.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: threadCmd.c,v 1.15 2000/08/29 17:06:57 davidg Exp $
 */

#include "thread.h"

/*
 * Each thread has an single instance of the following structure.  There
 * is one instance of this structure per thread even if that thread contains
 * multiple interpreters.  The interpreter identified by this structure is
 * the main interpreter for the thread.  
 *
 * The main interpreter is the one that will process any messages 
 * received by a thread.  Any thread can send messages but only the
 * main interpreter can receive them.
 */

typedef struct ThreadSpecificData {
    Tcl_ThreadId  threadId;             /* Tcl ID for this thread */
    Tcl_Interp *interp;                 /* Main interpreter for this thread */
    int flags;                          /* See tcl.h for list of flags */
    int stopped;                        /* Signalize the thread to stop */
    struct ThreadSpecificData *nextPtr;	/* List for "thread names" */
    struct ThreadSpecificData *prevPtr;	/* List for "thread names" */
} ThreadSpecificData;
static Tcl_ThreadDataKey dataKey;

/*
 * This is a convenience macro used to initialize a thread local storage ptr.
 */
#ifndef TCL_TSD_INIT
#define TCL_TSD_INIT(keyPtr) \
   (ThreadSpecificData*)Tcl_GetThreadData((keyPtr),sizeof(ThreadSpecificData))
#endif

/*
 * This list is used to list all threads that have interpreters.
 * This is protected by threadMutex.
 */

static struct ThreadSpecificData *threadList;

/*
 * An instance of the following structure contains all information that is
 * passed into a new thread when the thread is created using either the
 * "thread create" Tcl command or the ThreadCreate() C function.
 */

typedef struct ThreadCtrl {
    char *script;     /* The TCL command this thread should execute */
    int flags;        /* Initial value of the "flags" field in the 
                       * ThreadSpecificData structure for the new thread */
    Tcl_Condition condWait;
    /* This condition variable is used to synchronize
     * the parent and child threads.  The child won't run
     * until it acquires threadMutex, and the parent function
     * won't complete until signaled on this condition
     * variable. */
} ThreadCtrl;

/*
 * This is the event used to send scripts to other threads.
 */

typedef struct ThreadEvent {
    Tcl_Event event;		/* Must be first */
    char *script;		/* The script to execute. */
    struct ThreadEventResult *resultPtr;
				/* To communicate the result.  This is
				 * NULL if we don't care about it. */
} ThreadEvent;

typedef struct ThreadEventResult {
    Tcl_Condition done;		/* Signaled when the script completes */
    int code;			/* Return value of Tcl_Eval */
    char *result;		/* Result from the script */
    char *errorInfo;		/* Copy of errorInfo variable */
    char *errorCode;		/* Copy of errorCode variable */
    Tcl_ThreadId srcThreadId;	/* Id of sending thread, in case it dies */
    Tcl_ThreadId dstThreadId;	/* Id of target thread, in case it dies */
    struct ThreadEvent *eventPtr;	/* Back pointer */
    struct ThreadEventResult *nextPtr;	/* List for cleanup */
    struct ThreadEventResult *prevPtr;

} ThreadEventResult;

static ThreadEventResult *resultList;

#if (TCL_MAJOR_VERSION > 8) || (TCL_MAJOR_VERSION == 8 && TCL_MINOR_VERSION >= 4)
/*
 * This is the special event used to transfer a channel between threads.
 * This requires Tcl core support added in Tcl 8.4
 */

typedef struct ThreadTransferEvent {
    Tcl_Event     event;	/* Must be first */
    Tcl_Channel   chan;		/* The channel to transfer */
    struct ThreadTransferResult *resultPtr;
				/* To communicate the result. */
} ThreadTransferEvent;


typedef struct ThreadTransferResult {
    Tcl_Condition done;		/* Signaled when the transfer completes */
    int           resultCode;	/* Set to TCL_OK or TCL_ERROR when the
				 * transfer completes. Initialized to -1. */
    char*         resultMsg;    /* Initialized to NULL. Set to a allocated
				 * string by the destination thread in case
				 * of an error. */
    Tcl_ThreadId  srcThreadId;	/* Id of sending thread, in case it dies */
    Tcl_ThreadId  dstThreadId;	/* Id of target thread, in case it dies */
    struct ThreadTransferEvent  *eventPtr;	/* Back pointer */
    struct ThreadTransferResult *nextPtr;	/* List for cleanup */
    struct ThreadTransferResult *prevPtr;
} ThreadTransferResult;

static ThreadTransferResult *transferList;

static int	ThreadTransferEventProc _ANSI_ARGS_((Tcl_Event *evPtr,
	int mask));
static int 	ThreadTransfer _ANSI_ARGS_((Tcl_Interp *interp,
        Tcl_ThreadId id, Tcl_Channel chan));
#endif


/*
 * This is for simple error handling when a thread script exits badly.
 */

static Tcl_ThreadId errorThreadId;
static char *errorProcString;

/* 
 * Access to the list of threads and to the thread send results is
 * guarded by this mutex. 
 */

TCL_DECLARE_MUTEX(threadMutex)

/*
 * Forward declaration of functions used within this file
 */

Tcl_ThreadCreateType	NewThread _ANSI_ARGS_((ClientData clientData));
static void	ListRemove _ANSI_ARGS_((ThreadSpecificData *tsdPtr));
static void	ListUpdateInner _ANSI_ARGS_((ThreadSpecificData *tsdPtr));
static int	ThreadEventProc _ANSI_ARGS_((Tcl_Event *evPtr, int mask));
static int      ThreadWait _ANSI_ARGS_((void));
static int      ThreadStop _ANSI_ARGS_((void));
static int 	ThreadCreate _ANSI_ARGS_((Tcl_Interp *interp,
	CONST char *script, int stacksize, int flags));
static int 	ThreadList _ANSI_ARGS_((Tcl_Interp *interp));
static int 	ThreadSend _ANSI_ARGS_((Tcl_Interp *interp,
	Tcl_ThreadId id, char *script, int wait));
static int 	ThreadJoin _ANSI_ARGS_((Tcl_Interp *interp,
	Tcl_ThreadId id));
static int 	ThreadExists _ANSI_ARGS_((Tcl_ThreadId id));
static void	ThreadErrorProc _ANSI_ARGS_((Tcl_Interp *interp));
static void	ThreadFreeProc _ANSI_ARGS_((ClientData clientData));
Tcl_EventDeleteProc	ThreadDeleteEvent;
static void	ThreadExitProc _ANSI_ARGS_((ClientData clientData));


/*
 *----------------------------------------------------------------------
 *
 * Thread_Init --
 *
 *	Initialize the thread commands.
 *
 * Results:
 *      TCL_OK if the package was properly initialized.
 *
 * Side effects:
 *	Add the "thread" command to the interp.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Thread_Init(interp)
    Tcl_Interp *interp; /* The current Tcl interpreter */
{
    Tcl_Obj *boolObjPtr;
    int boolVar;
    int maj, min, ptch, type;

    if (Tcl_InitStubs(interp, "8.3", 0) == NULL) {
	return TCL_ERROR;
    }

    Tcl_GetVersion(&maj, &min, &ptch, &type);

    if ((maj == 8) && (min == 3) && (ptch < 1)) {
	/* Truely depends on 8.3.1+ with the new Tcl_CreateThread API
	 */
	Tcl_SetObjResult(interp, Tcl_NewStringObj(
		"The thread extension can't run in a Tcl core less than version 8.3.1", -1));
	return TCL_ERROR;
    }

    boolObjPtr = Tcl_GetVar2Ex(interp, "::tcl_platform", "threaded", 0);

    if ((boolObjPtr != NULL)
	 && (Tcl_GetBooleanFromObj(interp, boolObjPtr, &boolVar) != TCL_ERROR)
	 && boolVar) {

	/*
	 * We seem to have a Tcl core compiled with threads enabled,
	 * so let's initialize ourselves.  This uses the thread:: namespace.
	 */

	Tcl_CreateObjCommand(interp,"thread::create", ThreadCreateObjCmd, 
		(ClientData)NULL ,NULL);
	Tcl_CreateObjCommand(interp,"thread::send", ThreadSendObjCmd, 
		(ClientData)NULL ,NULL);
	Tcl_CreateObjCommand(interp,"thread::exit", ThreadExitObjCmd, 
		(ClientData)NULL ,NULL);
	Tcl_CreateObjCommand(interp,"thread::id", ThreadIdObjCmd, 
		(ClientData)NULL ,NULL);
	Tcl_CreateObjCommand(interp,"thread::names", ThreadNamesObjCmd, 
		(ClientData)NULL ,NULL);
	Tcl_CreateObjCommand(interp,"thread::wait", ThreadWaitObjCmd, 
		(ClientData)NULL ,NULL);
	Tcl_CreateObjCommand(interp,"thread::errorproc", ThreadErrorProcObjCmd, 
		(ClientData)NULL ,NULL);
	Tcl_CreateObjCommand(interp,"thread::exists", ThreadExistsObjCmd, 
		(ClientData)NULL ,NULL);

	if ((maj > 8) || ((maj == 8) && (min >= 4)) ) {
	    /* Only 8.4+ supports the transfer of channels.
	     */
	    Tcl_CreateObjCommand(interp,"thread::join", ThreadJoinObjCmd, 
		    (ClientData)NULL ,NULL);
	    Tcl_CreateObjCommand(interp,"thread::transfer", ThreadTransferObjCmd, 
		    (ClientData)NULL ,NULL);
	}

	/*
	 * Add sv_* family of commands in the same namespace.
	 * Command implementation is located in threadSvCmd.c file.
	 */
    
	Initialize_Sv(interp);

    
	/*
	 * Add access to synchronisation primitives (condition variables,
	 * mutexes) in the same namespace.
 	 * Command implementation is located in threadSpCmd.c file.
	 */
    
	Initialize_Sp(interp);

	if (Tcl_PkgProvide(interp, "Thread", THREAD_VERSION) != TCL_OK) {
	    return TCL_ERROR;
	}
	return TCL_OK;
    } else {
	Tcl_SetObjResult(interp, Tcl_NewStringObj(
		"This Tcl core wasn't compiled for multithreading.", -1));
	return TCL_ERROR;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Init --
 *
 *	Make sure our internal list of threads is created.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	The list of threads is initialzied to include the current thread.
 *
 *----------------------------------------------------------------------
 */

static void
Init(interp)
    Tcl_Interp *interp;			/* Current interpreter. */
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    if (tsdPtr->interp == (Tcl_Interp *)NULL) {
	Tcl_MutexLock(&threadMutex);
	tsdPtr->interp = interp;
	ListUpdateInner(tsdPtr);
	Tcl_CreateThreadExitHandler(ThreadExitProc, NULL);
	Tcl_MutexUnlock(&threadMutex);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadCreateObjCmd --
 *
 *	This procedure is invoked to process the "thread::create" Tcl
 *	command. See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
ThreadCreateObjCmd(dummy, interp, objc, objv)
    ClientData dummy;			/* Not used. */
    Tcl_Interp *interp;			/* Current interpreter. */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    char *script;
    int   flags = TCL_THREAD_NOFLAGS;

    /* Syntax of create:  ?-joinable? ?script?
     */

    Init(interp);

    if (objc == 1) {
        /* Neither option nor script available.
	 */

        script = "thread::wait";	/* Just enter the event loop */

    } else if (objc == 2) {
        /* Either option or script possible, but not both.
	 */

        int   arglen;
        char* arg = Tcl_GetStringFromObj (objv[1], &arglen);

	if ((arglen > 1) && (arg [0] == '-') && (arg [1] == 'j') &&
	    (0 == strncmp (arg, "-joinable", arglen))) {

	    /* flag specified, use default script
	     */

	    flags |= TCL_THREAD_JOINABLE;
	    script = "thread::wait"; /* Just enter the event loop */
	} else {
	    /* No flag, remember argument as the script
	     */

            script = arg;
	}
    } else if (objc == 3) {
        /* Enough information for both flag and script. Check that the flag
	 * is valid.
	 */

        int   arglen;
        char* arg = Tcl_GetStringFromObj (objv[1], &arglen);

	if ((arglen > 1) && (arg [0] == '-') && (arg [1] == 'j') &&
	    (0 == strncmp (arg, "-joinable", arglen))) {

	    /* flag specified
	     */

	    flags |= TCL_THREAD_JOINABLE;
	} else {
	    Tcl_WrongNumArgs(interp, 1, objv, "?-joinable? ?script?");
	    return TCL_ERROR;
	}
        script = Tcl_GetString(objv[2]);
    } else {
	Tcl_WrongNumArgs(interp, 1, objv, "?-joinable? ?script?");
	return TCL_ERROR;
    }
    return ThreadCreate(interp, script, TCL_THREAD_STACK_DEFAULT, flags);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadExitObjCmd --
 *
 *	This procedure is invoked to process the "thread::exit" Tcl 
 *	command. See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
ThreadExitObjCmd(dummy, interp, objc, objv)
    ClientData dummy;			/* Not used. */
    Tcl_Interp *interp;			/* Current interpreter. */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    Init(interp);
    if (objc > 1) {
	Tcl_WrongNumArgs(interp, 1, objv, NULL);
	return TCL_ERROR;
    }
    return ThreadStop();
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadIdObjCmd --
 *
 *	This procedure is invoked to process the "thread::id" Tcl command.
 *	This returns the ID of the current thread.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
ThreadIdObjCmd(dummy, interp, objc, objv)
    ClientData dummy;			/* Not used. */
    Tcl_Interp *interp;			/* Current interpreter. */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    Tcl_Obj *idObj;
    Init(interp);
    if (objc > 1) {
	Tcl_WrongNumArgs(interp, 1, objv, NULL);
	return TCL_ERROR;
    }
    idObj = Tcl_NewLongObj((long)Tcl_GetCurrentThread());
    Tcl_SetObjResult(interp, idObj);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadNamesObjCmd --
 *
 *	This procedure is invoked to process the "thread::names" Tcl 
 *	command. This returns a list of all known thread IDs.  
 *	These are only threads created via this module (e.g., not 
 *	driver threads or the notifier).
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
ThreadNamesObjCmd(dummy, interp, objc, objv)
    ClientData dummy;			/* Not used. */
    Tcl_Interp *interp;			/* Current interpreter. */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    Init(interp);
    if (objc > 1) {
	Tcl_WrongNumArgs(interp, 1, objv, NULL);
	return TCL_ERROR;
    }
    return ThreadList(interp);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadSendObjCmd --
 *
 *	This procedure is invoked to process the "thread::send" Tcl 
 *	command. This sends a script to another thread for execution.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
ThreadSendObjCmd(dummy, interp, objc, objv)
    ClientData dummy;			/* Not used. */
    Tcl_Interp *interp;			/* Current interpreter. */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    long id;
    char *script;
    int wait, arg;

    Init(interp);
    if ((objc != 3) && (objc != 4)) {
	Tcl_WrongNumArgs(interp, 1, objv, "?-async? id script");
	return TCL_ERROR;
    }
    if (objc == 4) {
	if (strcmp("-async", Tcl_GetString(objv[1])) != 0) {
	    Tcl_WrongNumArgs(interp, 1, objv, "?-async? id script");
	    return TCL_ERROR;
	}
	wait = 0;
	arg  = 2;
    } else {
	wait = 1;
	arg  = 1;
    }
    if (Tcl_GetLongFromObj(interp, objv[arg], &id) != TCL_OK) {
	return TCL_ERROR;
    }
    arg++;
    script = Tcl_GetString(objv[arg]);
    return ThreadSend(interp, (Tcl_ThreadId) id, script, wait);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadWaitObjCmd --
 *
 *	This procedure is invoked to process the "thread::wait" Tcl 
 *	command. This enters the event loop. It returns when the thread
 *	is being stop by calling the ThreadStop() function.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	Enters the event loop.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
ThreadWaitObjCmd(dummy, interp, objc, objv)
    ClientData dummy;			/* Not used. */
    Tcl_Interp *interp;			/* Current interpreter. */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    Init(interp);

    if (objc > 1) {
	Tcl_WrongNumArgs(interp, 1, objv, NULL);
	return TCL_ERROR;
    }
    return ThreadWait();
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadErrorProcObjCmd --
 *
 *	This procedure is invoked to process the "thread::errorproc" 
 *	command. This registers a procedure to handle thread errors.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	Registers an errorproc.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
ThreadErrorProcObjCmd(dummy, interp, objc, objv)
    ClientData dummy;			/* Not used. */
    Tcl_Interp *interp;			/* Current interpreter. */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    /*
     * Arrange for this proc to handle thread errors.
     */

    char *proc;
    Init(interp);
    if (objc > 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "?proc?");
	return TCL_ERROR;
    }
    Tcl_MutexLock(&threadMutex);
    if (objc == 1) {
	if (errorProcString) {
	    Tcl_SetResult(interp, errorProcString, TCL_VOLATILE);
	}
    } else {
	errorThreadId = Tcl_GetCurrentThread();
	if (errorProcString) {
	    Tcl_Free(errorProcString);
	}
	proc = Tcl_GetString(objv[1]);
	errorProcString = Tcl_Alloc(strlen(proc)+1);
	strcpy(errorProcString, proc);
    }
    Tcl_MutexUnlock(&threadMutex);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadJoinObjCmd --
 *
 *	This procedure is invoked to process the "thread::join" Tcl 
 *	command. See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
ThreadJoinObjCmd(dummy, interp, objc, objv)
    ClientData dummy;			/* Not used. */
    Tcl_Interp *interp;			/* Current interpreter. */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    /* Syntax of 'join': id
     */

#if (TCL_MAJOR_VERSION > 8) || (TCL_MAJOR_VERSION == 8 && TCL_MINOR_VERSION >= 4)
    long id;

    Init(interp);

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "id");
	return TCL_ERROR;
    }
    if (Tcl_GetLongFromObj(interp, objv [1], &id) != TCL_OK) {
	return TCL_ERROR;
    }

    return ThreadJoin(interp, (Tcl_ThreadId) id);
#else
    Tcl_SetObjResult(interp, Tcl_NewStringObj(
	    "Thread join not supported in this Tcl version.", -1));
    return TCL_ERROR;
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadTransferObjCmd --
 *
 *	This procedure is invoked to process the "thread::transfer" Tcl
 *	command. See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

    /* ARGSUSED */
int
ThreadTransferObjCmd(dummy, interp, objc, objv)
    ClientData dummy;			/* Not used. */
    Tcl_Interp *interp;			/* Current interpreter. */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    /* Syntax of 'transfer': id channel
     */

    long        id;
    Tcl_Channel chan;

#if (TCL_MAJOR_VERSION > 8) || (TCL_MAJOR_VERSION == 8 && TCL_MINOR_VERSION >= 4)
    Init(interp);

    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "id channel");
	return TCL_ERROR;
    }
    if (Tcl_GetLongFromObj(interp, objv [1], &id) != TCL_OK) {
	return TCL_ERROR;
    }
    chan = Tcl_GetChannel(interp, Tcl_GetString (objv[2]), NULL);
    if (chan == (Tcl_Channel) NULL) {
	return TCL_ERROR;
    }

    return ThreadTransfer(interp, (Tcl_ThreadId) id, chan);
#else
    Tcl_SetObjResult(interp, Tcl_NewStringObj(
	    "Channel transfer not supported in this Tcl version.", -1));
    return TCL_ERROR;
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadExistsObjCmd --
 *
 *	This procedure is invoked to process the "thread::exists" Tcl
 *	command. See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

    /* ARGSUSED */
int
ThreadExistsObjCmd(dummy, interp, objc, objv)
    ClientData dummy;			/* Not used. */
    Tcl_Interp *interp;			/* Current interpreter. */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    long id;
    int exists;

    Init(interp);

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "id");
	return TCL_ERROR;
    }
    if (Tcl_GetLongFromObj(interp, objv[1], &id) != TCL_OK) {
	return TCL_ERROR;
    }
    exists = ThreadExists((Tcl_ThreadId)id);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(exists));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadCreate --
 *
 *	This procedure is invoked to create a thread containing an 
 *	interp to run a script. This returns after the thread has
 *	started executing.
 *
 * Results:
 *	A standard Tcl result, which is the thread ID.
 *
 * Side effects:
 *	Create a thread.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadCreate(interp, script, stacksize, flags)
    Tcl_Interp *interp;			/* Current interpreter. */
    CONST char *script;			/* Script to execute */
    int         stacksize;		/* zero for default size */
    int         flags;			/* zero for no flags */
{
    ThreadCtrl ctrl;
    Tcl_ThreadId id;

    ctrl.script = (char *) script;
    ctrl.condWait = NULL;
    ctrl.flags = 0;

    Tcl_MutexLock(&threadMutex);
    if (Tcl_CreateThread(&id, NewThread, (ClientData) &ctrl,
	 stacksize, flags) != TCL_OK) {
	Tcl_MutexUnlock(&threadMutex);
        Tcl_AppendResult(interp,"can't create a new thread",0);
	Tcl_Free((void *)ctrl.script);
	return TCL_ERROR;
    }

    /*
     * Wait for the thread to start because it is using something on our stack!
     */

    Tcl_ConditionWait(&ctrl.condWait, &threadMutex, NULL);
    Tcl_MutexUnlock(&threadMutex);
    Tcl_ConditionFinalize(&ctrl.condWait);
    Tcl_SetObjResult(interp, Tcl_NewLongObj((long)id));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NewThread --
 *
 *    This routine is the "main()" for a new thread whose task is to
 *    execute a single TCL script.  The argument to this function is
 *    a pointer to a structure that contains the text of the TCL script
 *    to be executed.
 *
 *    Space to hold the script field of the ThreadControl structure passed 
 *    in as the only argument was obtained from malloc() and must be freed 
 *    by this function before it exits.  Space to hold the ThreadControl
 *    structure itself is released by the calling function, and the
 *    two condition variables in the ThreadControl structure are destroyed
 *    by the calling function.  The calling function will destroy the
 *    ThreadControl structure and the condition variable as soon as
 *    ctrlPtr->condWait is signaled, so this routine must make copies of
 *    any data it might need after that point.
 *
 * Results:
 *    none
 *
 * Side effects:
 *    A TCL script is executed in a new thread.
 *
 *----------------------------------------------------------------------
 */
Tcl_ThreadCreateType
NewThread(clientData)
    ClientData clientData;
{
    ThreadCtrl *ctrlPtr = (ThreadCtrl *)clientData;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    int result;
    char *threadEvalScript;

    /*
     * Initialize the interpreter.
     */

    tsdPtr->interp = Tcl_CreateInterp();
    if ((result = Tcl_Init(tsdPtr->interp)) != TCL_OK) {
	Tcl_ConditionNotify(&ctrlPtr->condWait);
	ThreadErrorProc(tsdPtr->interp);
	Tcl_ExitThread(result);
    }
    result = Thread_Init(tsdPtr->interp);

    /*
     * Update the list of threads.
     */

    Tcl_MutexLock(&threadMutex);
    ListUpdateInner(tsdPtr);

    /*
     * We need to keep a pointer to the alloc'ed mem of the script
     * we are eval'ing, for the case that we exit during evaluation
     */

    threadEvalScript = (char *) Tcl_Alloc(strlen(ctrlPtr->script)+1);
    strcpy(threadEvalScript, ctrlPtr->script);

    Tcl_CreateThreadExitHandler(ThreadExitProc, (ClientData) threadEvalScript);

    /*
     * Notify the parent we are alive.
     */

    Tcl_ConditionNotify(&ctrlPtr->condWait);
    Tcl_MutexUnlock(&threadMutex);

    /*
     * Run the script.
     */

    Tcl_Preserve((ClientData) tsdPtr->interp);
    result = Tcl_Eval(tsdPtr->interp, threadEvalScript);
    if (result != TCL_OK) {
	ThreadErrorProc(tsdPtr->interp);
    }
    Tcl_Release((ClientData) tsdPtr->interp);

    /*
     * Clean up. Note: add something like TlistRemove for trasfer list.
     */

    ListRemove(tsdPtr);
    Tcl_DeleteInterp(tsdPtr->interp);
    Tcl_ExitThread(result);

    TCL_THREAD_CREATE_RETURN;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadErrorProc --
 *
 *	Send a message to the thread willing to hear about errors.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Send an event.
 *
 *----------------------------------------------------------------------
 */
static void
ThreadErrorProc(interp)
    Tcl_Interp *interp;			/* Interp that failed */
{
    Tcl_Channel errChannel;
    char *errorInfo, *script;
    char *argv[3];
    char buf[10];
    sprintf(buf, "%ld", (long) Tcl_GetCurrentThread());

    errorInfo = Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
    if (errorProcString == NULL) {
	errChannel = Tcl_GetStdChannel(TCL_STDERR);
	Tcl_WriteChars(errChannel, "Error from thread ", -1);
	Tcl_WriteChars(errChannel, buf, -1);
	Tcl_WriteChars(errChannel, "\n", 1);
	Tcl_WriteChars(errChannel, errorInfo, -1);
	Tcl_WriteChars(errChannel, "\n", 1);
    } else {
	argv[0] = errorProcString;
	argv[1] = buf;
	argv[2] = errorInfo;
	script = Tcl_Merge(3, argv);
	ThreadSend(interp, errorThreadId, script, 0);
	Tcl_Free(script);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * ListUpdateInner --
 *
 *	Add the thread local storage to the list.  This assumes
 *	the caller has obtained the mutex.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Add the thread local storage to its list.
 *
 *----------------------------------------------------------------------
 */
static void
ListUpdateInner(tsdPtr)
    ThreadSpecificData *tsdPtr;
{
    if (tsdPtr == NULL) {
	tsdPtr = TCL_TSD_INIT(&dataKey);
    }
    tsdPtr->threadId = Tcl_GetCurrentThread();
    tsdPtr->nextPtr = threadList;
    if (threadList) {
	threadList->prevPtr = tsdPtr;
    }
    tsdPtr->prevPtr = NULL;
    threadList = tsdPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * ListRemove --
 *
 *	Remove the thread local storage from its list.  This grabs the
 *	mutex to protect the list.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Remove the thread local storage from its list.
 *
 *----------------------------------------------------------------------
 */
static void
ListRemove(tsdPtr)
    ThreadSpecificData *tsdPtr;
{
    if (tsdPtr == NULL) {
	tsdPtr = TCL_TSD_INIT(&dataKey);
    }
    if (tsdPtr->prevPtr == NULL && tsdPtr->nextPtr == NULL) {
	return; /* we've never been here or we're already spliced out */
    }
    Tcl_MutexLock(&threadMutex);
    if (tsdPtr->prevPtr) {
	tsdPtr->prevPtr->nextPtr = tsdPtr->nextPtr;
    } else {
	threadList = tsdPtr->nextPtr;
    }
    if (tsdPtr->nextPtr) {
	tsdPtr->nextPtr->prevPtr = tsdPtr->prevPtr;
    }
    tsdPtr->nextPtr = tsdPtr->prevPtr = NULL;
    Tcl_MutexUnlock(&threadMutex);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadList --
 *
 *	Return a list of threads running Tcl interpreters.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadList(interp)
    Tcl_Interp *interp;
{
    ThreadSpecificData *tsdPtr;
    Tcl_Obj *listPtr;

    listPtr = Tcl_NewListObj(0, NULL);
    Tcl_MutexLock(&threadMutex);
    for (tsdPtr = threadList ; tsdPtr ; tsdPtr = tsdPtr->nextPtr) {
	Tcl_ListObjAppendElement(interp, listPtr,
		Tcl_NewLongObj((long)tsdPtr->threadId));
    }
    Tcl_MutexUnlock(&threadMutex);
    Tcl_SetObjResult(interp, listPtr);
    return TCL_OK;
}

#if (TCL_MAJOR_VERSION > 8) || (TCL_MAJOR_VERSION == 8 && TCL_MINOR_VERSION >= 4)
/*
 *----------------------------------------------------------------------
 *
 * ThreadJoin --
 *
 *	Wait for the exit of a different thread.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	The status of the exiting thread is left in the interp result 
 *	area, but only in the case of success.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadJoin(interp, id)
    Tcl_Interp *interp;			/* The current interpreter. */
    Tcl_ThreadId id;			/* Thread Id of other interpreter. */
{
    int result, state;

    result = Tcl_JoinThread (id, &state);
    if (result == TCL_OK) {
        Tcl_SetIntObj (Tcl_GetObjResult (interp), state);
    } else {
        char buf [20];
	sprintf (buf, "%ld", (long)id);
	Tcl_AppendResult (interp, "cannot join thread ", buf, NULL);
    }
    return result;
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * ThreadExists --
 *
 *	Test wether a thread given by it's id is known to us.
 *
 * Results:
 *	True (1) if yes, zero (0) otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadExists(id)
    Tcl_ThreadId id;                    /* Thread id to look for. */
{
    int found = 0;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    Tcl_MutexLock(&threadMutex);
    for (tsdPtr = threadList ; tsdPtr ; tsdPtr = tsdPtr->nextPtr) {
	if (tsdPtr->threadId == id) {
            found = 1;
	    break;
	}
    }
    Tcl_MutexUnlock(&threadMutex);
    return found;
}

#if (TCL_MAJOR_VERSION > 8) || (TCL_MAJOR_VERSION == 8 && TCL_MINOR_VERSION >= 4)
/*
 *----------------------------------------------------------------------
 *
 * ThreadTransfer --
 *
 *	Transfers the specified channel which must not be shared and has
 *	to be registered in the given interp from that location to the
 *	main interp of the specified thread.
 *
 * Results:
 * *	A standard Tcl result.
 *
 * Side effects:
 *	The thread-global lists of all known channels of both threads
 *	involved (specified and current) are modified. The channel is
 *	moved, all event handling for the channel is killed.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadTransfer(interp, id, chan)
    Tcl_Interp *interp;			/* The current interpreter. */
    Tcl_ThreadId id;			/* Thread Id of other interpreter. */
    Tcl_Channel  chan;			/* The channel to transfer */
{
    /* Steps to perform for the transfer:
     *
     * i.   Sanity checks: chan has to registered in interp, must not be
     *      shared. This automatically excludes the special channels for
     *      stdin, stdout and stderr!
     * ii.  Clear event handling.
     * iii. Bump reference counter up to prevent destruction during the
     *      following unregister, then unregister the channel from the
     *      interp. Remove it from the thread-global list of all channels
     *      too.
     * iv.  Wrap the channel into an event and send that to the other
     *      thread, then wait for the other thread to process our message.
     * v.   The event procedure called by the other thread is
     *	    'ThreadTransferEventProc'. It links the channel into the
     *	    thread-global list of channels for that thread, registers it
     *	    in the main interp of the other thread, removes the artificial
     *	    reference, at last notifies this thread of the sucessful
     *	    transfer. This allows this thread then to proceed.
     */

    ThreadSpecificData   *tsdPtr = TCL_TSD_INIT(&dataKey);
    ThreadTransferEvent  *evPtr;
    ThreadTransferResult *resultPtr;
    int found;

    if (!Tcl_IsChannelRegistered (interp, chan)) {
	Tcl_AppendResult(interp, "channel is not registered here", NULL);
    }
    if (Tcl_IsChannelShared (chan)) {
	Tcl_AppendResult(interp, "channel is shared", NULL);
        return TCL_ERROR;
    }

    /* 
     * Verify the thread exists.
     */

    Tcl_MutexLock(&threadMutex);
    found = 0;
    for (tsdPtr = threadList ; tsdPtr ; tsdPtr = tsdPtr->nextPtr) {
	if (tsdPtr->threadId == id) {
	    found = 1;
	    break;
	}
    }
    if (!found) {
	Tcl_MutexUnlock(&threadMutex);
	Tcl_AppendResult(interp, "invalid thread id", NULL);
	return TCL_ERROR;
    }

    /*
     * Short circut transfers to ourself.  Nothing to do.
     */

    if (id == Tcl_GetCurrentThread()) {
        Tcl_MutexUnlock(&threadMutex);
	return TCL_OK;
    }

    /*
     * Cut channel out of interp and current thread.
     */

    Tcl_ClearChannelHandlers (chan);
    Tcl_RegisterChannel ((Tcl_Interp *) NULL, chan);
    Tcl_UnregisterChannel (interp, chan);
    Tcl_CutChannel (chan);

    /*
     * Wrap it into an event.
     */

    resultPtr= (ThreadTransferResult *)Tcl_Alloc(sizeof(ThreadTransferResult));
    evPtr    = (ThreadTransferEvent *)Tcl_Alloc(sizeof(ThreadTransferEvent));

    evPtr->chan        = chan;
    evPtr->event.proc  = ThreadTransferEventProc;
    evPtr->resultPtr   = resultPtr;

    /*
     * Initialize the result fields.
     */

    resultPtr->done       = (Tcl_Condition) NULL;
    resultPtr->resultCode = -1;
    resultPtr->resultMsg  = (char *) NULL;

    /* 
     * Maintain the cleanup list.
     */

    resultPtr->srcThreadId = Tcl_GetCurrentThread ();
    resultPtr->dstThreadId = id;
    resultPtr->eventPtr    = evPtr;
    resultPtr->nextPtr     = transferList;

    if (transferList) {
      transferList->prevPtr = resultPtr;
    }

    resultPtr->prevPtr = NULL;
    transferList       = resultPtr;

    /*
     * Queue the event and poke the other thread's notifier.
     */

    Tcl_ThreadQueueEvent(id, (Tcl_Event *) evPtr, TCL_QUEUE_TAIL);
    Tcl_ThreadAlert(id);

    /*
     * (*) Block until the other thread has either processed the transfer
     * or rejected it.
     */

    while (resultPtr->resultCode < 0) {
        Tcl_ConditionWait (&resultPtr->done, &threadMutex, NULL);
    }

    /*
     * Unlink result from the result list.
     */

    if (resultPtr->prevPtr) {
	resultPtr->prevPtr->nextPtr = resultPtr->nextPtr;
    } else {
	transferList = resultPtr->nextPtr;
    }
    if (resultPtr->nextPtr) {
	resultPtr->nextPtr->prevPtr = resultPtr->prevPtr;
    }
    resultPtr->eventPtr = NULL;
    resultPtr->nextPtr = NULL;
    resultPtr->prevPtr = NULL;

    Tcl_MutexUnlock(&threadMutex);
    Tcl_ConditionFinalize (&resultPtr->done);

    /*
     * Process the result now.
     */

    if (resultPtr->resultCode != TCL_OK) {
        /*
	 * Transfer failed, restore old state of channel with respect
	 * to current thread and specified interp.
	 */

	Tcl_SpliceChannel (chan);
	Tcl_RegisterChannel (interp, chan);
        Tcl_UnregisterChannel ((Tcl_Interp *) NULL, chan);

	Tcl_AppendResult(interp, "transfer failed: ", NULL);
	if (resultPtr->resultMsg) {
	    Tcl_AppendResult(interp, resultPtr->resultMsg, NULL);
	    Tcl_Free (resultPtr->resultMsg);
	} else {
	    Tcl_AppendResult(interp, "for reasons unknown", NULL);
	}
	return TCL_ERROR;
    }

    if (resultPtr->resultMsg) {
        Tcl_Free (resultPtr->resultMsg);
    }

    return TCL_OK;
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * ThreadSend --
 *
 *	Send a script to another thread.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadSend(interp, id, script, wait)
    Tcl_Interp *interp;			/* The current interpreter. */
    Tcl_ThreadId id;			/* Thread Id of other interpreter. */
    char *script;			/* The script to evaluate. */
    int wait;				/* If 1, we block for the result. */
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    ThreadEvent *threadEventPtr;
    ThreadEventResult *resultPtr;
    int found, code;
    Tcl_ThreadId threadId = (Tcl_ThreadId) id;

    /* 
     * Verify the thread exists.
     */

    Tcl_MutexLock(&threadMutex);
    found = 0;
    for (tsdPtr = threadList ; tsdPtr ; tsdPtr = tsdPtr->nextPtr) {
	if (tsdPtr->threadId == threadId) {
	    found = 1;
	    break;
	}
    }
    if (!found) {
	Tcl_MutexUnlock(&threadMutex);
	Tcl_AppendResult(interp, "invalid thread id", NULL);
	return TCL_ERROR;
    }

    /*
     * Short circut sends to ourself.  Ought to do something with -async,
     * like run in an idle handler.
     */

    if (threadId == Tcl_GetCurrentThread()) {
        Tcl_MutexUnlock(&threadMutex);
	return Tcl_EvalEx(interp, script, -1, TCL_EVAL_GLOBAL);
    }

    /* 
     * Create the event for its event queue.
     */

    threadEventPtr = (ThreadEvent *) Tcl_Alloc(sizeof(ThreadEvent));
    threadEventPtr->script = Tcl_Alloc(strlen(script) + 1);
    strcpy(threadEventPtr->script, script);
    if (!wait) {
	resultPtr = threadEventPtr->resultPtr = NULL;
    } else {
	resultPtr = (ThreadEventResult *) Tcl_Alloc(sizeof(ThreadEventResult));
	threadEventPtr->resultPtr = resultPtr;

	/*
	 * Initialize the result fields.
	 */

	resultPtr->done = NULL;
	resultPtr->code = 0;
	resultPtr->result = NULL;
	resultPtr->errorInfo = NULL;
	resultPtr->errorCode = NULL;

	/* 
	 * Maintain the cleanup list.
	 */

	resultPtr->srcThreadId = Tcl_GetCurrentThread();
	resultPtr->dstThreadId = threadId;
	resultPtr->eventPtr = threadEventPtr;
	resultPtr->nextPtr = resultList;
	if (resultList) {
	    resultList->prevPtr = resultPtr;
	}
	resultPtr->prevPtr = NULL;
	resultList = resultPtr;
    }

    /*
     * Queue the event and poke the other thread's notifier.
     */

    threadEventPtr->event.proc = ThreadEventProc;
    Tcl_ThreadQueueEvent(threadId, (Tcl_Event *)threadEventPtr, 
	    TCL_QUEUE_TAIL);
    Tcl_ThreadAlert(threadId);

    if (!wait) {
	Tcl_MutexUnlock(&threadMutex);
	return TCL_OK;
    }

    /* 
     * Block on the results and then get them.
     */

    Tcl_ResetResult(interp);
    while (resultPtr->result == NULL) {
        Tcl_ConditionWait(&resultPtr->done, &threadMutex, NULL);
    }

    /*
     * Unlink result from the result list.
     */

    if (resultPtr->prevPtr) {
	resultPtr->prevPtr->nextPtr = resultPtr->nextPtr;
    } else {
	resultList = resultPtr->nextPtr;
    }
    if (resultPtr->nextPtr) {
	resultPtr->nextPtr->prevPtr = resultPtr->prevPtr;
    }
    resultPtr->eventPtr = NULL;
    resultPtr->nextPtr = NULL;
    resultPtr->prevPtr = NULL;

    Tcl_MutexUnlock(&threadMutex);

    if (resultPtr->code != TCL_OK) {
	if (resultPtr->errorCode) {
	    Tcl_SetErrorCode(interp, resultPtr->errorCode, NULL);
	    Tcl_Free(resultPtr->errorCode);
	}
	if (resultPtr->errorInfo) {
	    Tcl_AddErrorInfo(interp, resultPtr->errorInfo);
	    Tcl_Free(resultPtr->errorInfo);
	}
    }
    Tcl_SetResult(interp, resultPtr->result, TCL_DYNAMIC);
    Tcl_ConditionFinalize(&resultPtr->done);
    code = resultPtr->code;

    Tcl_Free((char *) resultPtr);

    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadWait --
 *
 *	Waits for events and process them as they come, until stopped.
 *	At stop, process all runable events from the event queue
 *	and return gracefuly.
 *
 * Results:
 *	TCL_OK always
 *
 * Side effects:
 *	Deletes events posted from other threads and processes all 
 *	pending events from other sources.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadWait()
{    
    int eventFlags = TCL_ALL_EVENTS;
    int i;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    /*
     * Hang-around until signaled to exit.
     */

    while (!(tsdPtr->stopped)) {
        (void) Tcl_DoOneEvent(eventFlags);
    }

    /*
     * Splice ourselves from the thread-list early. This prevents
     * other threads to send us more work while we're exiting.
     */

    ListRemove(tsdPtr);

    /*
     * Delete all pending thread::send events.
     * By doing this here, we avoid processing them below.
     */

    Tcl_DeleteEvents((Tcl_EventDeleteProc *)ThreadDeleteEvent, NULL);

    /*
     * Run all other pending events which we can not sink otherwise.
     * We assume that no runnable event will block us indefinitely
     * or kick us into a infinite loop, otherwise we're stuck.
     */

    eventFlags |= TCL_DONT_WAIT;
    i=100;
    while (Tcl_DoOneEvent(eventFlags) && (--i > 0)) {
        ; /* empty loop */
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadStop --
 *
 *	Signals the current thread to stop.
 *
 * Results:
 *	TCL_OK always.
 *
 * Side effects:
 * 	Thread will jump out of the event loop and prepare for exit.	
 *
 *----------------------------------------------------------------------
 */
static int 
ThreadStop()
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    tsdPtr->stopped = 1;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadEventProc --
 *
 *	Handle the event in the target thread.
 *
 * Results:
 *	Returns 1 to indicate that the event was processed.
 *
 * Side effects:
 *	Fills out the ThreadEventResult struct.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadEventProc(evPtr, mask)
    Tcl_Event *evPtr;			/* Really ThreadEvent */
    int mask;
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    ThreadEvent *threadEventPtr = (ThreadEvent *)evPtr;
    ThreadEventResult *resultPtr = threadEventPtr->resultPtr;
    Tcl_Interp *interp = tsdPtr->interp;
    int code;
    char *result, *errorCode, *errorInfo;

    if (interp == NULL) {
	code = TCL_ERROR;
	result = "no target interp!";
	errorCode = "THREAD";
	errorInfo = "";
    } else {
	Tcl_Preserve((ClientData) interp);
	Tcl_ResetResult(interp);
	Tcl_CreateThreadExitHandler(ThreadFreeProc,
		(ClientData) threadEventPtr->script);
	code = Tcl_EvalEx(interp, threadEventPtr->script, -1, TCL_EVAL_GLOBAL);
	Tcl_DeleteThreadExitHandler(ThreadFreeProc,
		(ClientData) threadEventPtr->script);
	result = Tcl_GetStringResult(interp);
	if (code != TCL_OK) {
	    errorCode = Tcl_GetVar(interp, "errorCode", TCL_GLOBAL_ONLY);
	    errorInfo = Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
	} else {
	    errorCode = errorInfo = NULL;
	}
    }
    Tcl_Free(threadEventPtr->script);
    if (resultPtr) {
	Tcl_MutexLock(&threadMutex);
	resultPtr->code = code;
	resultPtr->result = Tcl_Alloc(strlen(result) + 1);
	strcpy(resultPtr->result, result);
	if (errorCode != NULL) {
	    resultPtr->errorCode = Tcl_Alloc(strlen(errorCode) + 1);
	    strcpy(resultPtr->errorCode, errorCode);
	}
	if (errorInfo != NULL) {
	    resultPtr->errorInfo = Tcl_Alloc(strlen(errorInfo) + 1);
	    strcpy(resultPtr->errorInfo, errorInfo);
	}
	Tcl_ConditionNotify(&resultPtr->done);
	Tcl_MutexUnlock(&threadMutex);
    }
    if (interp != NULL) {
	Tcl_Release((ClientData) interp);
    }
    return 1;
}

#if (TCL_MAJOR_VERSION > 8) || (TCL_MAJOR_VERSION == 8 && TCL_MINOR_VERSION >= 4)
/*
 *----------------------------------------------------------------------
 *
 * ThreadTransferEventProc --
 *
 *	Handle a transfer event in the target thread.
 *
 * Results:
 *	Returns 1 to indicate that the event was processed.
 *
 * Side effects:
 *	Fills out the ThreadTransferResult struct.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadTransferEventProc(evPtr, mask)
    Tcl_Event *evPtr;			/* Really ThreadEvent */
    int mask;
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    ThreadTransferEvent *threadEventPtr = (ThreadTransferEvent *)evPtr;
    ThreadTransferResult *resultPtr = threadEventPtr->resultPtr;
    Tcl_Interp *interp = tsdPtr->interp;
    int   code;
    char* msg = NULL;

    if (interp == NULL) {
        /*
	 * Reject transfer in case of a missing target.
	 */
        code = TCL_ERROR;
	msg  = "target interp missing";
    } else {
        /*
	 * Add channel to current thread and interp.
	 * See ThreadTransfer for more explanations.
	 */

        if (Tcl_IsChannelExisting(Tcl_GetChannelName(threadEventPtr->chan))) {
	    /*
	     * Reject transfer. Channel of same name already exists in target.
	     */
	    code = TCL_ERROR;
	    msg  = "channel already exists in target";
	} else {
	    Tcl_SpliceChannel (threadEventPtr->chan);
	    Tcl_RegisterChannel (interp, threadEventPtr->chan);
	    Tcl_UnregisterChannel ((Tcl_Interp *) NULL, threadEventPtr->chan);

	    /*
	     * Return success.
	     */

	    code = TCL_OK;
	}
    }

    if (resultPtr) {
	Tcl_MutexLock(&threadMutex);
	resultPtr->resultCode = code;

	if (msg != NULL) {
	    resultPtr->resultMsg = (char *) Tcl_Alloc (1+strlen (msg));
	    strcpy (resultPtr->resultMsg, msg);
	}

	Tcl_ConditionNotify(&resultPtr->done);
	Tcl_MutexUnlock(&threadMutex);
    }

    return 1;
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * ThreadFreeProc --
 *
 *	This is called from when we are exiting and memory needs
 *	to be freed.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Clears up mem specified in ClientData
 *
 *----------------------------------------------------------------------
 */
static void
ThreadFreeProc(clientData)
    ClientData clientData;
{
    if (clientData) {
	Tcl_Free((char *) clientData);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadDeleteEvent --
 *
 *	This is called from the ThreadExitProc to delete memory related
 *	to events that we put on the queue.
 *
 * Results:
 *	1 it was our event and we want it removed, 0 otherwise.
 *
 * Side effects:
 *	It cleans up our events in the event queue for this thread.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadDeleteEvent(eventPtr, clientData)
    Tcl_Event *eventPtr;		/* Really ThreadEvent */
    ClientData clientData;		/* dummy */
{
    if (eventPtr->proc == ThreadEventProc) {
	Tcl_Free((char *) ((ThreadEvent *) eventPtr)->script);
	return 1;
    }
#if (TCL_MAJOR_VERSION > 8) || (TCL_MAJOR_VERSION == 8 && TCL_MINOR_VERSION >= 4)
    if (eventPtr->proc == ThreadTransferEventProc) {
        /* A channel is in flight toward the thread just exiting.
	 * Pass it back to the originator, if possible.
	 * Else kill it.
	 */

      ThreadTransferEvent* evPtr = (ThreadTransferEvent *) eventPtr;

      if (evPtr->resultPtr == (ThreadTransferResult *) NULL) {
	  /* No thread to pass the channel back to. Kill it.
	   * This requires to splice it temporarily into our channel
	   * list and then forcing the ref.counter down to the real
	   * value of zero. This destroys the channel.
	   */

          Tcl_SpliceChannel (evPtr->chan);
	  Tcl_UnregisterChannel ((Tcl_Interp *) NULL, evPtr->chan);
	  return 1;
      }

      /* Our caller (ThreadExitProc) will pass the channel back.
       */

      return 1;
    }
#endif

    /*
     * If it was NULL, we were in the middle of servicing the event
     * and it should be removed
     */
    return (eventPtr->proc == NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadExitProc --
 *
 *	This is called when the thread exits.  
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	It unblocks anyone that is waiting on a send to this thread.
 *	It cleans up any events in the event queue for this thread.
 *
 *----------------------------------------------------------------------
 */
static void
ThreadExitProc(clientData)
    ClientData clientData;
{
    char *threadEvalScript = (char *) clientData;
    ThreadEventResult *resultPtr, *nextPtr;
#if (TCL_MAJOR_VERSION > 8) || (TCL_MAJOR_VERSION == 8 && TCL_MINOR_VERSION >= 4)
    ThreadTransferResult *tResultPtr, *tNextPtr;
#endif
    Tcl_ThreadId self = Tcl_GetCurrentThread();

    Tcl_MutexLock(&threadMutex);

    if (threadEvalScript) {
	Tcl_Free((char *) threadEvalScript);
	threadEvalScript = NULL;
    }

    /* 
     * Delete events posted to our queue while we were running.
     * For threads exiting from the thread::wait command, this 
     * has already been done in ThreadWait() function.
     * For one-shot threads, having something here is a very 
     * strange condition. It *may* happen if somebody posts us
     * an event while we were in the middle of processing some
     * lengthly user script. It is very unlikely to happen, though.
     */ 

    Tcl_DeleteEvents((Tcl_EventDeleteProc *)ThreadDeleteEvent, NULL);

    /*
     * Walk the list of threads waiting for result from us 
     * and inform them that we're about to exit.
     */

    for (resultPtr = resultList ; resultPtr ; resultPtr = nextPtr) {
	nextPtr = resultPtr->nextPtr;
	if (resultPtr->srcThreadId == self) {

	    /*
	     * We are going away.  By freeing up the result we signal
	     * to the other thread we don't care about the result.
	     */

	    if (resultPtr->prevPtr) {
		resultPtr->prevPtr->nextPtr = resultPtr->nextPtr;
	    } else {
		resultList = resultPtr->nextPtr;
	    }
	    if (resultPtr->nextPtr) {
		resultPtr->nextPtr->prevPtr = resultPtr->prevPtr;
	    }
	    resultPtr->nextPtr = resultPtr->prevPtr = NULL;
	    resultPtr->eventPtr->resultPtr = NULL;
	    Tcl_Free((char *)resultPtr);
	} else if (resultPtr->dstThreadId == self) {

	    /*
	     * Dang.  The target is going away.  Unblock the caller.
	     * The result string must be dynamically allocated because
	     * the main thread is going to call free on it.
	     */

	    char *msg = "target thread died";
	    resultPtr->result = Tcl_Alloc(strlen(msg)+1);
	    strcpy(resultPtr->result, msg);
	    resultPtr->code = TCL_ERROR;
	    Tcl_ConditionNotify(&resultPtr->done);
	}
    }

#if (TCL_MAJOR_VERSION > 8) || (TCL_MAJOR_VERSION == 8 && TCL_MINOR_VERSION >= 4)
    for (tResultPtr = transferList ; tResultPtr ; tResultPtr = tNextPtr) {
	tNextPtr = tResultPtr->nextPtr;
	if (tResultPtr->srcThreadId == self) {

	    /*
	     * We are going away.  By freeing up the result we signal
	     * to the other thread we don't care about the result.
	     *
	     * This should not happen, as this thread should be in
	     * ThreadTransfer at location (*).
	     */

	    if (tResultPtr->prevPtr) {
		tResultPtr->prevPtr->nextPtr = tResultPtr->nextPtr;
	    } else {
		transferList = tResultPtr->nextPtr;
	    }
	    if (tResultPtr->nextPtr) {
		tResultPtr->nextPtr->prevPtr = tResultPtr->prevPtr;
	    }
	    tResultPtr->nextPtr = tResultPtr->prevPtr = 0;
	    tResultPtr->eventPtr->resultPtr = NULL;
	    Tcl_Free((char *)tResultPtr);
	} else if (tResultPtr->dstThreadId == self) {

	    /*
	     * Dang.  The target is going away.  Unblock the caller and
	     *	      deliver a failure notice.
         *
	     * The result string must be dynamically allocated because
	     * the main thread is going to call free on it.
	     */

	    char *msg = "target thread died";
	    tResultPtr->resultMsg = Tcl_Alloc(strlen(msg)+1);
	    strcpy(tResultPtr->resultMsg, msg);
	    tResultPtr->resultCode = TCL_ERROR;
	    Tcl_ConditionNotify(&tResultPtr->done);
	}
    }
#endif
    Tcl_MutexUnlock(&threadMutex);
}
