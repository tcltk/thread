/* 
 * threadSpCmd.c --
 *
 * This file implements commands for script-level access to thread 
 * synchronization primitives. Currently, the regular mutex, the
 * recursive mutex. the reader/writer mutex and condition variable 
 * objects are exposed to the script programmer.
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
 * RCS: @(#) $Id: threadSpCmd.c,v 1.17 2003/12/22 21:20:59 vasiljevic Exp $
 * ----------------------------------------------------------------------------
 */

#include "tclThread.h"
#include "threadSpCmd.h"

/*
 * Structure representing one generic mutex. 
 * We use this to transparently handle both
 * regular and recursive mutexes.
 */

typedef struct SpMutex {
    char type;   /* Type of the mutex: 'm'-regular or 'r'-recursive */
    void *lock;  /* Opaque mutex handle */
} SpMutex;

/*
 * Wrapper functions for handling generic types
 * of mutexes (both regular and recursive).
 */

static void SpMutexLock(char type, void **mutexPtr);
static void SpMutexUnlock(char type, void **mutexPtr);
static void SpMutexFinalize(char type, void **mutexPtr);

/*
 * This global (static) data is used to map opaque Tcl-level handles 
 * to pointers of their corresponding synchronization objects.
 */

static Tcl_HashTable syncHandles;       /* Handles/pointers to sync objects */
static Tcl_Mutex syncHandlesMutex;      /* Protects the above table */
static unsigned int syncHandlesCounter; /* Counter for unique object id's */

/*
 * Types of synchronization variables we support.
 */

#define EMUTEXID  'm' /* First letter of the exclusive mutex handle */
#define RMUTEXID  'r' /* First letter of the recursive mutex handle */
#define WMUTEXID  'w' /* First letter of the read/write mutex handle */
#define CONDID    'c' /* First letter of the condition variable handle */

/*
 * Functions implementing Tcl commands
 */

static Tcl_ObjCmdProc ThreadMutexObjCmd;
static Tcl_ObjCmdProc ThreadRWMutexObjCmd;
static Tcl_ObjCmdProc ThreadCondObjCmd;
static Tcl_ObjCmdProc ThreadEvalObjCmd;

/*
 * Forward declaration of functions used only within this file
 */

static int  GetObjFromHandle  _ANSI_ARGS_((Tcl_Interp *interp, char *id,
                                           void **addrPtrPtr));
static void SetHandleFromObj  _ANSI_ARGS_((Tcl_Interp *interp, int type, 
                                           void *addrPtr));
