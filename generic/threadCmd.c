/* 
 * threadCmd.c --
 *
 *  This file implements the Tcl thread commands that allow script
 *  level access to threading. It will not load into a core that was
 *  not compiled for thread support.
 *
 *  see http://dev.activestate.com/doc/howto/thread_model.html
 *
 *  Some of this code is based on work done by Richard Hipp on behalf of
 *  Conservation Through Innovation, Limited, with their permission.
 *
 * Copyright (c) 1998 by Sun Microsystems, Inc.
 * Copyright (c) 1999,2000 by Scriptics Corporation.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: threadCmd.c,v 1.38 2002/03/08 07:26:11 vasiljevic Exp $
 * ----------------------------------------------------------------------------
 */

#include "tclThread.h"

/*
 * Some functionality within the package depens on the Tcl version.
 */

#if ((TCL_MAJOR_VERSION<8) || (TCL_MAJOR_VERSION>=8 && TCL_MINOR_VERSION<4))
# define POST_TCL_83 0
#else
# define POST_TCL_83 1
#endif

/*
 * Allow for some command/namespace customization.
 */

#ifdef NS_AOLSERVER
# include <ns.h>
# define NS "Thread::"
#else
# define NS "thread::"
#endif

/* 
 * Access to the list of threads and to the thread send results
 * (defined below) is guarded by this mutex. 
 */

TCL_DECLARE_MUTEX(threadMutex)

static char *threadEmptyResult = "";

/*
 * Each thread has an single instance of the following structure. There
 * is one instance of this structure per thread even if that thread contains
 * multiple interpreters. The interpreter identified by this structure is
 * the main interpreter for the thread. The main interpreter is the one that
 * will process any messages received by a thread. Any interpreter can send
 * messages but only the main interpreter can receive them.
 * Unless you're not doing asynchronous script backfiring, but this is
 * another story. 
 */

typedef struct ThreadSpecificData {
    Tcl_ThreadId threadId;                /* The ID of this thread */
    Tcl_Interp *interp;                   /* Main interp for this thread */
    Tcl_Condition doOneEvent;             /* Signalled just before running 
                                           * an event from the event loop */
    int flags;                            /* One of the ThreadFlags below */
    int refCount;                         /* Used for thread reservation */
    int eventsPending;                    /* # of unprocessed events */
    int maxEventsCount;                   /* Maximum # of pending events */
    struct ThreadEventResult  *result;
    struct ThreadSpecificData *nextPtr;
    struct ThreadSpecificData *prevPtr;
} ThreadSpecificData;

static Tcl_ThreadDataKey dataKey;

#define THREAD_FLAGS_NONE          0      /* None */
#define THREAD_FLAGS_STOPPED       1      /* Thread is being stopped */
#define THREAD_FLAGS_UNWINDONERROR 2      /* Thread unwinds on script error */

#define THREAD_RESERVE             1      /* Reserves the thread */
#define THREAD_RELEASE             2      /* Releases the thread */

/*
 * This list is used to list all threads that have interpreters.
 */

static struct ThreadSpecificData *threadList = NULL;

/*
 * An instance of the following structure contains all information that is
 * passed into a new thread when the thread is created using either the
 * "thread create" Tcl command or the ThreadCreate() C function.
 */

typedef struct ThreadCtrl {
    char *script;                         /* Script to execute */
    int flags;                            /* Initial value of the "flags" 
                                           * field in ThreadSpecificData */
    Tcl_Condition condWait;               /* Condition variable used to
                                           * sync parent and child threads */
} ThreadCtrl;

/*
 * Structure holding result of the command executed in target thread.
 */

typedef struct ThreadEventResult {
    Tcl_Condition done;                   /* Set when the script completes */
    int code;                             /* Return value of the function */
    char *result;                         /* Result from the function */
    char *errorInfo;                      /* Copy of errorInfo variable */
    char *errorCode;                      /* Copy of errorCode variable */
    Tcl_ThreadId srcThreadId;             /* Id of sender, if it dies */
    Tcl_ThreadId dstThreadId;             /* Id of target, if it dies */
    struct ThreadEvent *eventPtr;         /* Back pointer */
    struct ThreadEventResult *nextPtr;    /* List for cleanup */
    struct ThreadEventResult *prevPtr;
} ThreadEventResult;

/*
 * This list links all active ThreadEventResult structures. This way
 * an exiting thread can inform all threads waiting on jobs posted to
 * his event queue that it is dying, so they might stop waiting.
 */

static ThreadEventResult *resultList;

/*
 * This is the event used to send commands to other threads.
 */

typedef struct ThreadEvent {
    Tcl_Event event;                      /* Must be first */
    struct ThreadSendData *sendData;      /* See below */
    struct ThreadClbkData *clbkData;      /* See below */
    struct ThreadEventResult *resultPtr;  /* To communicate the result back.
                                           * NULL if we don't care about it */
} ThreadEvent;

typedef int  (ThreadSendProc) _ANSI_ARGS_((Tcl_Interp*, ClientData));
typedef void (ThreadSendFree) _ANSI_ARGS_((ClientData));

static ThreadSendProc ThreadSendEval;     /* Does a regular Tcl_Eval */
static ThreadSendProc ThreadClbkSetVar;   /* Sets the named variable */

/*
 * These structures are used to communicate commands between source and target
 * threads. The ThreadSendData is used for source->target command passing,
 * while the ThreadClbkData is used for doing asynchronous callbacks.
 *
 * Important: structures below must have first three elements indentical!
 */

typedef struct ThreadSendData {
    ThreadSendProc *execProc;             /* Func to exec in remote thread */
    ClientData clientData;                /* Ptr to pass to send function */
    ThreadSendFree *freeProc;             /* Function to free client data */
     /* ---- */
    Tcl_Interp *interp;                   /* Interp to run the command */
} ThreadSendData;

typedef struct ThreadClbkData {
    ThreadSendProc *execProc;             /* The callback function */
    ClientData clientData;                /* Ptr to pass to clbk function */
    ThreadSendFree *freeProc;             /* Function to free client data */
    /* ---- */
    Tcl_Interp *interp;                   /* Interp to run the command */
    Tcl_ThreadId threadId;                /* Thread where to post callback */
    ThreadEventResult result;             /* Returns result asynchronously */
} ThreadClbkData;

#if (POST_TCL_83)

/*
 * Event used to transfer a channel between threads.
 * This requires Tcl core support added in Tcl 8.4.
 */
typedef struct TransferEvent {
    Tcl_Event event;                      /* Must be first */
    Tcl_Channel chan;                     /* The channel to transfer */
    struct TransferResult *resultPtr;     /* To communicate the result */
} TransferEvent;

typedef struct TransferResult {
    Tcl_Condition done;                   /* Set when transfer is done */
    int resultCode;                       /* Set to TCL_OK or TCL_ERROR when
                                           * the transfer is done. Def = -1 */
    char *resultMsg;                      /* Initialized to NULL. Set to a
                                           * allocated string by the targer
                                           * thread in case of an error  */
    Tcl_ThreadId srcThreadId;             /* Id of src thread, if it dies */
    Tcl_ThreadId dstThreadId;             /* Id of tgt thread, if it dies */
    struct TransferEvent *eventPtr;       /* Back pointer */
    struct TransferResult *nextPtr;
    struct TransferResult *prevPtr;
} TransferResult;

static TransferResult *transferList;

#endif /* POST_TCL_83 */

/*
 * This is for simple error handling 
 * when a thread script exits badly.
 */

static Tcl_ThreadId errorThreadId; /* Id of thread to post error message */
static char *errorProcString;      /* Tcl script to run when reporting error */

#ifdef BUILD_thread
# undef  TCL_STORAGE_CLASS
# define TCL_STORAGE_CLASS DLLEXPORT
#endif

/*
 * Miscelaneous functions used within this file
 */

static Tcl_EventDeleteProc ThreadDeleteEvent;

static Tcl_ThreadCreateType 
NewThread         _ANSI_ARGS_((ClientData clientData));

static ThreadSpecificData* 
ThreadExistsInner _ANSI_ARGS_((Tcl_ThreadId id));

static int  
ThreadCreate      _ANSI_ARGS_((Tcl_Interp *interp,
                               CONST char *script,
                               int stacksize,
                               int flags));
static int  
ThreadSend        _ANSI_ARGS_((Tcl_Interp *interp, 
                               Tcl_ThreadId id, 
                               ThreadSendData *sendPtr,
                               ThreadClbkData *clbkPtr,
                               int wait));
