/* 
 * threadPoolCmd.c --
 *
 *  This file implements the Tcl thread pools.
 *
 * Copyright (c) 2002 by Zoran Vasiljevic, Archiware GmbH
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: threadPoolCmd.c,v 1.1 2002/12/02 18:18:34 vasiljevic Exp $
 * ----------------------------------------------------------------------------
 */

#include "tclThread.h"

/*
 * Structure to maintain idle threads
 */

typedef struct IdleThread {
    int eventId;                    /* Event counter */
    Tcl_ThreadId threadId;          /* Thread id of the current thread */
    struct IdleThread *nextPtr;     /* Next structure in the list */
    struct IdleThread *prevPtr;     /* Previous structure in the list */
} IdleThread;

/*
 * Structure describing an instance of a thread pool.
 */

typedef struct ThreadPool {
    unsigned int tpoolId;           /* Pool id number */
    unsigned int jobId;             /* Job counter */
    int idleTime;                   /* Time in ms an worker thread idles */
    char *initScript;               /* Script to initialize worker thread */
    int minWorkers;                 /* Minimum number or worker threads */
    int maxWorkers;                 /* Maximum number of worker threads */
    int numWorkers;                 /* Current number of worker threads */
    int refCount;                   /* Reference counter for reserve/release */
    Tcl_Mutex tpoolMutex;           /* Pool mutex */
    Tcl_Condition tpoolCond;        /* Pool condition variable */
    struct IdleThread *workersTail; /* Tail of idle worker thread list */
    struct IdleThread *workersHead; /* Start of idle worker thread list*/
    struct IdleThread *waitersTail; /* Tail of waiter thread list */
    struct IdleThread *waitersHead; /* Start of waiter thread list */
    struct ThreadPool *nextPtr;     /* Next structure in the threadpool list */
    struct ThreadPool *prevPtr;     /* Previous structure in threadpool list */
} ThreadPool;

#define TPOOL_PREFIX       "tpool"  /* Prefix to generate Tcl pool handles */
#define TPOOL_MINWORKERS   0        /* Default minimum # of worker threads */
#define TPOOL_MAXWORKERS   4        /* Default maximum # of worker threads */
#define TPOOL_IDLETIMER    0        /* Default worker thread idle timer */
#define TPOOL_RESERVE      0        /* The pool reserve flag */
#define TPOOL_RELEASE      1        /* The pool release flag */ 

/*
 * Structure for passing evaluation results
 */

typedef struct SendResult {
    unsigned int jobId;             /* The job id of the current job */
    int retcode;                    /* Tcl return code of the current job */
    char *result;                   /* Tcl result of the current job */
    char *errorCode;                /* On error: content of the errorCode var */
    char *errorInfo;                /* On error: content of the errorInfo var */
    Tcl_ThreadId threadId;          /* Originating thread id */
    ThreadPool *tpoolPtr;           /* Current thread pool */
} SendResult;

/*
 * This is the event used to send data to worker threads.
 */

typedef struct SendEvent {
    Tcl_Event event;                /* Must be the first element */
    int eventId;                    /* Event serial number */
    int detached;                   /* The job is detached (ignore result) */
    char *script;                   /* Script to evaluate in worker thread */
    SendResult *resultPtr;          /* Result structure */
} SendEvent;

/*
 * Private structure for each worker/poster thread.
 */