static void DeleteObjHandle   _ANSI_ARGS_((char *id));
static void SpFinalizeAll     _ANSI_ARGS_((ClientData clientData));
static void SpMutexLock       _ANSI_ARGS_((char type, void **mutexPtr));
static void SpMutexUnlock     _ANSI_ARGS_((char type, void **mutexPtr));
static void SpMutexFinalize   _ANSI_ARGS_((char type, void **mutexPtr));


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
    char *mutexHandle, type;
    SpMutex *mutex;

    static CONST84 char *cmdOpts[] = {
        "create", "destroy", "lock", "unlock", NULL
    };
    enum options {
        m_CREATE, m_DESTROY, m_LOCK, m_UNLOCK,
    };
    
    /* 
     * Syntax:
     *
     *     thread::mutex create ?-recursive?
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
     * Cover the "create" option first, since it needs no existing handle.
     * It should allow the "-readwrite" qualifier, though.
     */

    if (opt == (int)m_CREATE) {
        char *arg;
        if (objc == 2) {
            type = EMUTEXID;
        } else if (objc > 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "?-recursive?");
            return TCL_ERROR;
        } else {
            arg = Tcl_GetStringFromObj(objv[2], NULL);
            if (OPT_CMP(arg, "-recursive")) {
                type = RMUTEXID;
            } else {
                Tcl_WrongNumArgs(interp, 2, objv, "?-recursive?");
                return TCL_ERROR;
            }
        }
        mutex = (SpMutex*)Tcl_Alloc(sizeof(SpMutex));
        mutex->lock = NULL; /* Will be auto-initialized */
        mutex->type = type;
        SetHandleFromObj(interp, type, (void*)mutex);
        return TCL_OK;
    }

    /*
     * All other options require a valid handle.
     */

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 2, objv, "mutexHandle");
        return TCL_ERROR;
    }

    mutexHandle = Tcl_GetString(objv[2]);
    ret = GetObjFromHandle(interp, mutexHandle, (void**)&mutex);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }
    if (mutex->type != EMUTEXID && mutex->type != RMUTEXID) {
        Tcl_AppendResult(interp, "wrong mutex type", NULL);
        return TCL_ERROR;
    }

    /*
     * Having found the correct handle, take care about other options.
     */

    switch ((enum options)opt) {
    case m_LOCK:
        SpMutexLock(mutex->type, &mutex->lock);
        break;
    case m_UNLOCK:
        SpMutexUnlock(mutex->type, &mutex->lock);
        break;
    case m_DESTROY:
        SpMutexFinalize(mutex->type, &mutex->lock);
        Tcl_Free((char*)mutex);
        DeleteObjHandle(mutexHandle);
        break;
    default:
        break;
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadRwMutexObjCmd --
 *
 *    This procedure is invoked to process "thread::rwmutex" Tcl command.
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
ThreadRWMutexObjCmd(dummy, interp, objc, objv)
    ClientData dummy;                   /* Not used. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    int opt, ret;
    char *mutexHandle, type;
    SpMutex *mutex;

    static CONST84 char *cmdOpts[] = {
        "create", "destroy", "rlock", "wlock", "unlock", NULL
    };
    enum options {
        w_CREATE, w_DESTROY, w_RLOCK, w_WLOCK, w_UNLOCK,
    };
    
    /* 
     * Syntax:
     *
     *     thread::rwmutex create
     *     thread::rwmutex destroy <mutexHandle>
     *     thread::rwmutex rlock <mutexHandle>
     *     thread::rwmutex wlock <mutexHandle>
     *     thread::rwmutex unlock <mutexHandle>
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
     * Cover the "create" option first, since it needs no existing handle.
     */

    if (opt == (int)w_CREATE) {
        type = WMUTEXID;
        mutex = (SpMutex*)Tcl_Alloc(sizeof(SpMutex));
        mutex->lock = NULL; /* Will be auto-initialized */
        mutex->type = type;
        SetHandleFromObj(interp, type, (void*)mutex);
        return TCL_OK;
    }

    /*
     * All other options require a valid handle.
     */

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 2, objv, "mutexHandle");
        return TCL_ERROR;
    }

    mutexHandle = Tcl_GetString(objv[2]);
    ret = GetObjFromHandle(interp, mutexHandle, (void**)&mutex);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }
    if (mutex->type != WMUTEXID) {
        Tcl_AppendResult(interp, "wrong mutex type", NULL);
        return TCL_ERROR;
    }

    /*
     * Having found the correct handle, take care about other options.
     */

    switch ((enum options)opt) {
    case w_RLOCK:
        Sp_ReadWriteMutexRLock((Sp_ReadWriteMutex*)&mutex->lock);
        break;
    case w_WLOCK:
        Sp_ReadWriteMutexWLock((Sp_ReadWriteMutex*)mutex->lock);
        break;
    case w_UNLOCK:
        Sp_ReadWriteMutexUnlock((Sp_ReadWriteMutex*)&mutex->lock);
        break;
    case w_DESTROY:
        Sp_ReadWriteMutexFinalize((Sp_ReadWriteMutex*)&mutex->lock);
        Tcl_Free((char*)mutex);
        DeleteObjHandle(mutexHandle);
        break;
    default:
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
    SpMutex *mutex;
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
        ret = GetObjFromHandle(interp, mutexHandle, (void**)&mutex);
        if (ret != TCL_OK) {
            return TCL_ERROR;
        }
        if (mutex->type != EMUTEXID && mutex->type != RMUTEXID) {
            Tcl_AppendResult(interp, "wrong mutex type");
            return TCL_ERROR;
        }
        optx = 3;
        SpMutexLock(mutex->type, &mutex->lock);
    } else {
        optx = 1;
        mutexHandle = "internal";
        Tcl_MutexLock(&evalMutex);
    }

    objc -= optx;

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

    Tcl_IncrRefCount(scriptObj);
    ret = Tcl_EvalObjEx(interp, scriptObj, TCL_EVAL_DIRECT);
    Tcl_DecrRefCount(scriptObj);

    if (ret == TCL_ERROR) {
        char msg[32 + TCL_INTEGER_SPACE];   
        sprintf(msg, "\n    (\"eval\" body line %d)", interp->errorLine);
        Tcl_AddObjErrorInfo(interp, msg, -1);
    }

    /*
     * Double-check the mutex handle in case somebody
     * has deleted the mutex in the evaluated script.
     * This is considered to be A Bad Thing!
     */
    
    if (strcmp(mutexHandle, "internal")) {
        if (GetObjFromHandle(NULL, mutexHandle,(void**)&mutex) == TCL_OK) {
            SpMutexUnlock(mutex->type, &mutex->lock);
        }
    } else {
        Tcl_MutexUnlock(&evalMutex);
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
    SpMutex *mutex = NULL;
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
    ret = GetObjFromHandle(interp, condHandle, (void**)&condPtr);
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
        ret = GetObjFromHandle(interp, mutexHandle, (void**)&mutex);
        if (ret != TCL_OK) {
            return TCL_ERROR;
        }
        if (mutex->type != EMUTEXID) {
            Tcl_AppendResult(interp, "wrong mutex type", NULL);
            return TCL_ERROR;
        }
        if (mutex->lock == NULL) {
            Tcl_AppendResult(interp, "mutex never locked", NULL);
            return TCL_ERROR;
        }
        if (objc == 5) {
            int timeMsec;
            if (Tcl_GetIntFromObj(interp, objv[4], &timeMsec) != TCL_OK) {
                return TCL_ERROR;
            }
            waitTime.sec  = timeMsec/1000;
            waitTime.usec = (timeMsec%1000) * 1000;
            Tcl_ConditionWait(condPtr, (Tcl_Mutex*)&mutex->lock, &waitTime);
        } else {
            Tcl_ConditionWait(condPtr, (Tcl_Mutex*)&mutex->lock, NULL);
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
    default:
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
    int type;                           /* Type: EMUTEXID or CONDID. */
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

    Tcl_MutexLock(&syncHandlesMutex);
    hashEntryPtr = Tcl_CreateHashEntry(&syncHandles, handle, &new);
    Tcl_SetHashValue(hashEntryPtr, (ClientData)addrPtr);
    Tcl_MutexUnlock(&syncHandlesMutex);

#endif /* NS_AOLSERVER */

    Tcl_SetObjResult(interp, Tcl_NewStringObj(handle, -1));
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
GetObjFromHandle(interp, handle, addrPtrPtr)
    Tcl_Interp *interp;                 /* Interpreter for error msg. */
    char *handle;                       /* Tcl string handle */
    void **addrPtrPtr;                  /* Return object address here */
{
#ifdef NS_AOLSERVER
    void *addrPtr = NULL;
#endif
    Tcl_HashEntry *hashEntryPtr;

    if (handle[1] != 'i' || handle[2] != 'd'
#ifdef NS_AOLSERVER
        || sscanf(handle+3, "%p", &addrPtr) != 1 || addrPtr == NULL
#endif
        ) {
        goto badhandle;
    }
#ifdef NS_AOLSERVER
    *addrPtrPtr = addrPtr;
#else
    Tcl_MutexLock(&syncHandlesMutex);
    hashEntryPtr = Tcl_FindHashEntry(&syncHandles, handle);
    if (hashEntryPtr == (Tcl_HashEntry *)NULL) {
        Tcl_MutexUnlock(&syncHandlesMutex);
        goto badhandle;
    }
    *addrPtrPtr = (void *)Tcl_GetHashValue(hashEntryPtr);
    Tcl_MutexUnlock(&syncHandlesMutex);
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
    Tcl_MutexLock(&syncHandlesMutex);
    Tcl_DeleteHashEntry(Tcl_FindHashEntry(&syncHandles, handle));
    Tcl_MutexUnlock(&syncHandlesMutex);
} 

/*
 *----------------------------------------------------------------------
 *
 * Sp_Init --
 *
 *      Create commands in current interpreter.
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
        Tcl_MutexLock(&syncHandlesMutex);
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
        Tcl_MutexUnlock(&syncHandlesMutex);
    }

    TCL_CMD(interp, THNS"::mutex",   ThreadMutexObjCmd);
    TCL_CMD(interp, THNS"::rwmutex", ThreadRWMutexObjCmd);
    TCL_CMD(interp, THNS"::cond",    ThreadCondObjCmd);
    TCL_CMD(interp, THNS"::eval",    ThreadEvalObjCmd);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * SpFinalizeAll --
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
SpFinalizeAll (clientData)
    ClientData clientData;              /* Not used. */
{
    char *objHdl;
    void *objPtr;
    Tcl_HashEntry *entryPtr;
    Tcl_HashSearch search;

    Tcl_MutexLock(&syncHandlesMutex);

    for (entryPtr = Tcl_FirstHashEntry(&syncHandles, &search);
            entryPtr; entryPtr = Tcl_NextHashEntry(&search)) {
        objHdl = (char*)Tcl_GetHashKey(&syncHandles, entryPtr);
        objPtr = (void*)Tcl_GetHashValue(entryPtr);
        switch((char)*objHdl) {
        case EMUTEXID:
        case RMUTEXID:
        case WMUTEXID:
            SpMutexFinalize((char)*objHdl, &objPtr);
            break;
        case CONDID:
            Tcl_ConditionFinalize((Tcl_Condition*)objPtr);
            break;
        }
        Tcl_Free((char*)objPtr);
        Tcl_DeleteHashEntry(entryPtr);
    }
    Tcl_DeleteHashTable(&syncHandles);

    Tcl_MutexUnlock(&syncHandlesMutex);
}

/*
 *----------------------------------------------------------------------
 *
 * SpMutexLock --
 *
 *      Locks the typed mutex.
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
SpMutexLock(char type, void **mutexPtr)
{
    switch (type) {
    case EMUTEXID: 
        Tcl_MutexLock((Tcl_Mutex*)mutexPtr); 
        break;
    case RMUTEXID: 
        Sp_RecursiveMutexLock((Sp_RecursiveMutex*)mutexPtr);
        break;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * SpMutexUnlock --
 *
 *      Unlocks the typed mutex.
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
SpMutexUnlock(char type, void **mutexPtr)
{
    switch (type) {
    case EMUTEXID: 
        Tcl_MutexUnlock((Tcl_Mutex*)mutexPtr);
        break;
    case RMUTEXID:
        Sp_RecursiveMutexUnlock((Sp_RecursiveMutex*)mutexPtr);
        break;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * SpMutexFinalize --
 *
 *      Finalizes the typed mutex.
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
SpMutexFinalize(char type, void **mutexPtr)
{
    switch (type) {
    case EMUTEXID:
        Tcl_MutexFinalize((Tcl_Mutex*)mutexPtr);
        break;
    case RMUTEXID:
        Sp_RecursiveMutexFinalize((Sp_RecursiveMutex*)mutexPtr);
        break;
    case WMUTEXID:
        Sp_ReadWriteMutexFinalize((Sp_ReadWriteMutex*)mutexPtr);
        break;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Sp_RecursiveMutexLock --
 *
 *      Locks the recursive mutex.
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
Sp_RecursiveMutexLock(Sp_RecursiveMutex *muxPtr)
{
    Sp_RecursiveMutex_ *rmPtr;
    Tcl_ThreadId self = Tcl_GetCurrentThread();

    /*
     * Allocate the mutex structure on first access
     */

    if (*muxPtr == 0) {
        Tcl_MutexLock(&syncHandlesMutex);
        if (*muxPtr == 0) {
            *muxPtr=(Sp_RecursiveMutex_*)Tcl_Alloc(sizeof(Sp_RecursiveMutex_));
            memset(*muxPtr, 0, sizeof(Sp_RecursiveMutex_));
        }
        Tcl_MutexUnlock(&syncHandlesMutex);
    }

    rmPtr = *(Sp_RecursiveMutex_**)muxPtr;
    Tcl_MutexLock(&rmPtr->lock);
    
    if (rmPtr->owner == self) {
        /*
         * We are already holding the mutex
         * so just count one more lock.
         */
    	rmPtr->lrcnt++;
    } else {
    	if (rmPtr->owner == (Tcl_ThreadId)0) {
            /*
             * Nobody holds the mutex, we do now.
             */
    		rmPtr->owner = self;
    		rmPtr->lrcnt = 1;
    	} else {
            /*
             * Somebody else holds the mutex; wait.
             */
    		while (1) {
                Tcl_ConditionWait(&rmPtr->cond, &rmPtr->lock, NULL);
    			if (rmPtr->owner == (Tcl_ThreadId)0) {
    				rmPtr->owner = self;
    				rmPtr->lrcnt = 1;
    				break;
    			}
    		}
    	}
    }

    Tcl_MutexUnlock(&rmPtr->lock);
}

/*
 *----------------------------------------------------------------------
 *
 * Sp_RecursiveMutexUnlock --
 *
 *      Unlock the recursive mutex.
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
Sp_RecursiveMutexUnlock(Sp_RecursiveMutex *muxPtr)
{
    Sp_RecursiveMutex_ *rmPtr;

    if (*muxPtr == NULL) {
        return; /* Never locked before */
    }

    rmPtr = *(Sp_RecursiveMutex_**)muxPtr;
    Tcl_MutexLock(&rmPtr->lock);

    if (--rmPtr->lrcnt <= 0) {
        rmPtr->lrcnt = 0;
        rmPtr->owner = (Tcl_ThreadId)0;
        Tcl_ConditionNotify(&rmPtr->cond);
    }

    Tcl_MutexUnlock(&rmPtr->lock);
}