static void 
ThreadSetResult   _ANSI_ARGS_((Tcl_Interp *interp,
                               int code,
                               ThreadEventResult *resultPtr));
static int  
ThreadGetOption   _ANSI_ARGS_((Tcl_Interp *interp,
                               Tcl_ThreadId id,
                               char *option, 
                               Tcl_DString *ds));
static int  
ThreadSetOption   _ANSI_ARGS_((Tcl_Interp *interp,
                               Tcl_ThreadId id,
                               char *option,
                               char *value));
static int  
ThreadReserve     _ANSI_ARGS_((Tcl_Interp *interp, 
                               Tcl_ThreadId id, 
                               int operation));
static int  
ThreadEventProc   _ANSI_ARGS_((Tcl_Event *evPtr, 
                               int mask));
static int  
ThreadWait        _ANSI_ARGS_((void));

static int  
ThreadExists      _ANSI_ARGS_((Tcl_ThreadId id));

static int  
ThreadList        _ANSI_ARGS_((Tcl_Interp *interp));

static void 
ThreadErrorProc   _ANSI_ARGS_((Tcl_Interp *interp));

static void 
ThreadFreeProc    _ANSI_ARGS_((ClientData clientData));

static void 
ThreadIdleProc    _ANSI_ARGS_((ClientData clientData));

static void 
ThreadExitProc    _ANSI_ARGS_((ClientData clientData));

static void
ListRemove        _ANSI_ARGS_((ThreadSpecificData *tsdPtr));

static void 
ListRemoveInner   _ANSI_ARGS_((ThreadSpecificData *tsdPtr));

static void 
ListUpdateInner   _ANSI_ARGS_((ThreadSpecificData *tsdPtr));

#if (POST_TCL_83)
static int 
ThreadJoin        _ANSI_ARGS_((Tcl_Interp *interp,
                               Tcl_ThreadId id));
static int 
ThreadTransfer    _ANSI_ARGS_((Tcl_Interp *interp, 
                               Tcl_ThreadId id,
                               Tcl_Channel chan));
static int 
TransferEventProc _ANSI_ARGS_((Tcl_Event *evPtr, 
                               int mask));
#endif

/*
 * Functions implementing Tcl commands
 */

static Tcl_ObjCmdProc ThreadCreateObjCmd;
static Tcl_ObjCmdProc ThreadReserveObjCmd;
static Tcl_ObjCmdProc ThreadSendObjCmd;
static Tcl_ObjCmdProc ThreadUnwindObjCmd;
static Tcl_ObjCmdProc ThreadExitObjCmd;
static Tcl_ObjCmdProc ThreadIdObjCmd;
static Tcl_ObjCmdProc ThreadNamesObjCmd;
static Tcl_ObjCmdProc ThreadWaitObjCmd;
static Tcl_ObjCmdProc ThreadExistsObjCmd;
static Tcl_ObjCmdProc ThreadConfigureObjCmd;
static Tcl_ObjCmdProc ThreadErrorProcObjCmd;

#if (POST_TCL_83)
static Tcl_ObjCmdProc ThreadJoinObjCmd;
static Tcl_ObjCmdProc ThreadTransferObjCmd;
#endif


/*
 *----------------------------------------------------------------------
 *
 * Thread_Init --
 *
 *  Initialize the thread commands.
 *
 * Results:
 *      TCL_OK if the package was properly initialized.
 *
 * Side effects:
 *  Add "thread::*" and "tsv::*" commands to the interp.
 *
 *----------------------------------------------------------------------
 */

EXTERN int
Thread_Init(interp)
    Tcl_Interp *interp; /* The current Tcl interpreter */
{
    Tcl_Obj *boolObjPtr;
    char *msg;
    int boolVar, maj, min, ptch, type, subset83;

    if (Tcl_InitStubs(interp, "8.3", 0) == NULL) {
        return TCL_ERROR;
    }

    Tcl_GetVersion(&maj, &min, &ptch, &type);

    if ((maj == 8) && (min == 3) && (ptch < 1)) {
        /*
         * Truely depends on 8.3.1+ with the new Tcl_CreateThread API
         */
        msg = "This extension can't run in a Tcl core less than 8.3.1";
        Tcl_SetStringObj(Tcl_GetObjResult(interp), msg, -1);
        return TCL_ERROR;
    }

    /*
     * Tcl 8.3.[1,*) is limited to a subset of commands and provides a
     * different package version of the thread extension.  This way of
     * dynamically (at runtime) adjusting what commands are added to the
     * interp based on the core version, is needed to prevent accessing 
     * stubs functions that don't exist in 8.3.[1,*) and to maintain the
     * proper package version provided to Tcl for a consistent interface.
     */

#if (POST_TCL_83)
    subset83 = ((maj == 8) && (min == 3)) ? 1 : 0;
#else
    subset83 = 1;
#endif

    boolObjPtr = Tcl_GetVar2Ex(interp, "::tcl_platform", "threaded", 0);

    if (boolObjPtr == NULL
            || Tcl_GetBooleanFromObj(interp, boolObjPtr, &boolVar) != TCL_OK
            || boolVar == 0) {
        msg = "Tcl core wasn't compiled for multithreading.";
        Tcl_SetObjResult(interp, Tcl_NewStringObj(msg, -1));
        return TCL_ERROR;        
    }

    /*
     * We seem to have a Tcl core compiled with threads enabled.
     */

    TCL_CMD(interp, NS"create",    ThreadCreateObjCmd);
    TCL_CMD(interp, NS"send",      ThreadSendObjCmd);
    TCL_CMD(interp, NS"exit",      ThreadExitObjCmd);
    TCL_CMD(interp, NS"unwind",    ThreadUnwindObjCmd);
    TCL_CMD(interp, NS"id",        ThreadIdObjCmd);
    TCL_CMD(interp, NS"names",     ThreadNamesObjCmd);
    TCL_CMD(interp, NS"exists",    ThreadExistsObjCmd);
    TCL_CMD(interp, NS"wait",      ThreadWaitObjCmd);
    TCL_CMD(interp, NS"configure", ThreadConfigureObjCmd);
    TCL_CMD(interp, NS"errorproc", ThreadErrorProcObjCmd);

    TCL_CMD1(interp,NS"preserve",  ThreadReserveObjCmd, THREAD_RESERVE);
    TCL_CMD1(interp,NS"release",   ThreadReserveObjCmd, THREAD_RELEASE);

    if (!subset83) {
#if (POST_TCL_83)
    TCL_CMD(interp, NS"join",      ThreadJoinObjCmd);
    TCL_CMD(interp, NS"transfer",  ThreadTransferObjCmd);
#endif
    }

    /*
     * Add shared variable commands
     */
    
    Sv_Init(interp);
    
    /*
     * Add commands to access thread
     * synchronization primitives.
     */
    
    Sp_Init(interp);
    
    /*
     * Set the package version based 
     * on the core features available.
     */

    return Tcl_PkgProvide(interp, "Thread", 
            (subset83) ? THREAD_VERSION_SUBSET83 : THREAD_VERSION);
}

/*
 *----------------------------------------------------------------------
 *
 * Thread_SafeInit --
 *
 *  This function is called from within initialization of the safe
 *  Tcl interpreter.
 *
 * Results:
 *  Standard Tcl result
 *
 * Side effects:
 *  Commands added to the current interpreter,
 *
 *----------------------------------------------------------------------
 */

EXTERN int
Thread_SafeInit(interp)
    Tcl_Interp *interp;
{
    /*
     * FIXME: shouldn't we disable threading for safe interp?
     */
    return Thread_Init(interp);
}

/*
 *----------------------------------------------------------------------
 *
 * Init --
 *
 *  Make sure internal list of threads is created.
 *
 * Results:
 *  None
 *
 * Side effects:
 *  The list of threads is initialzied to include the current thread.
 *
 *----------------------------------------------------------------------
 */

