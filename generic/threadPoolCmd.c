/* 
 * threadPoolCmd.c --
 *
 * This file implements the Tcl thread pools.
 *
 * Copyright (c) 2002 by Zoran Vasiljevic, Archiware GmbH
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: threadPoolCmd.c,v 1.4 2002/12/03 23:49:33 vasiljevic Exp $
 * ----------------------------------------------------------------------------
 */

#include "tclThread.h"

/*
 * Structure to maintain idle poster threads
 */

typedef struct TpoolWaiter {
    Tcl_ThreadId threadId;         /* Thread id of the current thread */
    struct TpoolWaiter *nextPtr;   /* Next structure in the list */
    struct TpoolWaiter *prevPtr;   /* Previous structure in the list */
} TpoolWaiter;

/*
 * Structure describing an instance of a thread pool.
 */

typedef struct ThreadPool {
    unsigned int tpoolId;           /* Pool id number */
    unsigned int jobId;             /* Job counter */
    int idleTime;                   /* Time in secs a worker thread idles */
    int teardown;                   /* Set to 1 to tear down the pool */
    char *initScript;               /* Script to initialize worker thread */
    int minWorkers;                 /* Minimum number or worker threads */
    int maxWorkers;                 /* Maximum number of worker threads */
    int numWorkers;                 /* Current number of worker threads */
    int idleWorkers;                /* Number of idle workers */
    int refCount;                   /* Reference counter for reserve/release */
    Tcl_Mutex mutex;                /* Pool mutex */
    Tcl_Condition cond;             /* Pool condition variable */
    struct TpoolResult *workTail;   /* Tail of the list with jobs pending*/
    struct TpoolResult *workHead;   /* Head of the list with jobs pending*/
    struct TpoolResult *doneTail;   /* Tail of the list with jobs done */
    struct TpoolResult *doneHead;   /* Head of the list with jobs done */    
    struct TpoolWaiter *waitTail;   /* Tail of the thread waiters list */
    struct TpoolWaiter *waitHead;   /* Head of the thread waiters list */
    struct ThreadPool *nextPtr;     /* Next structure in the threadpool list */
    struct ThreadPool *prevPtr;     /* Previous structure in threadpool list */
} ThreadPool;

#define TPOOL_PREFIX      "tpool"   /* Prefix to generate Tcl pool handles */
#define TPOOL_MINWORKERS  0         /* Default minimum # of worker threads */
#define TPOOL_MAXWORKERS  4         /* Default maximum # of worker threads */
#define TPOOL_IDLETIMER   0         /* Default worker thread idle timer */
#define TPOOL_RESERVE     0         /* The pool reserve flag */
#define TPOOL_RELEASE     1         /* The pool release flag */ 

/*
 * Structure for passing evaluation results
 */

typedef struct TpoolResult {
    int detached;                   /* Result is to be ignored */
    unsigned int jobId;             /* The job id of the current job */
    char *script;                   /* Script to evaluate in worker thread */
    int scriptLen;                  /* Length of the script */    
    int retcode;                    /* Tcl return code of the current job */
    char *result;                   /* Tcl result of the current job */
    char *errorCode;                /* On error: content of the errorCode */
    char *errorInfo;                /* On error: content of the errorInfo */
    Tcl_ThreadId threadId;          /* Originating thread id */
    ThreadPool *tpoolPtr;           /* Current thread pool */
    struct TpoolResult *nextPtr;
    struct TpoolResult *prevPtr;
} TpoolResult;

/*
 * Private structure for each worker/poster thread.
 */

typedef struct ThreadSpecificData {
    int stop;                       /* Set stop event; exit from event loop */
    TpoolWaiter *waitPtr;           /* Threads private idle structure */
    ThreadPool *tpoolPtr;           /* Worker thread pool association */
    Tcl_Interp *interp;             /* Worker thread interpreter */
} ThreadSpecificData;

static Tcl_ThreadDataKey dataKey;

/*
 * This global list maintains thread pools.
 */

static ThreadPool * tpoolList    = NULL;
static int          tpoolCounter = 0;
static Tcl_Mutex    listMutex    = (Tcl_Mutex)0;

/*
 * Used to represent the empty result.
 */

static char *threadEmptyResult = "";

/*
 * Functions implementing Tcl commands
 */

static Tcl_ObjCmdProc TpoolCreateObjCmd;
static Tcl_ObjCmdProc TpoolPostObjCmd;
static Tcl_ObjCmdProc TpoolWaitObjCmd;
static Tcl_ObjCmdProc TpoolGetObjCmd;
static Tcl_ObjCmdProc TpoolReserveObjCmd;

/*
 * Miscelaneous functions used within this file
 */

static int
CreateWorker   _ANSI_ARGS_((Tcl_Interp *interp, ThreadPool *tpoolPtr));

static Tcl_ThreadCreateType
TpoolWorker    _ANSI_ARGS_((ClientData clientData));

static int
RunStopEvent   _ANSI_ARGS_((Tcl_Event *evPtr, int mask));