/*
 *----------------------------------------------------------------------
 *
 * Sp_RecursiveMutexFinalize --
 *
 *      Finalize the recursive mutex.
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
Sp_RecursiveMutexFinalize(Sp_RecursiveMutex *muxPtr)
{
    if (*muxPtr != NULL) {
        Sp_RecursiveMutex_ *rmPtr = *(Sp_RecursiveMutex_**)muxPtr;
        if (rmPtr->lock) {
            Tcl_MutexFinalize(&rmPtr->lock);
        }
        if (rmPtr->cond) {
            Tcl_ConditionFinalize(&rmPtr->cond);
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Sp_ReadWriteMutexRLock --
 *
 *      Read-locks the reader/writer mutex.
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
Sp_ReadWriteMutexRLock(Sp_ReadWriteMutex *muxPtr)
{
    Sp_ReadWriteMutex_ *rwPtr;

    /*
     * Allocate the mutex structure on first access
     */

    if (*muxPtr == NULL) {
        Tcl_MutexLock(&syncHandlesMutex);
        if (*muxPtr == NULL) {
            *muxPtr=(Sp_ReadWriteMutex_*)Tcl_Alloc(sizeof(Sp_ReadWriteMutex_));
            memset(*muxPtr, 0, sizeof(Sp_ReadWriteMutex_));
        }
        Tcl_MutexUnlock(&syncHandlesMutex);
    }

    rwPtr = *(Sp_ReadWriteMutex_**)muxPtr;
    Tcl_MutexLock(&rwPtr->lock);

    while (rwPtr->lrcnt < 0 || rwPtr->numwr > 0) {
        rwPtr->numrd++;
        Tcl_ConditionWait(&rwPtr->rcond, &rwPtr->lock, NULL);
        rwPtr->numrd--;
    }
    rwPtr->lrcnt++;

    Tcl_MutexUnlock(&rwPtr->lock);
}