typedef struct ThreadSpecificData {
    int stop;                       /* Marks stop event; exit from event loop */
    IdleThread *idlePtr;            /* Threads private idle structure */
    ThreadPool *tpoolPtr;           /* Worker thread pool association */
    Tcl_TimerToken timer;           /* Worker thread idle timer token */
    Tcl_Interp *interp;             /* Worker thread interpreter */
    Tcl_HashTable jobResults;       /* Poster thread job result table */
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

static void
IdleProc       _ANSI_ARGS_((ClientData clientData));

static int
RunStopEvent   _ANSI_ARGS_((Tcl_Event *evPtr, int mask));

static int  
RunSendEvent   _ANSI_ARGS_((Tcl_Event *evPtr, int mask));

static int  
RunResultEvent _ANSI_ARGS_((Tcl_Event *evPtr, int mask));

static void
PushWorker     _ANSI_ARGS_((IdleThread *idlePtr, ThreadPool *tpoolPtr));

static void
PushWaiter     _ANSI_ARGS_((IdleThread *idlePtr, ThreadPool *tpoolPtr));

static void
SignalWaiter   _ANSI_ARGS_((ThreadPool *tpoolPtr));

static int
TpoolEval      _ANSI_ARGS_((Tcl_Interp *interp,char*script,SendResult*rsPtr));

static void
TpoolResult    _ANSI_ARGS_((Tcl_Interp *interp, SendResult *rsPtr));

static IdleThread*
PopWorker      _ANSI_ARGS_((IdleThread *idlePtr, ThreadPool *tpoolPtr));

static IdleThread*
PopWaiter      _ANSI_ARGS_((IdleThread *idlePtr, ThreadPool *tpoolPtr));

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
    int ii, minw, maxw, idle, len;
    char buf[16], *cmd = NULL;
    ThreadPool *tpoolPtr;

    /* 
     * Syntax:  tpool::create ?-minworkers count?
     *                        ?-maxworkers count?
     *                        ?-initcmd command?
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
            idle *= 1000; /* We need msecs */
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
    tpoolPtr->tpoolId = tpoolCounter++;

    for (ii = 0; ii < tpoolPtr->minWorkers; ii++) {
        if (CreateWorker(interp, tpoolPtr) != TCL_OK) {
            Tcl_MutexUnlock(&listMutex);
            return TCL_ERROR;
        }
    }
    Tcl_MutexUnlock(&listMutex);

    /*
     * Plug the pool in the list of available pools
     */

    SpliceIn(tpoolPtr, tpoolList);
    sprintf(buf, "%s%u", TPOOL_PREFIX, tpoolPtr->tpoolId);
    Tcl_MutexUnlock(&listMutex);

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
    int new, detached, len;
    char *tpoolName, *script;
    SendEvent *evPtr;
    ThreadPool *tpoolPtr;
    IdleThread *idlePtr;
    Tcl_HashEntry *hPtr;

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
    
    if (tsdPtr->idlePtr == NULL) {
        tsdPtr->idlePtr  = (IdleThread*)Tcl_Alloc(sizeof(IdleThread));
        memset(tsdPtr->idlePtr, 0, sizeof(IdleThread));
        tsdPtr->idlePtr->threadId = Tcl_GetCurrentThread();
        Tcl_InitHashTable(&tsdPtr->jobResults, TCL_ONE_WORD_KEYS);
        Tcl_CreateThreadExitHandler(ExitHandler, NULL);
    }

    /*
     * Get one idle thread. If none found, go to sleep
     * servicing the event loop. First available worker
     * will kick us out of the event loop.
     */ 
   
    Tcl_MutexLock(&tpoolPtr->tpoolMutex);
    idlePtr = NULL;
    jobId   = tpoolPtr->jobId++;
    while(idlePtr == NULL) {
        idlePtr = PopWorker(NULL, tpoolPtr);
        if (idlePtr == NULL) {
            PushWaiter(tsdPtr->idlePtr, tpoolPtr);
            if (tpoolPtr->numWorkers < tpoolPtr->maxWorkers) {
                /*
                 * No more free workers; start new one
                 */
                if (CreateWorker(interp, tpoolPtr) != TCL_OK) {
                    Tcl_MutexUnlock(&tpoolPtr->tpoolMutex);
                    return TCL_ERROR;
                }
            }
            tsdPtr->idlePtr->eventId++;
            Tcl_MutexUnlock(&tpoolPtr->tpoolMutex);
            tsdPtr->stop = -1;
            while(tsdPtr->stop == -1) {
                Tcl_DoOneEvent(TCL_ALL_EVENTS);
            }
            Tcl_MutexLock(&tpoolPtr->tpoolMutex);
        }
    }
    Tcl_MutexUnlock(&tpoolPtr->tpoolMutex);

    /*
     * Now, that we have a worker, send him the job.
     */

    evPtr = (SendEvent*)Tcl_Alloc(sizeof(SendEvent));
    evPtr->event.proc = RunSendEvent;
    evPtr->detached   = detached;
    evPtr->script     = strcpy(Tcl_Alloc(len+1), script);
    if (detached == 0) {
        evPtr->resultPtr = (SendResult*)Tcl_Alloc(sizeof(SendResult));
        memset(evPtr->resultPtr, 0, sizeof(SendResult));
        evPtr->resultPtr->jobId    = jobId;
        evPtr->resultPtr->threadId = Tcl_GetCurrentThread();
        hPtr = Tcl_CreateHashEntry(&tsdPtr->jobResults, (char*)jobId, &new);
        Tcl_SetHashValue(hPtr, NULL);
        Tcl_SetObjResult(interp, Tcl_NewIntObj(jobId));
    } else {
        evPtr->resultPtr = NULL;
    }

    Tcl_ThreadQueueEvent(idlePtr->threadId,(Tcl_Event*)evPtr,TCL_QUEUE_TAIL);
    Tcl_ThreadAlert(idlePtr->threadId);

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
    int ii, done, wait, wObjc, wId;
    char *tpoolName, *listVar = NULL;
    Tcl_Obj *waitList, **wObjv;
    ThreadPool *tpoolPtr;
    Tcl_HashEntry *hPtr;

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

    if (tsdPtr->idlePtr == NULL) {
        if (listVar) {
            Tcl_SetVar2Ex(interp, listVar, NULL, Tcl_NewObj(), 0);
        }
        return TCL_OK; /* Never sent any job */
    }

    /*
     * Scan the whole job table and fill two jobid lists.
     * The result list will contain list of jobs done (can be collected)
     * while the other (optional) list accumulates jobs still pending.
     * If no job found on the first scan, enter the event loop and 
     * wait for first job to be serviced and put it in the result list.
     */

    done = -1; /* Index of done element */
    wait =  0; /* Number of elements in waiting list */

    while (done == -1) {
        if (listVar) {
            waitList = Tcl_NewListObj(0, NULL);
        }
        if (wObjc == 0) {
            break;
        }
        for (ii = 0; ii < wObjc; ii++) {
            if (Tcl_GetIntFromObj(interp, wObjv[ii], &wId) != TCL_OK) {
                return TCL_ERROR;
            } 
            hPtr = Tcl_FindHashEntry(&tsdPtr->jobResults, (char*)wId);
            if (hPtr == NULL) {
                continue; /* Silently ignore invalid job id's */
            }
            if (Tcl_GetHashValue(hPtr) == NULL) {
                wait++;
                if (listVar) {
                    Tcl_ListObjAppendElement(interp, waitList, wObjv[ii]);
                }
            } else if (done == -1) {
                done = ii; /* Got first hit; continue filling the waitlist */
            }
        }
        if (done >= 0) {
            Tcl_SetObjResult(interp, wObjv[done]);
        } else if (wObjc) {
            if (listVar && wait) {
                Tcl_DecrRefCount(waitList);
            }
            Tcl_DoOneEvent(TCL_ALL_EVENTS);
        }
    }

    if (listVar) {
        Tcl_SetVar2Ex(interp, listVar, NULL, waitList, 0);
    }

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
    char *tpoolName, *resVar = NULL;
    ThreadPool *tpoolPtr;
    Tcl_HashEntry *hPtr;
    SendResult *rsPtr;

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

    if (tsdPtr->idlePtr == NULL) {
        Tcl_AppendResult(interp, "job not found", NULL);
        return TCL_ERROR;
    }

    while(1) {
        hPtr = Tcl_FindHashEntry(&tsdPtr->jobResults, (char*)jobId);
        if (hPtr == NULL) {
            Tcl_AppendResult(interp, "job never posted", NULL);
            return TCL_ERROR;
        }
        rsPtr = (SendResult*)Tcl_GetHashValue(hPtr);
        if (rsPtr) {
            break;
        }
        Tcl_DoOneEvent(TCL_ALL_EVENTS);
    }
 
    ret = rsPtr->retcode;

    TpoolResult(interp, rsPtr);
    Tcl_Free((char*)rsPtr);
    Tcl_DeleteHashEntry(hPtr);

    if (resVar) {
        Tcl_SetVar2Ex(interp, resVar, NULL, Tcl_GetObjResult(interp), 0);
        Tcl_SetObjResult(interp, Tcl_NewIntObj(rsPtr->retcode));
        return TCL_OK; 
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
    SendResult result;
    Tcl_Mutex waitMutex = (Tcl_Mutex)0;

    /*
     * Creates the new worker thread
     */

    memset(&result, 0, sizeof(SendResult));
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
        Tcl_ConditionWait(&tpoolPtr->tpoolCond, &waitMutex, NULL);
    }
    Tcl_MutexUnlock(&waitMutex);
    Tcl_MutexFinalize(&waitMutex);

    /*
     * Set error-related information if the thread
     * failed to initialize correctly.
     */
    
    if (result.retcode == TCL_ERROR) {
        TpoolResult(interp, &result);
        return TCL_ERROR;
    }

    /*
     * Inform first waiter thread about new worker
     */

    SignalWaiter(tpoolPtr);

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
    SendResult          *rsPtr = (SendResult*)clientData;
    ThreadPool       *tpoolPtr = rsPtr->tpoolPtr;

    int maj, min, ptch, type;
    Tcl_Interp *interp;
    IdleThread *idlePtr = NULL;

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
        Tcl_ConditionNotify(&tpoolPtr->tpoolCond);
        goto out;
    }
    rsPtr->retcode = Thread_Init(interp);