static void
PushWork       _ANSI_ARGS_((TpoolResult *rsPtr, ThreadPool *tpoolPtr));

static TpoolResult*
PopWork       _ANSI_ARGS_((ThreadPool *tpoolPtr));

static void
PushDone      _ANSI_ARGS_((TpoolResult *rsPtr, ThreadPool *tpoolPtr));

static void
PushWaiter     _ANSI_ARGS_((TpoolWaiter *waitPtr, ThreadPool *tpoolPtr));

static TpoolWaiter*
PopWaiter      _ANSI_ARGS_((ThreadPool *tpoolPtr));

static void
SignalWaiter   _ANSI_ARGS_((ThreadPool *tpoolPtr));

static int
TpoolEval      _ANSI_ARGS_((Tcl_Interp *interp, char *script, int scriptLen,
                            TpoolResult *rsPtr));
static void
SetResult      _ANSI_ARGS_((Tcl_Interp *interp, TpoolResult *rsPtr));

static ThreadPool* 
GetTpool       _ANSI_ARGS_((char *tpoolName));

static ThreadPool* 
GetTpoolUnl    _ANSI_ARGS_((char *tpoolName));

static void
ExitHandler    _ANSI_ARGS_((ClientData clientData));

static int
TpoolReserve   _ANSI_ARGS_((Tcl_Interp *interp,ThreadPool *tpoolPtr,int what));