/*
 *----------------------------------------------------------------------
 *
 * Sp_ReadWriteMutexWLock --
 *
 *      Write-locks the reader/writer mutex.
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
Sp_ReadWriteMutexWLock(Sp_ReadWriteMutex *muxPtr)
{
    Sp_ReadWriteMutex_ *rwPtr;

    /*
     * Allocate the mutex structure on first access
     */

    if (*muxPtr == NULL) {
        Tcl_MutexLock(&syncHandlesMutex);
        if (*muxPtr == NULL) {
            *muxPtr=(Sp_ReadWriteMutex_*)Tcl_Alloc(sizeof(Sp_ReadWriteMutex_));
            memset(*muxPtr, 0, sizeof(Sp_ReadWriteMutex_));
        }
        Tcl_MutexUnlock(&syncHandlesMutex);
    }

    rwPtr = *(Sp_ReadWriteMutex_**)muxPtr;
    Tcl_MutexLock(&rwPtr->lock);

    while (rwPtr->lrcnt != 0) {
        rwPtr->numwr++;
        Tcl_ConditionWait(&rwPtr->wcond, &rwPtr->lock, NULL);
        rwPtr->numwr--;
    }
    rwPtr->lrcnt = -1; /* This designates the sole writer */

    Tcl_MutexUnlock(&rwPtr->lock);
}