#endif
    
    /*
     * Initialize the interpreter
     */

    if (rsPtr->retcode != TCL_ERROR && tpoolPtr->initScript) {
        TpoolEval(interp, tpoolPtr->initScript, rsPtr);
        if (rsPtr->retcode == TCL_ERROR) {
            Tcl_ConditionNotify(&tpoolPtr->tpoolCond);
            goto out;
        }
    }

    /*
     * Register us in the list of active workers
     */

    idlePtr = (IdleThread*)Tcl_Alloc(sizeof(IdleThread));
    memset(idlePtr, 0, sizeof(IdleThread));
    idlePtr->threadId = Tcl_GetCurrentThread();

    /*
     * No need to lock; caller holds the pool lock)
     */

    tpoolPtr->numWorkers++;
    PushWorker(idlePtr, tpoolPtr);

    tsdPtr->tpoolPtr = tpoolPtr;
    tsdPtr->idlePtr  = idlePtr;
    tsdPtr->interp   = interp;

    /*
     * Register idle timer event.
     */
    
    if (tpoolPtr->idleTime) {
        tsdPtr->timer=Tcl_CreateTimerHandler(tpoolPtr->idleTime,IdleProc,NULL);
    }

    /*
     * Tell caller we're ready. Caller will release 
     * the pool mutex from now on.
     */
    
    rsPtr->retcode = 0;
    Tcl_ConditionNotify(&tpoolPtr->tpoolCond);

    /*
     * Service event loop until signaled to stop.
     */

    tsdPtr->stop  = 0;
    while(tsdPtr->stop == 0) {
        Tcl_DoOneEvent(TCL_ALL_EVENTS);
    }

    Tcl_MutexLock(&tpoolPtr->tpoolMutex);
    tpoolPtr->numWorkers--;
    SignalWaiter(tpoolPtr);
    Tcl_MutexUnlock(&tpoolPtr->tpoolMutex);

    if (tsdPtr->timer) {
        Tcl_DeleteTimerHandler(tsdPtr->timer);
    }
    Tcl_Free((char*)idlePtr);

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
 * IdleProc --
 *
 *      Signalizes the thread to exit since its idle timer has expired.
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
IdleProc(cd) 
     ClientData cd;
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    ThreadPool       *tpoolPtr = tsdPtr->tpoolPtr;

    IdleThread *myPtr;

    Tcl_MutexLock(&tpoolPtr->tpoolMutex);
    
    /*
     * Remove this thread from the list of idle workers.
     * This way, nobody can send any work to it.
     */

    myPtr = PopWorker(tsdPtr->idlePtr, tpoolPtr);
    if (myPtr == NULL) {
       /*
        * We were not on the worker list, meaning
        * some poster is about to send us a job.
        */
        goto done;        
    }

    /*
     * Prune number of active workers if needed
     */

    if (tpoolPtr->numWorkers > tpoolPtr->minWorkers) {
        /*
         * We have more workers than need be
         * so this worker will stop now.
         */
        tsdPtr->stop = 1;
    } else {
        /*
         * Worker may live; put it back on the list
         */
        PushWorker(tsdPtr->idlePtr, tpoolPtr);
    }