/*
 *----------------------------------------------------------------------
 *
 * TpoolCreateObjCmd --
 *
 *  This procedure is invoked to process the "tpool::create" Tcl 
 *  command. See the user documentation for details on what it does.
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
TpoolCreateObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
{
    unsigned int tpoolId;
    int ii, minw, maxw, idle, len;
    char buf[16], *cmd = NULL;
    ThreadPool *tpoolPtr;

    /* 
     * Syntax:  tpool::create ?-minworkers count?
     *                        ?-maxworkers count?
     *                        ?-initscript script?
     *                        ?-idletime seconds?
     */

    if (((objc-1) % 2)) {
        goto usage;
    }

    minw = TPOOL_MINWORKERS;
    maxw = TPOOL_MAXWORKERS;
    idle = TPOOL_IDLETIMER;

    /*
     * Parse the optional arguments
     */

    for (ii = 1; ii < objc; ii += 2) {
        char *opt = Tcl_GetString(objv[ii]);
        if (OPT_CMP(opt, "-minworkers")) {
            if (Tcl_GetIntFromObj(interp, objv[ii+1], &minw) != TCL_OK) {
                return TCL_ERROR;
            }
        } else if (OPT_CMP(opt, "-maxworkers")) {
            if (Tcl_GetIntFromObj(interp, objv[ii+1], &maxw) != TCL_OK) {
                return TCL_ERROR;
            }
        } else if (OPT_CMP(opt, "-idletimer")) {
            if (Tcl_GetIntFromObj(interp, objv[ii+1], &idle) != TCL_OK) {
                return TCL_ERROR;
            }
        } else if (OPT_CMP(opt, "-initscript")) {
            char *val = Tcl_GetStringFromObj(objv[ii+1], &len);
            cmd  = strcpy(Tcl_Alloc(len+1), val);
        } else {
            goto usage;
        }
    }

    if (minw > maxw) {
        minw = maxw;
    }

    /*
     * Allocate and initialize thread pool structure
     */

    tpoolPtr = (ThreadPool*)Tcl_Alloc(sizeof(ThreadPool));
    memset(tpoolPtr, 0, sizeof(ThreadPool));

    tpoolPtr->minWorkers  = minw;
    tpoolPtr->maxWorkers  = maxw;
    tpoolPtr->idleTime    = idle;
    tpoolPtr->initScript  = cmd;

    /*
     * Start the required number of worker threads.
     * Note that this is done under mutex protection
     * in order to avoid contentions on the malloc 
     * mutex, since creating a new thread might be
     * expensive in terms of allocated memory.
     */

    Tcl_MutexLock(&listMutex);

    for (ii = 0; ii < tpoolPtr->minWorkers; ii++) {
        if (CreateWorker(interp, tpoolPtr) != TCL_OK) {
            Tcl_Free((char*)tpoolPtr);
            Tcl_MutexUnlock(&listMutex);
            return TCL_ERROR;
        }
    }

    tpoolPtr->tpoolId = tpoolCounter++;
    tpoolId = tpoolPtr->tpoolId;
    SpliceIn(tpoolPtr, tpoolList);

    Tcl_MutexUnlock(&listMutex);

    /*
     * Plug the pool in the list of available pools
     */

    sprintf(buf, "%s%u", TPOOL_PREFIX, tpoolId);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(buf, -1));

    return TCL_OK;

 usage:
    Tcl_WrongNumArgs(interp, 1, objv, "?-minworkers count?"
                     " ?-maxworkers count?"
                     " ?-initcmd command? ?-idletime seconds?");
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolPostObjCmd --
 *
 *  This procedure is invoked to process the "tpool::post" Tcl 
 *  command. See the user documentation for details on what it does.
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
TpoolPostObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
{
    unsigned int jobId;
    int detached, len;
    char *tpoolName, *script;
    TpoolResult *rsPtr;
    ThreadPool *tpoolPtr;

    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    /* 
     * Syntax: tpool::post ?-detached? tpoolId script
     */

    if (objc < 3 || objc > 4) {
        goto usage;
    }
    if (objc == 4) {
        if (OPT_CMP(Tcl_GetString(objv[1]), "-detached") == 0) {
            goto usage;
        }
        detached  = 1;
        tpoolName = Tcl_GetString(objv[2]);
        script    = Tcl_GetStringFromObj(objv[3], &len);
    } else {
        detached  = 0;
        tpoolName = Tcl_GetString(objv[1]);
        script    = Tcl_GetStringFromObj(objv[2], &len);
    }

    tpoolPtr = GetTpool(tpoolName);
    if (tpoolPtr == NULL) {
        Tcl_AppendResult(interp, "can not find threadpool \"", tpoolName, 
                         "\"", NULL);
        return TCL_ERROR;
    }
    
    /*
     * Initialize per-thread private data for this caller
     */
    
    if (tsdPtr->waitPtr == NULL) {
        tsdPtr->waitPtr = (TpoolWaiter*)Tcl_Alloc(sizeof(TpoolWaiter));
        tsdPtr->waitPtr->prevPtr  = NULL;
        tsdPtr->waitPtr->nextPtr  = NULL;
        tsdPtr->waitPtr->threadId = Tcl_GetCurrentThread();
        Tcl_CreateThreadExitHandler(ExitHandler, NULL);
    }

    /*
     * Wait for an idle worker thread
     */

    Tcl_MutexLock(&tpoolPtr->mutex);
    while (tpoolPtr->idleWorkers == 0) {
        PushWaiter(tsdPtr->waitPtr, tpoolPtr);
        if (tpoolPtr->numWorkers < tpoolPtr->maxWorkers) {
            /* No more free workers; start new one */
            if (CreateWorker(interp, tpoolPtr) != TCL_OK) {
                Tcl_MutexUnlock(&tpoolPtr->mutex);
                return TCL_ERROR;
            }
        }
        /* We wait while servicing the event loop */
        Tcl_MutexUnlock(&tpoolPtr->mutex);
        tsdPtr->stop = -1;
        while(tsdPtr->stop == -1) {
            Tcl_DoOneEvent(TCL_ALL_EVENTS);
        }
        Tcl_MutexLock(&tpoolPtr->mutex);
    }

    /*
     * Create new job ticket and put it on the list
     */

    rsPtr = (TpoolResult*)Tcl_Alloc(sizeof(TpoolResult));
    memset(rsPtr, 0, sizeof(TpoolResult));

    if (detached == 0) {
        jobId = tpoolPtr->jobId++;
        rsPtr->jobId = jobId;
    }
    rsPtr->script    = strcpy(Tcl_Alloc(len+1), script);
    rsPtr->scriptLen = len;
    rsPtr->detached  = detached;
    rsPtr->threadId  = Tcl_GetCurrentThread();

    PushWork(rsPtr, tpoolPtr);

    Tcl_ConditionNotify(&tpoolPtr->cond);
    Tcl_MutexUnlock(&tpoolPtr->mutex);

    if (detached == 0) {
        Tcl_SetObjResult(interp, Tcl_NewIntObj(jobId));
    }
    
    return TCL_OK;

  usage:
    Tcl_WrongNumArgs(interp, 1, objv, "?-detached? tpoolId script");
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolWaitObjCmd --
 *
 *  This procedure is invoked to process the "tpool::wait" Tcl 
 *  command. See the user documentation for details on what it does.
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
TpoolWaitObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
{
    int ii, done, jobs, wObjc, jobId;
    char *tpoolName, *listVar = NULL;
    Tcl_Obj *waitList, *doneList, **wObjv;
    ThreadPool *tpoolPtr;
    TpoolResult *rsPtr;

    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    /* 
     * Syntax: tpool::wait tpoolId jobIdList ?listVar?
     */

    if (objc < 3 || objc > 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "tpoolId jobIdList ?listVar");
        return TCL_ERROR;
    }
    if (objc == 4) {
        listVar = Tcl_GetString(objv[3]);
    }
    if (Tcl_ListObjGetElements(interp, objv[2], &wObjc, &wObjv) != TCL_OK) {
        return TCL_ERROR;
    }
    tpoolName = Tcl_GetString(objv[1]);
    tpoolPtr  = GetTpool(tpoolName);
    if (tpoolPtr == NULL) {
        Tcl_AppendResult(interp, "can not find threadpool \"", tpoolName,
                         "\"", NULL);
        return TCL_ERROR;
    }

    if (tsdPtr->waitPtr == NULL) {
        if (listVar) {
            Tcl_SetVar2Ex(interp, listVar, NULL, Tcl_NewObj(), 0);
        }
        return TCL_OK; /* Never sent any job */
    }

    done = 0; /* Number of elements in the done list */
    jobs = 0; /* Number of our own jobs */

    doneList = Tcl_NewListObj(0, NULL);

    Tcl_MutexLock(&tpoolPtr->mutex);
    while (1) {
        waitList = Tcl_NewListObj(0, NULL);
        for (rsPtr = tpoolPtr->doneHead; rsPtr; rsPtr = rsPtr->nextPtr) {
            if (rsPtr->threadId != Tcl_GetCurrentThread()) {
                continue; /* Not our job */
            }
            for (ii = 0; ii < wObjc; ii++) {
                if (Tcl_GetIntFromObj(interp, wObjv[ii], &jobId) != TCL_OK) {
                    Tcl_MutexUnlock(&tpoolPtr->mutex);
                    return TCL_ERROR;
                }
                if (rsPtr->jobId != jobId || rsPtr->detached) {
                    continue; /* Not this job or detached job */
                }
                jobs++;
                if (rsPtr->result) {
                    done++; /* Job has been processed */
                    Tcl_ListObjAppendElement(interp, doneList, wObjv[ii]);
                } else {
                    if (listVar) {
                        Tcl_ListObjAppendElement(interp, waitList, wObjv[ii]);
                    }
                }
            }
        }
        if (done || (jobs == 0)) {
            break;
        }

        /*
         * None of the jobs done, wait for completion
         * of the next job and try again.
         */

        Tcl_DecrRefCount(waitList); 
        Tcl_MutexUnlock(&tpoolPtr->mutex);
        Tcl_DoOneEvent(TCL_ALL_EVENTS);
        Tcl_MutexLock(&tpoolPtr->mutex);
    }
    Tcl_MutexUnlock(&tpoolPtr->mutex);

    if (listVar) {
        Tcl_SetVar2Ex(interp, listVar, NULL, waitList, 0);
    }

    Tcl_SetObjResult(interp, doneList);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolGetObjCmd --
 *
 *  This procedure is invoked to process the "tpool::collect" Tcl 
 *  command. See the user documentation for details on what it does.
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
TpoolGetObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
{
    int ret, jobId;
    char *tpoolName, done, jobs, *resVar = NULL;
    ThreadPool *tpoolPtr;
    TpoolResult *rsPtr;

    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    /* 
     * Syntax: tpool::collect tpoolId jobId ?result?
     */

    if (objc < 3 || objc > 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "tpoolId jobId ?result?");
        return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[2], &jobId) != TCL_OK) {
        return TCL_ERROR;
    }
    if (objc == 4) {
        resVar = Tcl_GetString(objv[3]);
    }

    tpoolName = Tcl_GetString(objv[1]);
    tpoolPtr  = GetTpool(tpoolName);
    if (tpoolPtr == NULL) {
        Tcl_AppendResult(interp, "can not find threadpool \"", tpoolName, 
                         "\"", NULL);
        return TCL_ERROR;
    }

    /*
     * Try to locate the requested job. If none found,
     * service event loop and wait for first job ready.
     */

    if (tsdPtr->waitPtr == NULL) {
        Tcl_AppendResult(interp, "job not found", NULL);
        return TCL_ERROR;
    }

    done = 0;
    jobs = 0;

    Tcl_MutexLock(&tpoolPtr->mutex);
    while (1) {
        for (rsPtr = tpoolPtr->doneHead; rsPtr; rsPtr = rsPtr->nextPtr) {
            if (rsPtr->threadId == Tcl_GetCurrentThread()) {
                jobs++;
            }
            if (rsPtr->jobId == jobId && rsPtr->result) {
                if (rsPtr == tpoolPtr->doneTail) {
                    tpoolPtr->doneTail = rsPtr->prevPtr;
                } 
                SpliceOut(rsPtr, tpoolPtr->doneHead);
                done = 1;
                break;
            }
        }
        if (done || (jobs == 0)) {
            break;
        }

        /*
         * Job is not done, wait for completion
         * of the next job and try again.
         */

        Tcl_MutexUnlock(&tpoolPtr->mutex);
        Tcl_DoOneEvent(TCL_ALL_EVENTS);
        Tcl_MutexLock(&tpoolPtr->mutex);
    }
    Tcl_MutexUnlock(&tpoolPtr->mutex);

    if (done) {
        ret = rsPtr->retcode;
        SetResult(interp, rsPtr);
        Tcl_Free((char*)rsPtr);
        if (resVar) {
            Tcl_SetVar2Ex(interp, resVar, NULL, Tcl_GetObjResult(interp), 0);
            Tcl_SetObjResult(interp, Tcl_NewIntObj(rsPtr->retcode));
            return TCL_OK; 
        }
    } else {
        ret = TCL_OK;
        if (resVar) {
            Tcl_SetVar2Ex(interp, resVar, NULL, Tcl_NewObj(), 0);
            Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
            return TCL_OK;
        }
    }
    
    return ret;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolReserveObjCmd --
 *
 *  This procedure is invoked to process the "tpool::preserve" Tcl 
 *  command. See the user documentation for details on what it does.
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
TpoolReserveObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
{
    int ret;
    char *tpoolName;
    ThreadPool *tpoolPtr;

    /*
     * Syntax: tpool::preserve tpoolId
     */

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "tpoolId");
        return TCL_ERROR;
    }

    tpoolName = Tcl_GetString(objv[1]);

    Tcl_MutexLock(&listMutex);
    tpoolPtr  = GetTpoolUnl(tpoolName);
    if (tpoolPtr == NULL) {
        Tcl_MutexUnlock(&listMutex);
        Tcl_AppendResult(interp, "can not find threadpool \"", tpoolName, 
                         "\"", NULL);
        return TCL_ERROR;
    }

    ret = TpoolReserve(interp, tpoolPtr, TPOOL_RESERVE); 
    Tcl_MutexUnlock(&listMutex);

    return ret; 
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolReleaseObjCmd --
 *
 *  This procedure is invoked to process the "tpool::release" Tcl 
 *  command. See the user documentation for details on what it does.
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
TpoolReleaseObjCmd(dummy, interp, objc, objv)
    ClientData  dummy;          /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int         objc;           /* Number of arguments. */
    Tcl_Obj    *CONST objv[];   /* Argument objects. */
{
    int ret;
    char *tpoolName;
    ThreadPool *tpoolPtr;

    /*
     * Syntax: tpool::release tpoolId
     */

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "tpoolId");
        return TCL_ERROR;
    }

    tpoolName = Tcl_GetString(objv[1]);

    Tcl_MutexLock(&listMutex);
    tpoolPtr  = GetTpoolUnl(tpoolName);
    if (tpoolPtr == NULL) {
        Tcl_MutexUnlock(&listMutex);
        Tcl_AppendResult(interp, "can not find threadpool \"", tpoolName,
                         "\"", NULL);
        return TCL_ERROR;
    }

    ret = TpoolReserve(interp, tpoolPtr, TPOOL_RELEASE); 
    Tcl_MutexUnlock(&listMutex);

    return ret; 
}

