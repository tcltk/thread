/* 
 * threadSpCmd.c --
 *
 * This file implements commands for script-level access to thread 
 * synchronization primitives. Currently, the mutex and condition
 * variable objects are exposed to the script programmer.
 *
 * Additionaly, a locked eval is also implemented. This is a practical
 * convenience function which relieves the programmer from the need
 * to take care about unlocking some mutex after evaluating a protected
 * part of code.
 *
 * Copyright (c) 2002 by Zoran Vasiljevic.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: threadSpCmd.c,v 1.15 2003/05/31 10:46:00 vasiljevic Exp $
 * ----------------------------------------------------------------------------
 */

#include "tclThread.h"

/*
 * This global (static) data is used to map opaque Tcl-level handles 
 * to pointers of their corresponding synchronization objects.
 */

static Tcl_HashTable syncHandles;       /* Handles/pointers to sync objects */
static Tcl_Mutex syncHandlesMutex;      /* Protects the above table */
static unsigned int syncHandlesCounter; /* Counter for unique object id's */

#define GRAB_SYNCMUTEX    Tcl_MutexLock(&syncHandlesMutex)
#define RELEASE_SYNCMUTEX Tcl_MutexUnlock(&syncHandlesMutex)

#define MUTEXID 'm' /* First letter of the mutex handle */
#define CONDID  'c' /* First letter of the condition-variable handle */

/*
 * Functions implementing Tcl commands
 */

static Tcl_ObjCmdProc ThreadMutexObjCmd;
static Tcl_ObjCmdProc ThreadCondObjCmd;
static Tcl_ObjCmdProc ThreadEvalObjCmd;

/*
 * Forward declaration of functions used only within this file
 */

static int  GetObjFromHandle _ANSI_ARGS_((Tcl_Interp *interp, 
                                          int type, 
                                          char *id, 
                                          void **addrPtrPtr));

static void SetHandleFromObj _ANSI_ARGS_((Tcl_Interp *interp, 
                                          int type, 
                                          void *addrPtr));

static void DeleteObjHandle  _ANSI_ARGS_((char *id));
static void FinalizeSp       _ANSI_ARGS_((ClientData clientData));