done:

    Tcl_MutexUnlock(&tpoolPtr->tpoolMutex);
    if (tpoolPtr->idleTime) {
        tsdPtr->timer = Tcl_CreateTimerHandler(tpoolPtr->idleTime,IdleProc,cd);
    }
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
    SendEvent           *evPtr = (SendEvent*)eventPtr;

    tsdPtr->stop = evPtr->eventId;
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * RunSendEvent --
 *
 *      Runs the job in the worker thread.
 *
 * Results:
 *      1 always.
 *
 * Side effects:
 *      Many, depending on the passed command.
 *
 *----------------------------------------------------------------------
 */
static int
RunSendEvent(eventPtr, mask)
    Tcl_Event *eventPtr;
    int mask;
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    Tcl_Interp         *interp = tsdPtr->interp; 
    ThreadPool       *tpoolPtr = tsdPtr->tpoolPtr;
    SendEvent           *evPtr = (SendEvent*)eventPtr;
    SendResult          *rsPtr = evPtr->resultPtr;

    /*
     * Cancel the idle timer and run the job
     */

    if (tsdPtr->timer) {
        Tcl_DeleteTimerHandler(tsdPtr->timer);
    }

    TpoolEval(interp, evPtr->script, rsPtr);
    Tcl_Free(evPtr->script);

    /*
     * Pass the result back to the caller
     */

    if (evPtr->detached == 0) {
        evPtr = (SendEvent*)Tcl_Alloc(sizeof(SendEvent));
        evPtr->event.proc = RunResultEvent;
        evPtr->resultPtr  = rsPtr;
        Tcl_ThreadQueueEvent(rsPtr->threadId,(Tcl_Event*)evPtr,TCL_QUEUE_TAIL);
        Tcl_ThreadAlert(rsPtr->threadId);
    }

    /*
     * Put us at the end of the list of idle workers
     * because the job poster has removed our record.
     * Also, signalize first waiter thread, if any.
     */

    Tcl_MutexLock(&tpoolPtr->tpoolMutex);
    PushWorker(tsdPtr->idlePtr, tpoolPtr);
    SignalWaiter(tpoolPtr);
    Tcl_MutexUnlock(&tpoolPtr->tpoolMutex);

    /*
     * Start the idle timer again
     */

    if (tsdPtr->tpoolPtr->idleTime) {
        tsdPtr->timer = 
            Tcl_CreateTimerHandler(tsdPtr->tpoolPtr->idleTime, IdleProc, NULL);
    }

    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * RunResultEvent --
 *
 *      Runs in the poster thread. Stores the jobs result in the 
 *      thread's private result table.
 *
 * Results:
 *      1 always.
 *
 * Side effects:
 *      None..
 *
 *----------------------------------------------------------------------
 */