static void
Init(interp)
    Tcl_Interp *interp;         /* Current interpreter. */
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    if (tsdPtr->interp == (Tcl_Interp*)NULL) {
        Tcl_MutexLock(&threadMutex);

        memset(tsdPtr, 0, sizeof(ThreadSpecificData));
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
 *  This procedure is invoked to process the "thread::create" Tcl
 *  command. See the user documentation for details on what it does.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadCreateObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
{
    char *script = NULL;
    int flags = TCL_THREAD_NOFLAGS;

    Init(interp);

    /* 
     * Syntax: thread::create ?-joinable? ?script?
     */

    if (objc == 1) {
        /* Neither option nor script available.
         */
        script = NS"wait"; /* Enters the event loop */
        
    } else if (objc == 2) {
        /* Either option or script possible, not both.
         */
        char *arg = Tcl_GetStringFromObj(objv[1], NULL);
        if (OPT_CMP(arg, "-joinable")) {
            flags |= TCL_THREAD_JOINABLE;
            script = NS"wait"; /* Enters the event loop */
        } else {
            script = arg;
        }
    } else if (objc == 3) {
        /* Enough information for both flag and script.
         */
        if (OPT_CMP(Tcl_GetStringFromObj(objv[1], NULL), "-joinable")) {
            flags |= TCL_THREAD_JOINABLE;
        } else {
            Tcl_WrongNumArgs(interp, 1, objv, "?-joinable? ?script?");
            return TCL_ERROR;
        }
        script = Tcl_GetStringFromObj(objv[2], NULL);
    } else {
        Tcl_WrongNumArgs(interp, 1, objv, "?-joinable? ?script?");
        return TCL_ERROR;
    }
    
    return ThreadCreate(interp, script, TCL_THREAD_STACK_DEFAULT, flags);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadReserveObjCmd --
 *
 *  This procedure is invoked to process the "thread::preserve" and
 *  "thread::release" Tcl commands, depending on the flag passed by
 *  the ClientData argument. See the user documentation for details 
 *  on what those command do.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadReserveObjCmd(flag, interp, objc, objv)
    ClientData  flag;           /* THREAD_RESERVE | THREAD_RELEASE */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
{
    long id;
    Tcl_ThreadId threadId;

    Init(interp);

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "threadId");
        return TCL_ERROR;
    }
    if (Tcl_GetLongFromObj(interp, objv[1], &id) != TCL_OK) {
        return TCL_ERROR;
    }

    threadId = (Tcl_ThreadId)id;

    return ThreadReserve(interp, threadId, (int)flag);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadUnwindObjCmd --
 *
 *  This procedure is invoked to process the "thread::unwind" Tcl 
 *  command. See the user documentation for details on what it does.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadUnwindObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
{
    Init(interp);

    if (objc > 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        return TCL_ERROR;
    }

    return ThreadReserve(interp, 0, THREAD_RELEASE);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadExitObjCmd --
 *
 *  This procedure is invoked to process the "thread::exit" Tcl 
 *  command.  This causes an unconditional close of the thread
 *  and is GUARENTEED to cause memory leaks.  Use this with caution.
 *
 * Results:
 *  Doesn't actually return.
 *
 * Side effects:
 *  Lots.  improper clean up of resources.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadExitObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
{

    Init(interp);
    ListRemove(NULL);
    Tcl_ExitThread(666);

    return TCL_OK; /* NOT REACHED */
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadIdObjCmd --
 *
 *  This procedure is invoked to process the "thread::id" Tcl command.
 *  This returns the ID of the current thread.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadIdObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
{
    Init(interp);
    
    if (objc > 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        return TCL_ERROR;
    }
    
    Tcl_SetLongObj(Tcl_GetObjResult(interp), (long)Tcl_GetCurrentThread());

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadNamesObjCmd --
 *
 *  This procedure is invoked to process the "thread::names" Tcl 
 *  command. This returns a list of all known thread IDs.  
 *  These are only threads created via this module (e.g., not 
 *  driver threads or the notifier).
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadNamesObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
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
 *  This procedure is invoked to process the "thread::send" Tcl 
 *  command. This sends a script to another thread for execution.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadSendObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
{
    int ret, len, vlen = 0, ii = 0, wait = 1;
    long threadId;
    char *script, *arg, *var = NULL;

    ThreadClbkData *clbkPtr = NULL;
    ThreadSendData *sendPtr = NULL;

    Init(interp);

    /*
     * Syntax: thread::send ?-async? threadId script ?varName?
     */

    if (objc < 3 || objc > 5) {
        goto usage;
    }
    while(++ii < objc) {
        arg = Tcl_GetStringFromObj(objv[ii], NULL);
        if (*arg != '-') {
            break;
        }
        if (OPT_CMP(arg, "-async")) {
            wait = 0;
        } else {
            goto usage;
        }
    }

    if (ii >= objc) {
        goto usage;
    }
    if (Tcl_GetLongFromObj(interp, objv[ii], &threadId) != TCL_OK) {
        return TCL_ERROR;
    }
    if (++ii >= objc) {
        goto usage;
    }

    script = Tcl_GetStringFromObj(objv[ii], &len);
    if (++ii < objc) {
        var = Tcl_GetStringFromObj(objv[ii], &vlen);
    }
    if (var && !wait) {
        if (threadId == (long)Tcl_GetCurrentThread()) {
            /*
             * FIXME: Do something for callbacks to self
             */
            Tcl_SetResult(interp, "can't notify self", TCL_STATIC);
            return TCL_ERROR;
        }

        /*
         * Prepare record for the callback. This is asynchronously
         * posted back to us when the target thread finishes processing.
         * We should do a vwait on the "var" to get notified.
         */
        
        clbkPtr = (ThreadClbkData*)Tcl_Alloc(sizeof(ThreadClbkData));
        clbkPtr->execProc   = ThreadClbkSetVar;
        clbkPtr->freeProc   = (ThreadSendFree*)Tcl_Free;
        clbkPtr->interp     = interp;
        clbkPtr->threadId   = Tcl_GetCurrentThread();
        clbkPtr->clientData = (ClientData)strcpy(Tcl_Alloc(1+vlen), var);
    }

    /*
     * Prepare job record for the target thread
     */

    sendPtr = (ThreadSendData*)Tcl_Alloc(sizeof(ThreadSendData));
    sendPtr->interp     = NULL; /* Signal to use thread main interp */
    sendPtr->execProc   = ThreadSendEval;
    sendPtr->freeProc   = (ThreadSendFree*)Tcl_Free;
    sendPtr->clientData = (ClientData)strcpy(Tcl_Alloc(1+len), script);

    ret = ThreadSend(interp, (Tcl_ThreadId)threadId, sendPtr, clbkPtr, wait);

    if (var && wait) {
        
        /*
         * Leave job's result in passed variable
         * and return the code, like "catch" does.
         */
        
        Tcl_Obj *resultObj = Tcl_GetObjResult(interp);
        if (!Tcl_SetVar2Ex(interp, var, NULL, resultObj, TCL_LEAVE_ERR_MSG)) {
            return TCL_ERROR;
        }
        Tcl_SetObjResult(interp, Tcl_NewIntObj(ret));
        return TCL_OK;
    }

    return ret;

usage:
    Tcl_WrongNumArgs(interp, 1, objv, "?-async? id script ?varName?");
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadWaitObjCmd --
 *
 *  This procedure is invoked to process the "thread::wait" Tcl 
 *  command. This enters the event loop.
 *
 * Results:
 *  Standard Tcl result.
 *
 * Side effects:
 *  Enters the event loop.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadWaitObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
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
 *  This procedure is invoked to process the "thread::errorproc" 
 *  command. This registers a procedure to handle thread errors.
 *  Empty string as the name of the procedure will reset the 
 *  default behaviour, which is writing to standard error channel.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  Registers an errorproc.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadErrorProcObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
{
    int len;
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
        proc = Tcl_GetStringFromObj(objv[1], &len);
        if (len == 0) {
            errorProcString = NULL;
        } else {
            errorProcString = Tcl_Alloc(1+strlen(proc));
            strcpy(errorProcString, proc);
        }
    }
    Tcl_MutexUnlock(&threadMutex);

    return TCL_OK;
}

#if (POST_TCL_83)
/*
 *----------------------------------------------------------------------
 *
 * ThreadJoinObjCmd --
 *
 *  This procedure is invoked to process the "thread::join" Tcl 
 *  command. See the user documentation for details on what it does.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadJoinObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
{

    long id;
    Tcl_ThreadId threadId;

    Init(interp);

    /*
     * Syntax of 'join': id
     */

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "id");
        return TCL_ERROR;
    }
    if (Tcl_GetLongFromObj(interp, objv [1], &id) != TCL_OK) {
        return TCL_ERROR;
    }
    
    threadId = (Tcl_ThreadId)id;

    return ThreadJoin(interp, threadId);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadTransferObjCmd --
 *
 *  This procedure is invoked to process the "thread::transfer" Tcl
 *  command. See the user documentation for details on what it does.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadTransferObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
{

    long        id;
    Tcl_Channel chan;

    Init(interp);

    /*
     * Syntax of 'transfer': id channel
     */

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "id channel");
        return TCL_ERROR;
    }
    if (Tcl_GetLongFromObj(interp, objv [1], &id) != TCL_OK) {
        return TCL_ERROR;
    }
    chan = Tcl_GetChannel(interp, Tcl_GetString(objv[2]), NULL);
    if (chan == (Tcl_Channel) NULL) {
        return TCL_ERROR;
    }
    
    return ThreadTransfer(interp, (Tcl_ThreadId)id, chan);
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * ThreadExistsObjCmd --
 *
 *  This procedure is invoked to process the "thread::exists" Tcl
 *  command. See the user documentation for details on what it does.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadExistsObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
{
    long id;
    Tcl_ThreadId threadId;

    Init(interp);

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "id");
        return TCL_ERROR;
    }
    if (Tcl_GetLongFromObj(interp, objv[1], &id) != TCL_OK) {
        return TCL_ERROR;
    }

    threadId = (Tcl_ThreadId)id;
    Tcl_SetBooleanObj(Tcl_GetObjResult(interp), ThreadExists(threadId));
    
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadConfigureObjCmd --
 *
 *	This procedure is invoked to process the Tcl "thread::configure"
 *  command. See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *----------------------------------------------------------------------
 */