/*
 *----------------------------------------------------------------------
 *
 * ThreadMutexObjCmd --
 *
 *    This procedure is invoked to process "thread::mutex" Tcl command.
 *    See the user documentation for details on what it does.
 *
 * Results:
 *    A standard Tcl result.
 *
 * Side effects:
 *    See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadMutexObjCmd(dummy, interp, objc, objv)
    ClientData dummy;                   /* Not used. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    int opt, ret;
    char *mutexHandle;
    Tcl_Mutex *mutexPtr = NULL;

    static CONST84 char *cmdOpts[] = {
        "create", "destroy", "lock", "unlock", NULL
    };
    enum options {
        m_CREATE, m_DESTROY, m_LOCK, m_UNLOCK
    };
    
    /* 
     * Syntax:
     *
     *     thread::mutex create
     *     thread::mutex destroy <mutexHandle>
     *     thread::mutex lock <mutexHandle>
     *     thread::mutex unlock <mutexHandle>
     */

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?args?");
        return TCL_ERROR;
    }
    ret = Tcl_GetIndexFromObj(interp, objv[1], cmdOpts, "option", 0, &opt);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }

    /*
     * Cover the "create" option first, since it needs no existing handle
     */

    if (opt == (int)m_CREATE) {
        mutexPtr = (Tcl_Mutex*)Tcl_Alloc(sizeof(Tcl_Mutex));
        *mutexPtr = 0;
        SetHandleFromObj(interp, MUTEXID, (void*)mutexPtr);
        return TCL_OK;
    }

    /*
     * All other options require a valid handle. See if we got any...
     */

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 2, objv, "mutexHandle");
        return TCL_ERROR;
    }
    mutexHandle = Tcl_GetString(objv[2]);
    ret = GetObjFromHandle(interp, MUTEXID, mutexHandle, (void**) &mutexPtr);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }

    /*
     * Having found the correct handle, take care about other options.
     */

    switch ((enum options)opt) {
    case m_LOCK:   
        Tcl_MutexLock(mutexPtr);
        break;
    case m_UNLOCK: 
        Tcl_MutexUnlock(mutexPtr);
        break;
    case m_DESTROY:
        Tcl_MutexFinalize(mutexPtr);
        Tcl_Free((char*)mutexPtr);
        DeleteObjHandle(mutexHandle);
        break;
    case m_CREATE: /* Already handled above */
        break;
    }

    return TCL_OK;
}
/*
 *----------------------------------------------------------------------
 *
 * ThreadEvalObjCmd --
 *
 *    This procedure is invoked to process "thread::eval" Tcl command.
 *    See the user documentation for details on what it does.
 *
 * Results:
 *    A standard Tcl result.
 *
 * Side effects:
 *    See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadEvalObjCmd(dummy, interp, objc, objv)
    ClientData dummy;                   /* Not used. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    int ret, optx;
    char *mutexHandle;
    Tcl_Obj *scriptObj;
    Tcl_Mutex *mutexPtr = NULL;
    static Tcl_Mutex evalMutex;

    /* 
     * Syntax:
     *
     *     thread::eval ?-lock <mutexHandle>? arg ?arg ...?
     */

    if (objc < 2) {
        Tcl_AppendResult(interp, "wrong # args: should be \"",
                         Tcl_GetString(objv[0]),
                         " ?-lock <mutexHandle>? arg ?arg...?\"", NULL);
        return TCL_ERROR;
    }

    if (objc > 3 && OPT_CMP(Tcl_GetString(objv[1]), "-lock")) {
        mutexHandle = Tcl_GetString(objv[2]);
        ret = GetObjFromHandle(interp, MUTEXID, mutexHandle, (void**)&mutexPtr);
        if (ret != TCL_OK) {
            return TCL_ERROR;
        }
        optx = 3;
    } else {
        optx = 1;
        mutexPtr = &evalMutex;
    }

    objc -= optx;
    Tcl_MutexLock(mutexPtr);

    /*
     * Evaluate passed arguments as Tcl script. Note that
     * Tcl_EvalObjEx throws away the passed object by 
     * doing an decrement reference count on it. This also
     * means we need not build object bytecode rep.
     */
    
    if (objc == 1) {
        scriptObj = Tcl_DuplicateObj(objv[optx]);
    } else {
        scriptObj = Tcl_ConcatObj(objc, objv + optx);
    }

    ret = Tcl_EvalObjEx(interp, scriptObj, TCL_EVAL_DIRECT);

    if (ret == TCL_ERROR) {
        char msg[32 + TCL_INTEGER_SPACE];   
        sprintf(msg, "\n    (\"eval\" body line %d)", interp->errorLine);
        Tcl_AddObjErrorInfo(interp, msg, -1);
    }

    /*
     * Double-check the mutex handle in case somebody
     * has deleted the mutex in the evaluated script.
     * This is considered to be A Bad Thing.
     */
    
    if (mutexPtr != &evalMutex) {
        if (GetObjFromHandle(NULL, MUTEXID, mutexHandle, (void**)&mutexPtr)
                == TCL_OK) {
            Tcl_MutexUnlock(mutexPtr);
        }
    } else {
        Tcl_MutexUnlock(mutexPtr);
    }

    return ret;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadCondObjCmd --
 *
 *    This procedure is invoked to process "thread::cond" Tcl command.
 *    See the user documentation for details on what it does.
 *
 * Results:
 *    A standard Tcl result.
 *
 * Side effects:
 *    See the user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadCondObjCmd(dummy, interp, objc, objv)
    ClientData dummy;                   /* Not used. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    int opt, ret;
    char *condHandle, *mutexHandle;
    Tcl_Time waitTime;
    Tcl_Mutex *mutexPtr = NULL;
    Tcl_Condition *condPtr = NULL;

    static CONST84 char *cmdOpts[] = {
        "create", "destroy", "notify", "wait", NULL
    };
    enum options {
        c_CREATE, c_DESTROY, c_NOTIFY, c_WAIT
    };

    /* 
     * Syntax:
     *
     *    thread::cond create
     *    thread::cond destroy <condHandle>
     *    thread::cond broadcast <condHandle>
     *    thread::cond wait <condHandle> <mutexHandle> ?timeout?
     */

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?args?");
        return TCL_ERROR;
    }
    ret = Tcl_GetIndexFromObj(interp, objv[1], cmdOpts, "option", 0, &opt);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }

    /*
     * Cover the "create" option since it needs no existing handle.
     */

    if (opt == (int)c_CREATE) {
        condPtr = (Tcl_Condition*)Tcl_Alloc(sizeof(Tcl_Condition));
        *condPtr = 0;
        SetHandleFromObj(interp, CONDID, (void*)condPtr);
        return TCL_OK;
    }

    /*
     * All others require at least a valid handle. See if we got any...
     */

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 2, objv, "condHandle ?args?");
        return TCL_ERROR;
    }
    condHandle = Tcl_GetString(objv[2]);
    ret = GetObjFromHandle(interp, CONDID, condHandle, (void**)&condPtr);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }

    switch ((enum options)opt) {
    case c_WAIT:

        /*
         * May improve the Tcl_ConditionWait() to report timeouts so we can
         * inform script programmer about this interesting fact. I think 
         * there is still a place for something like Tcl_ConditionWaitEx()
         * or similar in the core.
         */

        if (objc < 4 || objc > 5) {
            Tcl_WrongNumArgs(interp, 3, objv, "mutexHandle ?timeout?");
            return TCL_ERROR;
        }
        mutexHandle = Tcl_GetString(objv[3]);
        ret = GetObjFromHandle(interp,MUTEXID,mutexHandle,(void**)&mutexPtr);
        if (ret != TCL_OK) {
            return TCL_ERROR;
        }
        if (objc == 5) {
            int timeMsec;
            if (Tcl_GetIntFromObj(interp, objv[4], &timeMsec) != TCL_OK) {
                return TCL_ERROR;
            }
            waitTime.sec  = timeMsec/1000;
            waitTime.usec = (timeMsec%1000) * 1000;
            Tcl_ConditionWait(condPtr, mutexPtr, &waitTime);
        } else {
            Tcl_ConditionWait(condPtr, mutexPtr, NULL);
        }
        break;
    case c_NOTIFY:
        Tcl_ConditionNotify(condPtr);
        break;
    case c_DESTROY: 
        Tcl_ConditionFinalize(condPtr);
        Tcl_Free((char*)condPtr);
        DeleteObjHandle(condHandle);
        break;
    case c_CREATE: /* Already handled above */
        break;
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * SetHandleFromObj --
 *
 *      Take an pointer and convert it into a opaque object Tcl handle.
 *      Returns the string-handle in interpreter result.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies the interpreter result.
 *
 *----------------------------------------------------------------------
 */

static void
SetHandleFromObj(interp, type, addrPtr)
    Tcl_Interp *interp;                 /* Interpreter to set result. */
    int type;                           /* Type: MUTEXID or CONDID. */
    void *addrPtr;                      /* Object address */
{
    int new;
    char handle[20];
    Tcl_HashEntry *hashEntryPtr;

#ifdef NS_AOLSERVER
    sprintf(handle, "%cid%p", type, addrPtr);
#else
    sprintf(handle, "%cid%u", type, syncHandlesCounter++);
    /*
     * By their very nature, object handles are always unique
     * (the counter!) so no need to check for duplicates.
     */
    GRAB_SYNCMUTEX;    
    hashEntryPtr = Tcl_CreateHashEntry(&syncHandles, handle, &new);
    Tcl_SetHashValue(hashEntryPtr, (ClientData)addrPtr);
    RELEASE_SYNCMUTEX;
#endif /* NS_AOLSERVER */
    Tcl_SetObjResult(interp, Tcl_NewStringObj(handle, -1));

    return;
}

/*
 *----------------------------------------------------------------------
 *
 * GetObjFromHandle --
 *
 *      Take an opaque object Tcl handle and convert it into a pointer. 
 *
 * Results:
 *      Standard Tcl result. 
 *
 * Side effects:
 *      On error, error message will be left in interpreter result.
 *
 *----------------------------------------------------------------------
 */

static int
GetObjFromHandle(interp, type, handle, addrPtrPtr)
    Tcl_Interp *interp;                 /* Interpreter for error msg. */
    int type;                           /* Type: MUTEXID or CONDID. */
    char *handle;                       /* Tcl string handle */
    void **addrPtrPtr;                  /* Return object address here */
{
#ifdef NS_AOLSERVER
    void *addrPtr = NULL;
#endif
    Tcl_HashEntry *hashEntryPtr;

    if (handle[0] != type || handle[1] != 'i' || handle[2] != 'd'
#ifdef NS_AOLSERVER
        || sscanf(handle+3, "%p", &addrPtr) != 1 || addrPtr == NULL
#endif
        ) {
        goto badhandle;
    }
#ifdef NS_AOLSERVER
    *addrPtrPtr = addrPtr;
#else
    GRAB_SYNCMUTEX;
    hashEntryPtr = Tcl_FindHashEntry(&syncHandles, handle);
    if (hashEntryPtr == (Tcl_HashEntry *)NULL) {
        RELEASE_SYNCMUTEX;
        goto badhandle;
    }
    *addrPtrPtr = (void *)Tcl_GetHashValue(hashEntryPtr);
    RELEASE_SYNCMUTEX;
#endif /* NS_AOLSERVER */

    return TCL_OK;

 badhandle:
    if (interp != NULL) {
        Tcl_AppendResult(interp, "invalid handle \"", handle, "\"", NULL);
    }
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * DeleteObjHandle --
 *
 *      Deletes the object handle from internal hash table.
 *      Caller has to take care about disposing the object.
 *
 * Results:
 *      Standard Tcl result. 
 *
 * Side effects:
 *      Handle gets deleted from the hash table 
 *
 *----------------------------------------------------------------------
 */

static void
DeleteObjHandle(handle)
    char *handle;                       /* Tcl string handle */
{
    Tcl_HashEntry *hashEntryPtr;
    
    GRAB_SYNCMUTEX;
    hashEntryPtr = Tcl_FindHashEntry(&syncHandles, handle);
    Tcl_DeleteHashEntry(hashEntryPtr);
    RELEASE_SYNCMUTEX;
} 

/*
 *----------------------------------------------------------------------
 *
 * Sp_Init --
 *
 *      Create commands in current interpreter..
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Creates new commands in current interpreter, initializes 
 *      shared hash table for storing sync primitives handles/pointers.
 *
 *----------------------------------------------------------------------
 */

int
Sp_Init (interp)
    Tcl_Interp *interp;                 /* Interp where to create cmds */
{
    static int initialized;

    if (!initialized) {
        GRAB_SYNCMUTEX;
        if (!initialized) {
            Tcl_InitHashTable(&syncHandles, TCL_STRING_KEYS);
            /*
             * We should not be garbage-collecting sync 
             * primitives because there is no way to 
             * find out who/if is using them. Generally, 
             * this should be delegated to the programmer.
             */
            initialized = 1;
        }
        RELEASE_SYNCMUTEX;
    }

    TCL_CMD(interp, THNS"::mutex", ThreadMutexObjCmd);
    TCL_CMD(interp, THNS"::cond",  ThreadCondObjCmd);
    TCL_CMD(interp, THNS"::eval",  ThreadEvalObjCmd);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * FinalizeSp --
 *
 *      Garbage-collect hash table on application exit. 
 *      NOTE: this function should not be used!
 *
 * Results:
 *      Standard Tcl result. 
 *
 * Side effects:
 *      Memory gets reclaimed.  
 *
 *----------------------------------------------------------------------
 */

static void
FinalizeSp (clientData)
    ClientData clientData;              /* Not used. */
{
    char *objHdl;
    void *objPtr;
    Tcl_HashEntry *entryPtr;
    Tcl_HashSearch search;

    GRAB_SYNCMUTEX;

    for (entryPtr = Tcl_FirstHashEntry(&syncHandles, &search);
            entryPtr; entryPtr = Tcl_NextHashEntry(&search)) {
        objHdl = (char*)Tcl_GetHashKey(&syncHandles, entryPtr);
        objPtr = (void*)Tcl_GetHashValue(entryPtr);
        switch((char)*objHdl) {
        case MUTEXID: Tcl_MutexFinalize((Tcl_Mutex*)objPtr);
        case CONDID:  Tcl_ConditionFinalize((Tcl_Condition*)objPtr);
        }
        Tcl_Free((char*)objPtr);
        Tcl_DeleteHashEntry(entryPtr);
    }
    Tcl_DeleteHashTable(&syncHandles);

    RELEASE_SYNCMUTEX;
}

/* EOF $RCSfile: threadSpCmd.c,v $ */

/* Emacs Setup Variables */
/* Local Variables:      */
/* mode: C               */
/* indent-tabs-mode: nil */
/* c-basic-offset: 4     */
/* End:                  */