/*
 *----------------------------------------------------------------------
 *
 * CreateWorker --
 *
 *  Creates new worker thread for the given pool. Assumes the caller
 *  hods the pool mutex.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Informs waiter thread (if any) about the new worker thread.
 *
 *----------------------------------------------------------------------
 */
static int
CreateWorker(interp, tpoolPtr)
    Tcl_Interp *interp;
    ThreadPool *tpoolPtr;
{
    Tcl_ThreadId id;
    TpoolResult result;
    Tcl_Mutex waitMutex = (Tcl_Mutex)0;

    /*
     * Creates the new worker thread
     */

    memset(&result, 0, sizeof(TpoolResult));
    result.retcode  = -1;
    result.tpoolPtr = tpoolPtr;

    if (Tcl_CreateThread(&id, TpoolWorker, (ClientData)&result,
                         TCL_THREAD_STACK_DEFAULT, 0) != TCL_OK) {
        Tcl_SetResult(interp, "can't create a new thread", TCL_STATIC);
        return TCL_ERROR;
    }

    /*
     * Wait for the thread to start because it's using
     * the argument which is on our stack.
     */

    Tcl_MutexLock(&waitMutex);
    while(result.retcode == -1) {
        Tcl_ConditionWait(&tpoolPtr->cond, &waitMutex, NULL);
    }
    Tcl_MutexUnlock(&waitMutex);
    Tcl_MutexFinalize(&waitMutex);

    /*
     * Set error-related information if the thread
     * failed to initialize correctly.
     */
    
    if (result.retcode == TCL_ERROR) {
        SetResult(interp, &result);
        return TCL_ERROR;
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolWorker --
 *
 *  This is the main function of each of the threads in the pool.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static Tcl_ThreadCreateType
TpoolWorker(clientData)
    ClientData clientData;
{    
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    TpoolResult         *rsPtr = (TpoolResult*)clientData;
    ThreadPool       *tpoolPtr = rsPtr->tpoolPtr;

    int maj, min, ptch, type, tout = 0;
    Tcl_Interp *interp;
    Tcl_Time waitTime, *idlePtr, t1, t2;

    /*
     * Initialize the Tcl interpreter
     */

#ifdef NS_AOLSERVER
    interp = (Tcl_Interp*)Ns_TclAllocateInterp(NULL);
    rsPtr->retcode = TCL_OK;
#else
    interp = Tcl_CreateInterp();
    rsPtr->retcode = Tcl_Init(interp);
 
    /*
     *  Tcl_Init() under 8.3.[1,2] and 8.4a1 doesn't work under threads.
     */

    Tcl_GetVersion(&maj, &min, &ptch, &type);
    if (!((maj == 8) && (min == 3) && (ptch <= 2))
        && !((maj == 8) && (min == 4) && (ptch == 1)
             && (type == TCL_ALPHA_RELEASE)) && (rsPtr->retcode != TCL_OK)) {
        Tcl_ConditionNotify(&tpoolPtr->cond);
        goto out;
    }
    rsPtr->retcode = Thread_Init(interp);
#endif
    
    /*
     * Initialize the interpreter
     */

    if (rsPtr->retcode != TCL_ERROR && tpoolPtr->initScript) {
        TpoolEval(interp, tpoolPtr->initScript, -1, rsPtr);
        if (rsPtr->retcode == TCL_ERROR) {
            Tcl_ConditionNotify(&tpoolPtr->cond);
            goto out;
        }
    }

    /*
     * No need to lock; caller holds the pool lock
     */

    tpoolPtr->numWorkers++;
    tsdPtr->tpoolPtr = tpoolPtr;
    tsdPtr->interp   = interp;

    if (tpoolPtr->idleTime == 0) {
        idlePtr = NULL;
    } else {
        waitTime.sec  = tpoolPtr->idleTime/1000;
        waitTime.usec = (tpoolPtr->idleTime % 1000) * 1000;
        idlePtr = &waitTime;
    }

    /*
     * Tell caller we're ready.
     */
    
    rsPtr->retcode = 0;
    Tcl_ConditionNotify(&tpoolPtr->cond);

    /*
     * Wait for jobs to arrive. Note handcrafted time test.
     * Tcl API misses the return value of the Tcl_ConditionWait.
     * Hence, we do not know why the call returned. Was it the
     * caller signalled the variable or has the idle timer expired?
     */

    Tcl_MutexLock(&tpoolPtr->mutex);
    while (tpoolPtr->teardown == 0) {
        tpoolPtr->idleWorkers++;
        SignalWaiter(tpoolPtr);
        while (!tpoolPtr->teardown && !(rsPtr = PopWork(tpoolPtr))) {
            Tcl_GetTime(&t1);
            Tcl_ConditionWait(&tpoolPtr->cond, &tpoolPtr->mutex, idlePtr);
            Tcl_GetTime(&t2);
            if (tpoolPtr->idleTime) {
                if ((t2.sec - t1.sec) >= tpoolPtr->idleTime) {
                    tout = 1;
                }
            }
        }
        tpoolPtr->idleWorkers--;
        if (tpoolPtr->teardown || tout) {
            break;
        }
        Tcl_MutexUnlock(&tpoolPtr->mutex);
        TpoolEval(interp, rsPtr->script, rsPtr->scriptLen, rsPtr);
        Tcl_Free(rsPtr->script);
        Tcl_MutexLock(&tpoolPtr->mutex);

        PushDone(rsPtr, tpoolPtr);
    }

    /*
     * Tear down the worker
     */

    tpoolPtr->numWorkers--;
    SignalWaiter(tpoolPtr);
    Tcl_MutexUnlock(&tpoolPtr->mutex);

 out:

#ifdef NS_AOLSERVER
    Ns_TclMarkForDelete(tsdPtr->interp);
    Ns_TclDeAllocateInterp(tsdPtr->interp);
#else
    Tcl_DeleteInterp(tsdPtr->interp);
#endif
    Tcl_ExitThread(0);

    TCL_THREAD_CREATE_RETURN;
}

/*
 *----------------------------------------------------------------------
 *
 * RunStopEvent --
 *
 *      Signalizes the waiter thread to stop waiting.
 *
 * Results:
 *      1 (always)
 *
 * Side effects:
 *      None. 
 *
 *----------------------------------------------------------------------
 */
static int
RunStopEvent(eventPtr, mask)
    Tcl_Event *eventPtr; 
    int mask;
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    tsdPtr->stop = 1;
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * PushWork --
 *
 *      Adds a worker thread to the end of the workers list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
PushWork(rsPtr, tpoolPtr)
    TpoolResult *rsPtr;
    ThreadPool *tpoolPtr;
{
    SpliceIn(rsPtr, tpoolPtr->workHead);
    if (tpoolPtr->workTail == NULL) {
        tpoolPtr->workTail = rsPtr;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * PopWork --
 *
 *      Pops the work ticket from the list
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static TpoolResult *
PopWork(tpoolPtr)
    ThreadPool *tpoolPtr;
{   
    TpoolResult *rsPtr = tpoolPtr->workTail;

    if (rsPtr == NULL) {
        return NULL;
    }

    tpoolPtr->workTail = rsPtr->prevPtr;
    SpliceOut(rsPtr, tpoolPtr->workHead);

    rsPtr->nextPtr = rsPtr->prevPtr = NULL;

    return rsPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * PushDone --
 *
 *      Adds a worker thread to the end of the workers list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
PushDone(rsPtr, tpoolPtr)
    TpoolResult *rsPtr;
    ThreadPool *tpoolPtr;
{
    SpliceIn(rsPtr, tpoolPtr->doneHead);
    if (tpoolPtr->doneTail == NULL) {
        tpoolPtr->doneTail = rsPtr;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * PushWaiter --
 *
 *      Adds a waiter thread to the end of the waiters list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
PushWaiter(waitPtr, tpoolPtr)
    TpoolWaiter *waitPtr;
    ThreadPool *tpoolPtr;
{
    SpliceIn(waitPtr, tpoolPtr->waitHead);
    if (tpoolPtr->waitTail == NULL) {
        tpoolPtr->waitTail = waitPtr;
    }
} 

/*
 *----------------------------------------------------------------------
 *
 * PopWaiter --
 *
 *      Pops the first waiter from the head of the waiters list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static TpoolWaiter*
PopWaiter(tpoolPtr)
    ThreadPool *tpoolPtr;
{
    TpoolWaiter *waitPtr =  tpoolPtr->waitTail;

    if (waitPtr == NULL) {
        return NULL;
    }

    tpoolPtr->waitTail = waitPtr->prevPtr;
    SpliceOut(waitPtr, tpoolPtr->waitHead);

    waitPtr->prevPtr = waitPtr->nextPtr = NULL;

    return waitPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * GetTpool 
 *
 *      Parses the Tcl threadpool handle and locates the
 *      corresponding threadpool maintenance structure. 
 *
 * Results:
 *      Pointer to the threadpool struct or NULL if none found, 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static ThreadPool* 
GetTpool(tpoolName) 
    char *tpoolName;
{
    ThreadPool *tpoolPtr; 

    Tcl_MutexLock(&listMutex);
    tpoolPtr = GetTpoolUnl(tpoolName);
    Tcl_MutexUnlock(&listMutex);

    return tpoolPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * GetTpoolUnl 
 *
 *      Parses the threadpool handle and locates the
 *      corresponding threadpool maintenance structure. 
 *      Assumes caller holds the listMutex,
 *
 * Results:
 *      Pointer to the threadpool struct or NULL if none found, 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static ThreadPool* 
GetTpoolUnl (tpoolName) 
    char *tpoolName;
{
    unsigned int tpoolId, len = strlen(TPOOL_PREFIX);
    char *end;
    ThreadPool *tpoolPtr;

    if (strncmp(tpoolName, TPOOL_PREFIX, len) != 0) {
        return NULL;
    }
    tpoolName += len;
    tpoolId = strtoul(tpoolName, &end, 10);
    if ((end == tpoolName) || (*end != 0)) {
        return NULL; 
    }
    for (tpoolPtr = tpoolList; tpoolPtr; tpoolPtr = tpoolPtr->nextPtr) {
        if (tpoolPtr->tpoolId == tpoolId) {
            break;
        }
    }

    return tpoolPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolEval 
 *
 *      Evaluates the script and fills in the result structure. 
 *
 * Results:
 *      Standard Tcl result, 
 *
 * Side effects:
 *      Many, depending on the script.
 *
 *----------------------------------------------------------------------
 */
static int
TpoolEval(interp, script, scriptLen, rsPtr)
    Tcl_Interp *interp;
    char *script;
    int scriptLen;
    TpoolResult *rsPtr;
{
    int ret, reslen;
    char *result, *errorCode, *errorInfo;
    
    ret = Tcl_EvalEx(interp, script, scriptLen, TCL_EVAL_GLOBAL);
    if (rsPtr == NULL) {
        return ret;
    }
    rsPtr->retcode = ret;
    if (ret == TCL_ERROR) {
        errorCode = (char*)Tcl_GetVar(interp, "errorCode", TCL_GLOBAL_ONLY);
        errorInfo = (char*)Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
        if (errorCode != NULL) {
            rsPtr->errorCode = Tcl_Alloc(1 + strlen(errorCode));
            strcpy(rsPtr->errorCode, errorCode);
        }
        if (errorInfo != NULL) {
            rsPtr->errorInfo = Tcl_Alloc(1 + strlen(errorInfo));
            strcpy(rsPtr->errorInfo, errorInfo);
        }
    }
    
    result = (char*)Tcl_GetStringResult(interp);
    reslen = strlen(result);
    
    if (reslen == 0) {
        rsPtr->result = threadEmptyResult;
    } else {
        rsPtr->result = strcpy(Tcl_Alloc(1 + reslen), result);
    }

    return ret;
}

/*
 *----------------------------------------------------------------------
 *
 * SetResult
 *
 *      Sets the result in current interpreter.
 *
 * Results:
 *      Standard Tcl result, 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
SetResult(interp, rsPtr)
    Tcl_Interp *interp;
    TpoolResult *rsPtr;
{
    if (rsPtr->result) {
        if (rsPtr->result == threadEmptyResult) {
            Tcl_ResetResult(interp);
        } else {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(rsPtr->result,-1));
            Tcl_Free(rsPtr->result);
            rsPtr->result = NULL;
        }
    }
    if (rsPtr->retcode == TCL_ERROR) {
        if (rsPtr->errorCode) {
            Tcl_SetObjErrorCode(interp,Tcl_NewStringObj(rsPtr->errorCode,-1));
            Tcl_Free(rsPtr->errorCode);
            rsPtr->errorCode = NULL;
        }
        if (rsPtr->errorInfo) {
            Tcl_AddObjErrorInfo(interp, rsPtr->errorInfo, -1);
            Tcl_Free(rsPtr->errorInfo);
            rsPtr->errorInfo = NULL;
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolReserve --
 *
 *      Does the pool preserve and/or release. Assumes caller holds 
 *      the listMutex.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May tear-down the threadpool if refcount drops to 0 or below.
 *
 *----------------------------------------------------------------------
 */
static int
TpoolReserve(interp, tpoolPtr, what)
    Tcl_Interp *interp;
    ThreadPool *tpoolPtr;
    int what;
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    int refCount;
    TpoolResult *rsPtr;

    switch(what) {
    case TPOOL_RESERVE:
        refCount = ++tpoolPtr->refCount;
        break;

    case TPOOL_RELEASE:
        refCount = --tpoolPtr->refCount;
        if (refCount > 0) { 
            break;
        }

        /*
         * Pool is going away; remove from the list of pools,
         */ 
        
        SpliceOut(tpoolPtr, tpoolList);

        if (tsdPtr->waitPtr == NULL) {
            tsdPtr->waitPtr = (TpoolWaiter*)Tcl_Alloc(sizeof(TpoolWaiter));
            memset(tsdPtr->waitPtr, 0, sizeof(TpoolWaiter));
            tsdPtr->waitPtr->threadId = Tcl_GetCurrentThread();
        }

        /*
         * Signal and wait for all workers to die.
         */
        
        tpoolPtr->teardown = 1;
        Tcl_MutexLock(&tpoolPtr->mutex);
        while (tpoolPtr->numWorkers > 0) {
            PushWaiter(tsdPtr->waitPtr, tpoolPtr);
            Tcl_ConditionNotify(&tpoolPtr->cond);
            Tcl_MutexUnlock(&tpoolPtr->mutex);
            tsdPtr->stop = -1;
            while(tsdPtr->stop == -1) {
                Tcl_DoOneEvent(TCL_ALL_EVENTS);
            }
            Tcl_MutexLock(&tpoolPtr->mutex);
        }
        
        /*
         * Tear down the pool structure
         */
        
        if (tpoolPtr->initScript) {
            Tcl_Free(tpoolPtr->initScript);
        }
        for (rsPtr = tpoolPtr->doneHead; rsPtr; rsPtr = rsPtr->prevPtr) {
            if (rsPtr->result) {
                Tcl_Free(rsPtr->result);
            }
            if (rsPtr->retcode == TCL_ERROR) {
                if (rsPtr->errorInfo) {
                    Tcl_Free(rsPtr->errorInfo);
                }
                if (rsPtr->errorCode) {
                    Tcl_Free(rsPtr->errorCode);
                }
            }
        }
        for (rsPtr = tpoolPtr->workHead; rsPtr; rsPtr = rsPtr->prevPtr) {
            Tcl_Free(rsPtr->script);
        }
        Tcl_MutexFinalize(&tpoolPtr->mutex);
        Tcl_ConditionFinalize(&tpoolPtr->cond);
        Tcl_Free((char*)tpoolPtr);
        break;
    }

    Tcl_SetObjResult(interp, Tcl_NewIntObj(refCount));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * SignalWaiter --
 *
 *      Signals the waiter thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The waiter thread will exit from the event loop.
 *
 *----------------------------------------------------------------------
 */
static void
SignalWaiter(tpoolPtr)
    ThreadPool *tpoolPtr;
{
    TpoolWaiter *waitPtr;
    Tcl_Event *evPtr;

    waitPtr = PopWaiter(tpoolPtr);
    if (waitPtr == NULL) {
        return;
    }

    evPtr = (Tcl_Event*)Tcl_Alloc(sizeof(Tcl_Event));
    evPtr->proc = RunStopEvent;

    Tcl_ThreadQueueEvent(waitPtr->threadId,(Tcl_Event*)evPtr,TCL_QUEUE_TAIL);
    Tcl_ThreadAlert(waitPtr->threadId);
}

/*
 *----------------------------------------------------------------------
 *
 * ExitHandler --
 *
 *      Performs cleanup when a caller (poster) thread exits.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
ExitHandler(clientData)
    ClientData clientData;
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    Tcl_Free((char*)tsdPtr->waitPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Tpool_Init --
 *
 *      Create commands in current interpreter.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Tpool_Init (interp)
    Tcl_Interp *interp;                 /* Interp where to create cmds */
{
    TCL_CMD(interp, "tpool::create",   TpoolCreateObjCmd);
    TCL_CMD(interp, "tpool::post",     TpoolPostObjCmd);
    TCL_CMD(interp, "tpool::wait",     TpoolWaitObjCmd);
    TCL_CMD(interp, "tpool::get",      TpoolGetObjCmd);
    TCL_CMD(interp, "tpool::preserve", TpoolReserveObjCmd);
    TCL_CMD(interp, "tpool::release",  TpoolReleaseObjCmd);
}

/* EOF $RCSfile: threadPoolCmd.c,v $ */

/* Emacs Setup Variables */
/* Local Variables:      */
/* mode: C               */
/* indent-tabs-mode: nil */
/* c-basic-offset: 4     */
/* End:                  */