static int
ThreadConfigureObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
{
    char *option, *value;
    Tcl_ThreadId threadId;      /* Id of the thread to configure */ 
    int i;                      /* Iterate over arg-value pairs. */
    Tcl_DString ds;			    /* DString to hold result of
                                 * calling GetThreadOption. */

    if (objc < 2 || (objc % 2 == 1 && objc != 3)) {
        Tcl_WrongNumArgs(interp, 1, objv, "threadlId ?optionName? "
                         "?value? ?optionName value?...");
        return TCL_ERROR;
    }

    Init(interp);

    if (Tcl_GetLongFromObj(interp, objv[1], (long*)&threadId) != TCL_OK) {
        return TCL_ERROR;
    }
    if (ThreadExists(threadId) == 0) {
        Tcl_SetResult(interp, "invalid thread id", TCL_STATIC);
        return TCL_OK;
    }
    if (objc == 2) {
        Tcl_DStringInit(&ds);
        if (ThreadGetOption(interp, threadId, NULL, &ds) != TCL_OK) {
            Tcl_DStringFree(&ds);
            return TCL_ERROR;
        }
        Tcl_DStringResult(interp, &ds);
        return TCL_OK;
    }
    if (objc == 3) {
        Tcl_DStringInit(&ds);
        option = Tcl_GetString(objv[2]);
        if (ThreadGetOption(interp, threadId, option, &ds) != TCL_OK) {
            Tcl_DStringFree(&ds);
            return TCL_ERROR;
        }
        Tcl_DStringResult(interp, &ds);
        return TCL_OK;
    }
    for (i = 3; i < objc; i += 2) {
        option = Tcl_GetString(objv[i-1]);
        value  = Tcl_GetString(objv[i]);
        if (ThreadSetOption(interp, threadId, option, value) != TCL_OK) {
            return TCL_ERROR;
        }
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadSendEval --
 *
 *  Evaluates Tcl script passed from source to target thread.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

static int 
ThreadSendEval(interp, clientData)
    Tcl_Interp *interp;
    ClientData clientData;
{
    ThreadSendData *sendPtr = (ThreadSendData*)clientData;
    char *script = (char*)sendPtr->clientData;

    return Tcl_EvalEx(interp, script, -1, TCL_EVAL_GLOBAL);
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadClbkSetVar --
 *
 *  Sets the Tcl variable in the source thread, as the result
 *  of the asynchronous callback.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  New Tcl variable may be created
 *
 *----------------------------------------------------------------------
 */

static int 
ThreadClbkSetVar(interp, clientData)
    Tcl_Interp *interp;
    ClientData clientData;
{
    ThreadClbkData *clbkPtr = (ThreadClbkData*)clientData;
    char *var = (char*)clbkPtr->clientData;
    Tcl_Obj *valObj;
    ThreadEventResult *resultPtr = &clbkPtr->result;

    /*
     * Get the result of the posted command.
     * We will use it to fill-in the result variable.
     */

    valObj = Tcl_NewStringObj(resultPtr->result, -1);
    if (resultPtr->result != threadEmptyResult) {
        Tcl_Free(resultPtr->result);
    }

    /*
     * Set the result variable
     */

    if (Tcl_SetVar2Ex(interp, var, NULL, valObj, 
                      TCL_GLOBAL_ONLY | TCL_LEAVE_ERR_MSG) == NULL) {
        return TCL_ERROR;
    }

    /*
     * FIXME: what should be the proper way of informing the waiter
     * thread which might be vwait'ing for (this) result that the
     * error is pending ? Should we trigger the background error stuff ?
     */

    if (resultPtr->code != TCL_OK) {
        if (resultPtr->errorCode) {
            var = "errorCode";
            Tcl_SetVar(interp, var, resultPtr->errorCode, TCL_GLOBAL_ONLY);
            Tcl_Free((char*)resultPtr->errorCode);
        }
        if (resultPtr->errorInfo) {
            var = "errorInfo";
            Tcl_SetVar(interp, var, resultPtr->errorInfo, TCL_GLOBAL_ONLY);
            Tcl_Free((char*)resultPtr->errorInfo);
        }
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadCreate --
 *
 *  This procedure is invoked to create a thread containing an 
 *  interp to run a script. This returns after the thread has
 *  started executing.
 *
 * Results:
 *  A standard Tcl result, which is the thread ID.
 *
 * Side effects:
 *  Create a thread.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadCreate(interp, script, stacksize, flags)
    Tcl_Interp *interp;         /* Current interpreter. */
    CONST char *script;         /* Script to execute */
    int         stacksize;      /* Zero for default size */
    int         flags;          /* Zero for no flags */
{
    ThreadCtrl ctrl;
    Tcl_ThreadId id;

    ctrl.script   = (char *)script;
    ctrl.condWait = NULL;
    ctrl.flags    = 0;

    Tcl_MutexLock(&threadMutex);
    if (Tcl_CreateThread(&id, NewThread, (ClientData)&ctrl, stacksize,
            flags) != TCL_OK) {
        Tcl_MutexUnlock(&threadMutex);
        Tcl_SetResult(interp, "can't create a new thread", TCL_STATIC);
        Tcl_Free((void *)ctrl.script);
        return TCL_ERROR;
    }

    /*
     * Wait for the thread to start because it is using something
     * on our stack!
     */

    while (ctrl.script) {
        Tcl_ConditionWait(&ctrl.condWait, &threadMutex, NULL);
    }
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
    Tcl_Interp *interp;
    int result;
    char *threadEvalScript;
    int maj, min, ptch, type;

    /*
     * Initialize the interpreter.
     */

#ifdef NS_AOLSERVER
    interp = (Tcl_Interp*)Ns_TclAllocateInterp(NULL);
#else
    interp = Tcl_CreateInterp();
    result = Tcl_Init(interp);

    /*
     *  Tcl_Init() under 8.3.[1,2] and 8.4a1 doesn't work under threads.
     */

    Tcl_GetVersion(&maj, &min, &ptch, &type);
    if (!((maj == 8) && (min == 3) && (ptch <= 2)) &&
        !((maj == 8) && (min == 4) && (ptch == 1)  &&
          (type == TCL_ALPHA_RELEASE)) && (result != TCL_OK)) {
        Tcl_ConditionNotify(&ctrlPtr->condWait);
        ThreadErrorProc(interp);
        Tcl_ExitThread(result);
    }
    result = Thread_Init(interp);
#endif

    /*
     * Clear private state
     */
    
    memset(tsdPtr, 0, sizeof(ThreadSpecificData));
    tsdPtr->interp = interp;

    /*
     * Update the list of threads.
     */

    Tcl_MutexLock(&threadMutex);
    ListUpdateInner(tsdPtr);

    /*
     * We need to keep a pointer to the alloc'ed mem of the script
     * we are eval'ing, for the case that we exit during evaluation
     */

    threadEvalScript = (char*)Tcl_Alloc(1+strlen(ctrlPtr->script));
    strcpy(threadEvalScript, ctrlPtr->script);
    Tcl_CreateThreadExitHandler(ThreadExitProc,(ClientData)threadEvalScript);

    /*
     * Notify the parent we are alive.
     */

    ctrlPtr->script = NULL;
    Tcl_ConditionNotify(&ctrlPtr->condWait);
    Tcl_MutexUnlock(&threadMutex);

    /*
     * Run the script.
     */

    Tcl_Preserve((ClientData)tsdPtr->interp);
    result = Tcl_Eval(tsdPtr->interp, threadEvalScript);
    if (result != TCL_OK) {
        ThreadErrorProc(tsdPtr->interp);
    }
    Tcl_Release((ClientData)tsdPtr->interp);
    
    /*
     * Clean up. Note: add something like TlistRemove for transfer list.
     */

    if (tsdPtr->doOneEvent) {
        Tcl_ConditionFinalize(&tsdPtr->doOneEvent);
    }

    ListRemove(tsdPtr);

    /*
     * It is up to all other extensions, including Tk, to be responsible
     * for their own events when they receive their Tcl_CallWhenDeleted
     * notice when we delete this interp.
     */

#ifdef NS_AOLSERVER
    Ns_TclDeAllocateInterp(tsdPtr->interp);
#else
    Tcl_DeleteInterp(tsdPtr->interp);
#endif

    /*
     * Tcl_ExitThread calls Tcl_FinalizeThread() indirectly which calls
     * ThreadExitHandlers and cleans the notifier as well as other sub-
     * systems that save thread state data.
     */

    Tcl_ExitThread(result);

    TCL_THREAD_CREATE_RETURN;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadErrorProc --
 *
 *  Send a message to the thread willing to hear about errors.
 *
 * Results:
 *  None
 *
 * Side effects:
 *  Send an event.
 *
 *----------------------------------------------------------------------
 */

static void
ThreadErrorProc(interp)
    Tcl_Interp *interp;         /* Interp that failed */
{
    ThreadSendData *sendPtr;
    char *errorInfo, *argv[3], buf[10];

    sprintf(buf, "%ld", (long) Tcl_GetCurrentThread());
    errorInfo = Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);

    if (errorProcString == NULL) {
#ifdef NS_AOLSERVER
        Ns_Log(Error, "%s\n%s", Tcl_GetStringResult(interp), errorInfo);
#else
        Tcl_Channel errChannel = Tcl_GetStdChannel(TCL_STDERR);

        Tcl_WriteChars(errChannel, "Error from thread ", -1);
        Tcl_WriteChars(errChannel, buf, -1);
        Tcl_WriteChars(errChannel, "\n", 1);
        Tcl_WriteChars(errChannel, errorInfo, -1);
        Tcl_WriteChars(errChannel, "\n", 1);
#endif
    } else {
        argv[0] = errorProcString;
        argv[1] = buf;
        argv[2] = errorInfo;

        sendPtr = (ThreadSendData*)Tcl_Alloc(sizeof(ThreadSendData));
        sendPtr->execProc   = ThreadSendEval;
        sendPtr->freeProc   = (ThreadSendFree*)Tcl_Free;
        sendPtr->clientData = (ClientData) Tcl_Merge(3, argv);

        ThreadSend(interp, errorThreadId, sendPtr, NULL, 0);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ListUpdateInner --
 *
 *  Add the thread local storage to the list. This assumes the caller
 *  has obtained the threadMutex.
 *
 * Results:
 *  None
 *
 * Side effects:
 *  Add the thread local storage to its list.
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
    tsdPtr->nextPtr  = threadList;

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
 *  Remove the thread local storage from its list. This grabs the
 *  mutex to protect the list.
 *
 * Results:
 *  None
 *
 * Side effects:
 *  Remove the thread local storage from its list.
 *
 *----------------------------------------------------------------------
 */

static void
ListRemove(tsdPtr)
    ThreadSpecificData *tsdPtr;
{
    Tcl_MutexLock(&threadMutex);
    ListRemoveInner(tsdPtr);
    Tcl_MutexUnlock(&threadMutex);
}

/*
 *----------------------------------------------------------------------
 *
 * ListRemoveInner --
 *
 *  Remove the thread local storage from its list.
 *
 * Results:
 *  None
 *
 * Side effects:
 *  Remove the thread local storage from its list.
 *
 *----------------------------------------------------------------------
 */

static void
ListRemoveInner(tsdPtr)
    ThreadSpecificData *tsdPtr;
{
    if (tsdPtr == NULL) {
        tsdPtr = TCL_TSD_INIT(&dataKey);
    }
    if (tsdPtr->prevPtr) {
        tsdPtr->prevPtr->nextPtr = tsdPtr->nextPtr;
    } else {
        threadList = tsdPtr->nextPtr;
    }
    if (tsdPtr->nextPtr) {
        tsdPtr->nextPtr->prevPtr = tsdPtr->prevPtr;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadList --
 *
 *  Return a list of threads running Tcl interpreters.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadList(interp)
    Tcl_Interp *interp;
{
    ThreadSpecificData *tsdPtr;
    Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);
    
    Tcl_MutexLock(&threadMutex);
    for (tsdPtr = threadList; tsdPtr; tsdPtr = tsdPtr->nextPtr) {
        if (!(tsdPtr->flags & THREAD_FLAGS_STOPPED)) {
            long id = (long)tsdPtr->threadId;
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewLongObj(id));
        }
    }
    Tcl_MutexUnlock(&threadMutex);
    Tcl_SetObjResult(interp, listObj);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadExists --
 *
 *  Test wether a thread given by it's id is known to us.
 *
 * Results:
 *  Pointer to thread specific data structure or
 *  NULL if no thread with given ID found
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadExists(threadId)
     Tcl_ThreadId threadId;
{
    ThreadSpecificData *tsdPtr;
    
    Tcl_MutexLock(&threadMutex);
    tsdPtr = ThreadExistsInner(threadId);
    Tcl_MutexUnlock(&threadMutex);
    
    return tsdPtr != NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadExistsInner --
 *
 *  Test wether a thread given by it's id is known to us. Assumes 
 *  caller holds the thread mutex.
 *
 * Results:
 *  Pointer to thread specific data structure or
 *  NULL if no thread with given ID found
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static ThreadSpecificData *
ThreadExistsInner(threadId)
    Tcl_ThreadId threadId;              /* Thread id to look for. */
{
    ThreadSpecificData *tsdPtr;
    
    for (tsdPtr = threadList; tsdPtr; tsdPtr = tsdPtr->nextPtr) {
        if (tsdPtr->threadId == threadId
                && !(tsdPtr->flags & THREAD_FLAGS_STOPPED)) {
            return tsdPtr;
        }
    }

    return NULL;
}

#if (POST_TCL_83)
/*
 *----------------------------------------------------------------------
 *
 * ThreadJoin --
 *
 *  Wait for the exit of a different thread.
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  The status of the exiting thread is left in the interp result 
 *  area, but only in the case of success.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadJoin(interp, threadId)
    Tcl_Interp  *interp;        /* The current interpreter. */
    Tcl_ThreadId threadId;      /* Thread ID of other interpreter. */
{
    int ret, state;

    ret = Tcl_JoinThread(threadId, &state);

    if (ret == TCL_OK) {
        Tcl_SetIntObj (Tcl_GetObjResult (interp), state);
    } else {
        char buf [20];
        sprintf (buf, "%ld", (long)threadId);
        Tcl_AppendResult (interp, "cannot join thread ", buf, NULL);
    }

    return ret;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadTransfer --
 *
 *  Transfers the specified channel which must not be shared and has
 *  to be registered in the given interp from that location to the
 *  main interp of the specified thread.
 *
 * Results:
 * *    A standard Tcl result.
 *
 * Side effects:
 *  The thread-global lists of all known channels of both threads
 *  involved (specified and current) are modified. The channel is
 *  moved, all event handling for the channel is killed.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadTransfer(interp, id, chan)
    Tcl_Interp *interp;         /* The current interpreter. */
    Tcl_ThreadId id;            /* Thread Id of other interpreter. */
    Tcl_Channel  chan;          /* The channel to transfer */
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
     *      'TransferEventProc'. It links the channel into the
     *      thread-global list of channels for that thread, registers it
     *      in the main interp of the other thread, removes the artificial
     *      reference, at last notifies this thread of the sucessful
     *      transfer. This allows this thread then to proceed.
     */

    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    TransferEvent *evPtr;
    TransferResult *resultPtr;

    if (!Tcl_IsChannelRegistered (interp, chan)) {
        Tcl_SetResult(interp, "channel is not registered here", TCL_STATIC);
    }
    if (Tcl_IsChannelShared (chan)) {
        Tcl_SetResult(interp, "channel is shared", TCL_STATIC);
        return TCL_ERROR;
    }

    /* 
     * Verify the thread exists.
     */

    Tcl_MutexLock(&threadMutex);

    if (ThreadExistsInner(id) == NULL) {
        Tcl_MutexLock(&threadMutex);
        Tcl_SetResult(interp, "invalid thread id", TCL_STATIC);
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

    resultPtr = (TransferResult*)Tcl_Alloc(sizeof(TransferResult));
    evPtr     = (TransferEvent *)Tcl_Alloc(sizeof(TransferEvent));

    evPtr->chan       = chan;
    evPtr->event.proc = TransferEventProc;
    evPtr->resultPtr  = resultPtr;

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
    resultPtr->nextPtr  = NULL;
    resultPtr->prevPtr  = NULL;
    
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
            Tcl_Free(resultPtr->resultMsg);
        } else {
            Tcl_AppendResult(interp, "for reasons unknown", NULL);
        }

        return TCL_ERROR;
    }

    if (resultPtr->resultMsg) {
        Tcl_Free(resultPtr->resultMsg);
    }

    return TCL_OK;
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * ThreadSend --
 *
 *  Run the procedure in other thread
 *
 * Results:
 *  A standard Tcl result.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadSend(interp, id, send, clbk, wait)
    Tcl_Interp     *interp;      /* The current interpreter. */
    Tcl_ThreadId    id;          /* Thread Id of other thread. */
    ThreadSendData *send;        /* Pointer to structure with work to do */
    ThreadClbkData *clbk;        /* Opt. callback structure (may be NULL) */
    int             wait;        /* If 1, we block for the result. */
{
    ThreadSpecificData *tsdPtr  = TCL_TSD_INIT(&dataKey); /* our own */
    ThreadSpecificData *ttsdPtr = NULL; /* ... of the target thread */

    int code;
    ThreadEvent *eventPtr;
    ThreadEventResult *resultPtr;
    Tcl_ThreadId threadId = (Tcl_ThreadId)id;

    /* 
     * Verify the thread exists.
     */

    Tcl_MutexLock(&threadMutex);
    ttsdPtr = ThreadExistsInner(threadId);

    if (ttsdPtr == (ThreadSpecificData*)NULL) {
        Tcl_MutexUnlock(&threadMutex);
        ThreadFreeProc((ClientData)send);
        if (clbk) {
            ThreadFreeProc((ClientData)clbk);
        }
        Tcl_SetResult(interp, "invalid thread id", TCL_STATIC);
        return TCL_ERROR;
    }
    
    /*
     * Short circut sends to ourself.
     */

    if (threadId == Tcl_GetCurrentThread()) {
        if (wait) {
            if (tsdPtr->maxEventsCount) {
                tsdPtr->eventsPending--;
            }
            Tcl_MutexUnlock(&threadMutex);
            return (*send->execProc)(interp, send);
        } else {
            Tcl_MutexUnlock(&threadMutex);
            send->interp = interp;
            Tcl_Preserve(send->interp);
            Tcl_DoWhenIdle((Tcl_IdleProc*)ThreadIdleProc, (ClientData)send);
            return TCL_OK;
        }
    }
    
    /* 
     * Create the event for target thread event queue.
     */

    eventPtr = (ThreadEvent*)Tcl_Alloc(sizeof(ThreadEvent));
    eventPtr->sendData = send;
    eventPtr->clbkData = clbk;

    /*
     * Caller wants to be notified, so we must take care
     * it's interpreter stays alive until we've finished.
     */

    if (eventPtr->clbkData) {
        Tcl_Preserve(eventPtr->clbkData->interp);
    }

    if (!wait) {
        resultPtr              = NULL;
        eventPtr->resultPtr    = NULL;
    } else {
        resultPtr = (ThreadEventResult*)Tcl_Alloc(sizeof(ThreadEventResult));
        resultPtr->done        = (Tcl_Condition)NULL;
        resultPtr->result      = NULL;
        resultPtr->errorCode   = NULL;
        resultPtr->errorInfo   = NULL;
        resultPtr->dstThreadId = threadId;
        resultPtr->srcThreadId = Tcl_GetCurrentThread();

        resultPtr->eventPtr    = eventPtr;
        eventPtr->resultPtr    = resultPtr;

        /* 
         * Maintain the cleanup list.
         */

        resultPtr->nextPtr = resultList;
        if (resultList) {
            resultList->prevPtr = resultPtr;
        }
        resultPtr->prevPtr = NULL;
        resultList         = resultPtr;
    }

    /*
     * Queue the event and poke the other thread's notifier.
     */

    eventPtr->event.proc = ThreadEventProc;
    Tcl_ThreadQueueEvent(threadId, (Tcl_Event*)eventPtr, TCL_QUEUE_TAIL);
    Tcl_ThreadAlert(threadId);

    if (!wait) {
        /*
         * Might potentially spend some time here, until the
         * worker thread clean's up it's queue a little bit.
         */ 
        while (ttsdPtr->maxEventsCount &&
               ttsdPtr->eventsPending > ttsdPtr->maxEventsCount) {
            Tcl_ConditionWait(&ttsdPtr->doOneEvent, &threadMutex, NULL);
        }
        Tcl_MutexUnlock(&threadMutex);
        return TCL_OK;
    }
    
    /* 
     * Block on the result indefinitely.
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
    
    Tcl_MutexUnlock(&threadMutex);

    /*
     * Return result to caller
     */

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

    code = resultPtr->code;
    Tcl_SetStringObj(Tcl_GetObjResult(interp), resultPtr->result, -1);

    /*
     * Cleanup
     */

    Tcl_ConditionFinalize(&resultPtr->done);
    if (resultPtr->result != threadEmptyResult) {
        Tcl_Free(resultPtr->result);
    }
    Tcl_Free((char*)resultPtr);

    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadWait --
 *
 *  Waits for events and process them as they come, until signaled
 *  to stop.
 *
 * Results:
 *  TCL_OK always
 *
 * Side effects:
 *  Deletes any thread::send or thread::transfer events that are
 *  pending.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadWait()
{    
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    /*
     * Process events until signaled to stop.
     */

    while (!(tsdPtr->flags & THREAD_FLAGS_STOPPED)) {

        /*
         * About to service another event.
         * Wake-up eventual sleepers.
         */
        
        if (tsdPtr->maxEventsCount) {
            Tcl_MutexLock(&threadMutex);
            tsdPtr->eventsPending--;
            Tcl_ConditionNotify(&tsdPtr->doOneEvent);
            Tcl_MutexUnlock(&threadMutex);
        }

        Tcl_DoOneEvent(TCL_ALL_EVENTS);
    }

    /*
     * Splice ourselves from the thread-list early. This prevents
     * other threads from sending us more work while we're unwinding.
     */

    ListRemove(tsdPtr);

    /*
     * Now that the event processor for this thread is closing,
     * delete all pending thread::send and thread::transfer events.
     * These events are owned by us.  We don't delete anyone else's
     * events, but ours.
     */

    Tcl_DeleteEvents((Tcl_EventDeleteProc*)ThreadDeleteEvent, NULL);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadReserve --
 *
 * Results:
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

static int
ThreadReserve(interp, threadId, operation)
    Tcl_Interp *interp;                 /* Current interpreter */
    Tcl_ThreadId threadId;              /* Target thread ID */
    int operation;                      /* THREAD_RESERVE | THREAD_RELEASE */
{
    int users;
    ThreadEvent *evPtr;
    ThreadSpecificData *tsdPtr;

    Tcl_MutexLock(&threadMutex);

    /*
     * Check the given thread
     */

    if (threadId == 0) {
        tsdPtr = TCL_TSD_INIT(&dataKey);
    } else {
        tsdPtr = ThreadExistsInner(threadId);
        if (tsdPtr == (ThreadSpecificData*)NULL) {
            Tcl_MutexUnlock(&threadMutex);
            Tcl_SetResult(interp, "invalid thread id", TCL_STATIC);
            return TCL_OK;
        }
    }

    switch (operation) {
    case THREAD_RESERVE: ++tsdPtr->refCount; break;
    case THREAD_RELEASE: --tsdPtr->refCount; break;
    }

    users = tsdPtr->refCount;

    if (users <= 0) {
        
        /*
         * We're last attached user, so tear down the *target* thread
         */
        
        tsdPtr->flags |= THREAD_FLAGS_STOPPED;
        
        if (threadId /* Not current! */) {

            /*
             * Remove from the list of active threads, so nobody can post 
             * work to this thread, since it is just about to terminate.
             */
            
            ListRemoveInner(tsdPtr);
            
            /*
             * Send an dummy event, just to wake-up target thread.
             * It should immediately exit thereafter.
             */
            
            evPtr = (ThreadEvent*)Tcl_Alloc(sizeof(ThreadEvent));
            evPtr->event.proc = ThreadEventProc;
            evPtr->sendData   = NULL;
            evPtr->clbkData   = NULL;
            evPtr->resultPtr  = NULL;
            
            Tcl_ThreadQueueEvent(threadId, (Tcl_Event*)evPtr, TCL_QUEUE_TAIL);
            Tcl_ThreadAlert(threadId);
        }
    }

    Tcl_MutexUnlock(&threadMutex);
    Tcl_SetIntObj(Tcl_GetObjResult(interp), (users > 0) ? users : 0);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadEventProc --
 *
 *  Handle the event in the target thread.
 *
 * Results:
 *  Returns 1 to indicate that the event was processed.
 *
 * Side effects:
 *  Fills out the ThreadEventResult struct.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadEventProc(evPtr, mask)
    Tcl_Event *evPtr;           /* Really ThreadEvent */
    int mask;
{
    ThreadSpecificData* tsdPtr = TCL_TSD_INIT(&dataKey);

    Tcl_Interp         *interp   = NULL;
    Tcl_ThreadId        threadId = Tcl_GetCurrentThread();
    ThreadEvent        *eventPtr = (ThreadEvent*)evPtr;
    ThreadSendData      *sendPtr = eventPtr->sendData;
    ThreadClbkData      *clbkPtr = eventPtr->clbkData;
    ThreadEventResult* resultPtr = eventPtr->resultPtr;

    int code = TCL_ERROR; /* Pessimistic assumption */

    /*
     * See wether user has any preferences about which interpreter
     * to use for running this job. The job structure might indentify
     * one. If not, just use the thread's main interpreter which is
     * stored in the thread specific data structure.
     * Note that later on we might discover that we're running the
     * aync callback script. In this case, interpreter will be 
     * changed to one given in the callback.
     */

    if (sendPtr && sendPtr->interp) {
        interp = sendPtr->interp;
    } else {
        interp = tsdPtr->interp;
    }

    if (interp != NULL) {
        if (clbkPtr && clbkPtr->threadId == threadId) {
            /* Watch: this thread evaluates it's own callback. */
            interp = clbkPtr->interp;
        } else {
            Tcl_Preserve((ClientData)interp);
        }

        Tcl_ResetResult(interp);

        if (sendPtr) {
            Tcl_CreateThreadExitHandler(ThreadFreeProc, sendPtr);
            if (clbkPtr) {
                Tcl_CreateThreadExitHandler(ThreadFreeProc, clbkPtr);
            }   
            code = (*sendPtr->execProc)(interp, sendPtr);
            Tcl_DeleteThreadExitHandler(ThreadFreeProc, sendPtr);
            if (clbkPtr) {
                Tcl_DeleteThreadExitHandler(ThreadFreeProc, clbkPtr);
            }
        } else {
            code = TCL_OK;
        }
    }

    ThreadFreeProc(sendPtr);

    if (resultPtr) {

        /*
         * Report job result synchronously to waiting caller
         */

        Tcl_MutexLock(&threadMutex);
        ThreadSetResult(interp, code, resultPtr);
        Tcl_ConditionNotify(&resultPtr->done);
        Tcl_MutexUnlock(&threadMutex);

    } else if (clbkPtr && clbkPtr->threadId != threadId) {

        ThreadSendData *tmpPtr = (ThreadSendData*)clbkPtr;
        
        /*
         * Route the callback back to it's originator.
         * Do not wait for the result.
         */

        if (code != TCL_OK) {
            ThreadErrorProc(interp);
        }

        ThreadSetResult(interp, code, &clbkPtr->result);
        ThreadSend(interp, clbkPtr->threadId, tmpPtr, NULL, 0);

    } else if (code != TCL_OK) {
        /*
         * Only pass errors onto the registered error handler 
         * when we don't have a result target for this event.
         */
        ThreadErrorProc(interp);
    }

    /*
     * Mark unwind scenario for this thread if the script resulted
     * in error condition and thread has been marked to unwind
     * in such cases. This will cause thread to disappear from the
     * list of active threads, clean-up its event queue and exit.
     */

    if (code != TCL_OK && (tsdPtr->flags & THREAD_FLAGS_UNWINDONERROR)) {
        tsdPtr->flags |= THREAD_FLAGS_STOPPED;
    }

    if (interp != NULL) {
        Tcl_Release((ClientData)interp);
    }

    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadSetResult --
 *
 * Results:
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

static void
ThreadSetResult(interp, code, resultPtr)
    Tcl_Interp *interp;
    int code;
    ThreadEventResult *resultPtr;
{
    int resLen;
    char *errorCode, *errorInfo, *result;

    if (interp == NULL) {
        code      = TCL_ERROR;
        result    = "no target interp!";
        errorInfo = "";
        errorCode = "THREAD";
    } else {
        result = Tcl_GetStringResult(interp);
        if (code != TCL_OK) {
            errorCode = Tcl_GetVar(interp, "errorCode", TCL_GLOBAL_ONLY);
            errorInfo = Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
        } else {
            errorCode = errorInfo = NULL;
        }
    }
    
    resultPtr->code = code;
    resLen = strlen(result);

    resultPtr->result = (resLen) ?
            strcpy(Tcl_Alloc(1+resLen), result) : threadEmptyResult;

    if (errorCode != NULL) {
        resultPtr->errorCode = Tcl_Alloc(1+strlen(errorCode));
        strcpy(resultPtr->errorCode, errorCode);
    }
    if (errorInfo != NULL) {
        resultPtr->errorInfo = Tcl_Alloc(1+strlen(errorInfo));
        strcpy(resultPtr->errorInfo, errorInfo);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadGetOption --
 *
 * Results:
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

static int
ThreadGetOption(interp, threadId, option, dsPtr)
    Tcl_Interp *interp;
    Tcl_ThreadId threadId;
    char *option;
    Tcl_DString *dsPtr;
{
    int len;
    ThreadSpecificData *tsdPtr = NULL;

    /*
     * If the optionName is NULL it means that we want
     * a list of all options and values.
     */
    
    len = (option == NULL) ? 0 : strlen(option);

    Tcl_MutexLock(&threadMutex);

    tsdPtr = ThreadExistsInner(threadId);
    if (tsdPtr == (ThreadSpecificData*)NULL) {
        Tcl_MutexUnlock(&threadMutex);
        Tcl_SetResult(interp, "invalid thread id", TCL_STATIC);
        return TCL_ERROR;
    }
    if (len == 0 || (len > 2 && option[1] == 'e' 
                     && !strncmp(option,"-eventmark", len))) {
        char buf[16];
        if (len == 0) {
            Tcl_DStringAppendElement(dsPtr, "-eventmark");
        }
        sprintf(buf, "%d", tsdPtr->maxEventsCount);
        Tcl_DStringAppendElement(dsPtr, buf);
        if (len) {
            Tcl_MutexUnlock(&threadMutex);
            return TCL_OK;
        }
    }
    if (len == 0 || (len > 2 && option[1] == 'u' 
                     && !strncmp(option,"-unwindonerror", len))) {
        int flag = tsdPtr->flags & THREAD_FLAGS_UNWINDONERROR;
        if (len == 0) {
            Tcl_DStringAppendElement(dsPtr, "-unwindonerror");
        }
        Tcl_DStringAppendElement(dsPtr, flag ? "1" : "0");
        if (len) {
            Tcl_MutexUnlock(&threadMutex);
            return TCL_OK;
        }
    }

    Tcl_MutexUnlock(&threadMutex);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadSetOption --
 *
 * Results:
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

static int
ThreadSetOption(interp, threadId, option, value)
    Tcl_Interp *interp;
    Tcl_ThreadId threadId;
    char *option;
    char *value;
{
    int len = strlen(option);
    ThreadSpecificData *tsdPtr = NULL;

    Tcl_MutexLock(&threadMutex);

    tsdPtr = ThreadExistsInner(threadId);
    if (tsdPtr == (ThreadSpecificData*)NULL) {
        Tcl_MutexUnlock(&threadMutex);
        Tcl_SetResult(interp, "invalid thread id", TCL_STATIC);
        return TCL_ERROR;
    }
    if (len > 2 && option[1] == 'e' 
        && !strncmp(option,"-eventmark", len)) {
        if (sscanf(value, "%d", &tsdPtr->maxEventsCount) != 1) {
            Tcl_AppendResult(interp, "expected integer but got \"",
                             value, "\"", NULL);
            Tcl_MutexUnlock(&threadMutex);
            return TCL_ERROR;
        }
    } else if (len > 2 && option[1] == 'u' 
               && !strncmp(option,"-unwindonerror", len)) {
        int flag = 0;
        if (Tcl_GetBoolean(interp, value, &flag) != TCL_OK) {
            Tcl_MutexUnlock(&threadMutex);
            return TCL_ERROR;
        }
        if (flag) {
            tsdPtr->flags |=  THREAD_FLAGS_UNWINDONERROR;
        } else {
            tsdPtr->flags &= ~THREAD_FLAGS_UNWINDONERROR;
        }
    }
        
    Tcl_MutexUnlock(&threadMutex);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadIdleProc --
 *
 * Results:
 *
 * Side effects.
 *
 *----------------------------------------------------------------------
 */

static void
ThreadIdleProc(clientData)
    ClientData clientData;
{
    int ret;
    ThreadSendData *sendPtr = (ThreadSendData*)clientData;

    ret = (*sendPtr->execProc)(sendPtr->interp, sendPtr);
    if (ret != TCL_OK) {
        ThreadErrorProc(sendPtr->interp);
    }

    Tcl_Release(sendPtr->interp);
}

#if (POST_TCL_83)
/*
 *----------------------------------------------------------------------
 *
 * TransferEventProc --
 *
 *  Handle a transfer event in the target thread.
 *
 * Results:
 *  Returns 1 to indicate that the event was processed.
 *
 * Side effects:
 *  Fills out the TransferResult struct.
 *
 *----------------------------------------------------------------------
 */

static int
TransferEventProc(evPtr, mask)
    Tcl_Event *evPtr;           /* Really ThreadEvent */
    int mask;
{
    ThreadSpecificData    *tsdPtr = TCL_TSD_INIT(&dataKey);
    TransferEvent       *eventPtr = (TransferEvent *)evPtr;
    TransferResult     *resultPtr = eventPtr->resultPtr;
    Tcl_Interp            *interp = tsdPtr->interp;
    int code;
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
        
        if (Tcl_IsChannelExisting(Tcl_GetChannelName(eventPtr->chan))) {
            /*
             * Reject transfer. Channel of same name already exists in target.
             */
            code = TCL_ERROR;
            msg  = "channel already exists in target";
        } else {
            Tcl_SpliceChannel(eventPtr->chan);
            Tcl_RegisterChannel(interp, eventPtr->chan);
            Tcl_UnregisterChannel((Tcl_Interp *) NULL, eventPtr->chan);
            code = TCL_OK; /* Return success. */
        }
    }
    if (resultPtr) {
        Tcl_MutexLock(&threadMutex);
        resultPtr->resultCode = code;
        if (msg != NULL) {
            resultPtr->resultMsg = (char*)Tcl_Alloc(1+strlen (msg));
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
 *  Called when we are exiting and memory needs to be freed.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Clears up mem specified in ClientData
 *
 *----------------------------------------------------------------------
 */
static void
ThreadFreeProc(clientData)
    ClientData clientData;
{
    /*
     * This will free send and/or callback structures
     * since both are the same in the beginning.
     */

    ThreadSendData *anyPtr = (ThreadSendData*)clientData;

    if (anyPtr) {
        if (anyPtr->clientData) {
            (*anyPtr->freeProc)(anyPtr->clientData);
        }
        Tcl_Free((char*)anyPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadDeleteEvent --
 *
 *  This is called from the ThreadExitProc to delete memory related
 *  to events that we put on the queue.
 *
 * Results:
 *  1 it was our event and we want it removed, 0 otherwise.
 *
 * Side effects:
 *  It cleans up our events in the event queue for this thread.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadDeleteEvent(eventPtr, clientData)
    Tcl_Event *eventPtr;        /* Really ThreadEvent */
    ClientData clientData;      /* dummy */
{
    if (eventPtr->proc == ThreadEventProc) {
        /*
         * Regular script event. Just dispose memory
         */
        ThreadEvent *evPtr = (ThreadEvent*)eventPtr;
        if (evPtr->sendData) {
            ThreadFreeProc((ClientData)evPtr->sendData);
        }
        if (evPtr->clbkData) {
            ThreadFreeProc((ClientData)evPtr->clbkData);
        }
        return 1;
    }
#if (POST_TCL_83)
    if (eventPtr->proc == TransferEventProc) {
        /* 
         * A channel is in flight toward the thread just exiting.
         * Pass it back to the originator, if possible.
         * Else kill it.
         */
        TransferEvent* evPtr = (TransferEvent *) eventPtr;
        
        if (evPtr->resultPtr == (TransferResult *) NULL) {
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
 *  This is called when the thread exits.  
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  It unblocks anyone that is waiting on a send to this thread.
 *  It cleans up any events in the event queue for this thread.
 *
 *----------------------------------------------------------------------
 */
static void
ThreadExitProc(clientData)
    ClientData clientData;
{
    char *threadEvalScript = (char*)clientData;
    ThreadEventResult *resultPtr, *nextPtr;
    Tcl_ThreadId self = Tcl_GetCurrentThread();

#if (POST_TCL_83)
    TransferResult *tResultPtr, *tNextPtr;
#endif

    if (threadEvalScript) {
        Tcl_Free((char*)threadEvalScript);
    }
    
    Tcl_MutexLock(&threadMutex);

#ifdef NS_AOLSERVER
    /*
     * AOLserver threads get started/stopped 
     * outside the thread package, so this 
     * is the first chance to splice them out.
     */
    ListRemoveInner(NULL);
#endif
    
    /* 
     * Delete events posted to our queue while we were running.
     * For threads exiting from the thread::wait command, this 
     * has already been done in ThreadWait() function.
     * For one-shot threads, having something here is a very 
     * strange condition. It *may* happen if somebody posts us
     * an event while we were in the middle of processing some
     * lengthly user script. It is unlikely to happen, though.
     */ 

    Tcl_DeleteEvents((Tcl_EventDeleteProc*)ThreadDeleteEvent, NULL);

    /*
     * Walk the list of threads waiting for result from us 
     * and inform them that we're about to exit.
     */

    for (resultPtr = resultList; resultPtr; resultPtr = nextPtr) {
        nextPtr = resultPtr->nextPtr;
        if (resultPtr->srcThreadId == self) {
            
            /*
             * We are going away. By freeing up the result we signal
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
            Tcl_Free((char*)resultPtr);

        } else if (resultPtr->dstThreadId == self) {
            
            /*
             * Dang. The target is going away. Unblock the caller.
             * The result string must be dynamically allocated
             * because the main thread is going to call free on it.
             */
            
            char *msg = "target thread died";

            resultPtr->result = strcpy(Tcl_Alloc(1+strlen(msg)), msg);
            resultPtr->code = TCL_ERROR;
            resultPtr->errorCode = resultPtr->errorInfo = NULL;
            Tcl_ConditionNotify(&resultPtr->done);
        }
    }
#if (POST_TCL_83)
    for (tResultPtr = transferList; tResultPtr; tResultPtr = tNextPtr) {
        tNextPtr = tResultPtr->nextPtr;
        if (tResultPtr->srcThreadId == self) {
            
            /*
             * We are going away. By freeing up the result we signal
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
             * Dang.  The target is going away. Unblock the caller
             *        and deliver a failure notice.
             *
             * The result string must be dynamically allocated because
             * the main thread is going to call free on it.
             */
            
            char *msg = "target thread died";

            tResultPtr->resultMsg  = strcpy(Tcl_Alloc(1+strlen(msg)), msg); 
            tResultPtr->resultCode = TCL_ERROR;
            Tcl_ConditionNotify(&tResultPtr->done);
        }
    }
#endif
    Tcl_MutexUnlock(&threadMutex);
}

/* EOF $RCSfile: threadCmd.c,v $ */

/* Emacs Setup Variables */
/* Local Variables:      */
/* mode: C               */
/* indent-tabs-mode: nil */
/* c-basic-offset: 4     */
/* End:                  */