static int
RunResultEvent(tclEvent, mask)
    Tcl_Event *tclEvent;
    int mask;
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);
    SendEvent           *evPtr = (SendEvent*)tclEvent;
    SendResult          *rsPtr = evPtr->resultPtr;

    int new;
    Tcl_HashEntry *hPtr;

    hPtr = Tcl_CreateHashEntry(&tsdPtr->jobResults, (char*)rsPtr->jobId, &new);
    Tcl_SetHashValue(hPtr, rsPtr);

    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * PushWorker --
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
PushWorker(idlePtr, tpoolPtr)
    IdleThread *idlePtr;
    ThreadPool *tpoolPtr;
{
    SpliceIn(idlePtr, tpoolPtr->workersTail);
    if (tpoolPtr->workersHead == NULL) {
        tpoolPtr->workersHead = idlePtr;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * PopWorker --
 *
 *      Pops the first worker from the head of the workers list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static IdleThread *
PopWorker(idlePtr, tpoolPtr)
    IdleThread *idlePtr;
    ThreadPool *tpoolPtr;
{   
    if (idlePtr == NULL && (idlePtr = tpoolPtr->workersHead) == NULL) {
        return NULL;
    }

    SpliceOut(idlePtr, tpoolPtr->workersTail);

    if (tpoolPtr->workersHead == idlePtr) {
        tpoolPtr->workersHead =  idlePtr->prevPtr;
    }

    idlePtr->nextPtr = idlePtr->prevPtr = NULL;

    return idlePtr;
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
PushWaiter(idlePtr, tpoolPtr)
    IdleThread *idlePtr;
    ThreadPool *tpoolPtr;
{
    SpliceIn(idlePtr, tpoolPtr->waitersTail);
    if (tpoolPtr->waitersHead == NULL) {
        tpoolPtr->waitersHead = idlePtr;
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

static IdleThread *
PopWaiter(idlePtr, tpoolPtr)
    IdleThread *idlePtr;
    ThreadPool *tpoolPtr;
{
    if (idlePtr == NULL && (idlePtr = tpoolPtr->waitersHead) == NULL) {
        return NULL;
    }

    SpliceOut(idlePtr, tpoolPtr->waitersTail);

    if (tpoolPtr->waitersHead == idlePtr) {
        tpoolPtr->waitersHead =  idlePtr->prevPtr;
    }

    idlePtr->prevPtr = idlePtr->nextPtr = NULL;

    return idlePtr;
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
TpoolEval(interp, script, rsPtr)
    Tcl_Interp *interp;
    char *script;
    SendResult *rsPtr;
{
    int ret, reslen;
    char *result, *errorCode, *errorInfo;
    
    ret = Tcl_EvalEx(interp, script, -1, TCL_EVAL_GLOBAL);
    if (rsPtr == NULL) {
        return ret;
    }
    rsPtr->retcode = ret;
    if (rsPtr->retcode == TCL_ERROR) {
        errorCode = Tcl_GetVar(interp, "errorCode", TCL_GLOBAL_ONLY);
        errorInfo = Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
        if (errorCode != NULL) {
            rsPtr->errorCode = Tcl_Alloc(1 + strlen(errorCode));
            strcpy(rsPtr->errorCode, errorCode);
        }
        if (errorInfo != NULL) {
            rsPtr->errorInfo = Tcl_Alloc(1 + strlen(errorInfo));
            strcpy(rsPtr->errorInfo, errorInfo);
        }
    }
    
    result = Tcl_GetStringResult(interp);
    reslen = strlen(result);
    
    if (reslen == 0) {
        rsPtr->result = threadEmptyResult;
    } else {
        rsPtr->result = strcpy(Tcl_Alloc(1 + reslen), result);
    }

    return rsPtr->retcode;
}

/*
 *----------------------------------------------------------------------
 *
 * TpoolResult
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
TpoolResult(interp, rsPtr)
    Tcl_Interp *interp;
    SendResult *rsPtr;
{
    if (rsPtr->result) {
        if (rsPtr->result == threadEmptyResult) {
            Tcl_ResetResult(interp);
        } else {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(rsPtr->result,-1));
            Tcl_Free(rsPtr->result);
        }
    }
    if (rsPtr->retcode == TCL_ERROR) {
        if (rsPtr->errorCode) {
            Tcl_SetObjErrorCode(interp,Tcl_NewStringObj(rsPtr->errorCode,-1));
            Tcl_Free(rsPtr->errorCode);
        }
        if (rsPtr->errorInfo) {
            Tcl_AddObjErrorInfo(interp, rsPtr->errorInfo, -1);
            Tcl_Free(rsPtr->errorInfo);
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
    int refCount, eventId; 
    SendEvent *evPtr;
    IdleThread *idlePtr;

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

        if (tsdPtr->idlePtr == NULL) {
            tsdPtr->idlePtr = (IdleThread*)Tcl_Alloc(sizeof(IdleThread));
            memset(tsdPtr->idlePtr, 0, sizeof(IdleThread));
            tsdPtr->idlePtr->threadId = Tcl_GetCurrentThread();
        }

        /*
         * Signal and wait for all workers to die.
         */

        while(1) {
            Tcl_MutexLock(&tpoolPtr->tpoolMutex);
            idlePtr = PopWorker(NULL, tpoolPtr);
            if (idlePtr == NULL) {
                Tcl_MutexUnlock(&tpoolPtr->tpoolMutex);
                break; /* No more workers left */
            }
            eventId = tsdPtr->idlePtr->eventId;
            PushWaiter(tsdPtr->idlePtr, tpoolPtr);
            Tcl_MutexUnlock(&tpoolPtr->tpoolMutex);
            evPtr = (SendEvent*)Tcl_Alloc(sizeof(SendEvent));
            evPtr->event.proc = RunStopEvent;
            evPtr->eventId    = eventId;
            Tcl_ThreadQueueEvent(idlePtr->threadId, (Tcl_Event*)evPtr, 
                                 TCL_QUEUE_TAIL);
            Tcl_ThreadAlert(idlePtr->threadId);
            tsdPtr->stop = -1;
            while(tsdPtr->stop != eventId) {
                Tcl_DoOneEvent(TCL_ALL_EVENTS);
            }
            tsdPtr->idlePtr->eventId++;
        }
        
        /*
         * Tear down the pool structure
         */
        
        if (tpoolPtr->initScript) {
            Tcl_Free(tpoolPtr->initScript);
        }
        Tcl_MutexFinalize(&tpoolPtr->tpoolMutex);
        Tcl_ConditionFinalize(&tpoolPtr->tpoolCond);
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
    IdleThread *idlePtr;
    SendEvent *evPtr;

    idlePtr = PopWaiter(NULL, tpoolPtr);
    if (idlePtr == NULL) {
        return;
    }

    evPtr = (SendEvent*)Tcl_Alloc(sizeof(SendEvent));
    evPtr->event.proc = RunStopEvent;
    evPtr->eventId    = idlePtr->eventId;

    Tcl_ThreadQueueEvent(idlePtr->threadId,(Tcl_Event*)evPtr,TCL_QUEUE_TAIL);
    Tcl_ThreadAlert(idlePtr->threadId);
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

    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;

   /*
    * Clean non-collected jobs from the job table
    */

    hPtr = Tcl_FirstHashEntry(&tsdPtr->jobResults, &search);
    while(hPtr != NULL) {
        SendResult *rsPtr = (SendResult*)Tcl_GetHashValue(hPtr);
        if (rsPtr) {
            if (rsPtr->result) {
                Tcl_Free((char*)rsPtr->result);
            }
            if (rsPtr->retcode == TCL_ERROR) {
                if (rsPtr->errorCode) {
                    Tcl_Free((char*)rsPtr->errorCode);
                }
                if (rsPtr->errorInfo) {
                    Tcl_Free((char*)rsPtr->errorInfo);
                }
            }
            Tcl_Free((char*)rsPtr);
        }
        hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&tsdPtr->jobResults);
    Tcl_Free((char*)tsdPtr->idlePtr);
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