/*
 *----------------------------------------------------------------------
 *
 * Sp_ReadWriteMutexUnlock --
 *
 *      Unlock the reader/writer mutex.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

void
Sp_ReadWriteMutexUnlock(Sp_ReadWriteMutex *muxPtr)
{
    Sp_ReadWriteMutex_ *rwPtr;

    if (*muxPtr == NULL) {
        return; /* Never locked before */
    }
    
    rwPtr = *(Sp_ReadWriteMutex_**)muxPtr;
    Tcl_MutexLock(&rwPtr->lock);

    if (--rwPtr->lrcnt < 0) {
        rwPtr->lrcnt = 0;
    }
    if (rwPtr->numwr) {
        Tcl_ConditionNotify(&rwPtr->wcond);
    } else if (rwPtr->numrd) {
        Tcl_ConditionNotify(&rwPtr->rcond);
    }

    Tcl_MutexUnlock(&rwPtr->lock);
}

/*
 *----------------------------------------------------------------------
 *
 * Sp_ReadWriteMutexFinalize --
 *
 *      Finalizes the reader/writer mutex.
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
Sp_ReadWriteMutexFinalize(Sp_ReadWriteMutex *muxPtr)
{
    if (*muxPtr != NULL) {
        Sp_ReadWriteMutex_ *rwPtr = *(Sp_ReadWriteMutex_**)muxPtr;
        if (rwPtr->lock) {
            Tcl_MutexFinalize(&rwPtr->lock);
        }
        if (rwPtr->rcond) {
            Tcl_ConditionFinalize(&rwPtr->rcond);
        }
        if (rwPtr->wcond) {
            Tcl_ConditionFinalize(&rwPtr->wcond);
        }
    }
}

/* EOF $RCSfile: threadSpCmd.c,v $ */

/* Emacs Setup Variables */
/* Local Variables:      */
/* mode: C               */
/* indent-tabs-mode: nil */
/* c-basic-offset: 4     */
/* End:                  */
