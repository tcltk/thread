/* 
 * threadSpCmd.c --
 *
 * This file implements commands for script-level access to thread 
 * synchronization primitives. Currently, the mutex and condition-variable
 * objects are exposed to the script programmer.
 *
 * Copyright (c) 1998 by Sun Microsystems, Inc.
 * Copyright (c) 1999,2000 by Scriptics Corporation.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: threadSpCmd.c,v 1.5 2002/06/17 20:22:55 vasiljevic Exp $
 * ----------------------------------------------------------------------------
 */

#include "tclThread.h"

/*
 * This global (static) data is used to map opaque Tcl-level handles 
 * to pointers of their corresponding synchronization objects.
 */

static Tcl_HashTable syncHandles;       /* Handles/pointers to sync objects */
static Tcl_Mutex syncHandlesMutex;      /* Protects the above table */

static int syncHandlesInitialized;      /* Flag, table initalized or not */
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

/*
 * Forward declaration of functions used only within this file
 */

static int  GetObjFromHandle _ANSI_ARGS_((Tcl_Interp *interp, 
                                          char type, 
                                          char *id, 
                                          void **addrPtrPtr));

static void SetHandleFromObj _ANSI_ARGS_((Tcl_Interp *interp, 
                                          char type, 
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

    static CONST char *cmdOpts[] = {
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
    case m_CREATE: /* Already did above */    break;
    case m_LOCK:   Tcl_MutexLock(mutexPtr);   break;
    case m_UNLOCK: Tcl_MutexUnlock(mutexPtr); break;
    case m_DESTROY:
        Tcl_MutexFinalize(mutexPtr);
        Tcl_Free((char*)mutexPtr);
        DeleteObjHandle(mutexHandle);
        break;
    }

    return TCL_OK;
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

    static CONST char *cmdOpts[] = {
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
    case c_CREATE: break;
    case c_WAIT:

        /*
         * May improve the Tcl_ConditionWait() to report timeouts
         * so we can inform script programmer about this interesting fact.
         * I think threre is still a place for something like 
         * Tcl_ConditionWaitEx() or similar... 
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
    char type;                          /* Type: MUTEXID or CONDID. */
    void *addrPtr;                      /* Object address */
{
    int new;
    char handle[20];
    Tcl_HashEntry *hashEntryPtr;

    GRAB_SYNCMUTEX;

    sprintf(handle, "%cid%u", type, syncHandlesCounter++);

    /*
     * By their very nature, object handles are always unique
     * (the counter!) so no need to check for duplicates.
     */

    hashEntryPtr = Tcl_CreateHashEntry(&syncHandles, handle, &new);
    Tcl_SetHashValue(hashEntryPtr, (ClientData)addrPtr);

    RELEASE_SYNCMUTEX;

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
GetObjFromHandle(interp, type, handle, addrPtrPtr)
    Tcl_Interp *interp;                 /* Interpreter for error msg. */
    char type;                          /* Type: MUTEXID or CONDID. */
    char *handle;                       /* Tcl string handle */
    void **addrPtrPtr;                  /* Return object address here */
{
    Tcl_HashEntry *hashEntryPtr;

    if (handle[0] != type || handle[1] != 'i' || handle[2] != 'd') {
        Tcl_AppendResult(interp, "invalid handle \"", handle, "\"", NULL);
        return TCL_ERROR;
    }

    GRAB_SYNCMUTEX;
   
    hashEntryPtr = Tcl_FindHashEntry(&syncHandles, handle);
    if (hashEntryPtr == (Tcl_HashEntry *)NULL) {
        RELEASE_SYNCMUTEX;
        Tcl_AppendResult(interp, "no such handle \"", handle, "\"", NULL);
        return TCL_ERROR;        
    }
    *addrPtrPtr = (void *)Tcl_GetHashValue(hashEntryPtr);

    RELEASE_SYNCMUTEX;

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * DeleteObjHandle --
 *
 *      Deletes the object handle from internal hash table.
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
 *      Create the thread::mutex and thread::cond commands in current
 *      interpreter.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Creates new commands in current interpreter, registers
 *      new application exit handler, initializes shared hash table
 *      for storing sync primitives handles/pointers.
 *
 *----------------------------------------------------------------------
 */

void
Sp_Init (interp)
    Tcl_Interp *interp;                 /* Interp where to create cmds */
{
    if (!syncHandlesInitialized) {
        GRAB_SYNCMUTEX;
        if (!syncHandlesInitialized) {
            Tcl_InitHashTable(&syncHandles, TCL_STRING_KEYS);
            syncHandlesInitialized = 1; 
            Tcl_CreateExitHandler((Tcl_ExitProc *)FinalizeSp, NULL);
        }
        RELEASE_SYNCMUTEX;
    }

    TCL_CMD(interp, "thread::mutex", ThreadMutexObjCmd);
    TCL_CMD(interp, "thread::cond",  ThreadCondObjCmd);
}

/*
 *----------------------------------------------------------------------
 *
 * FinalizeSp --
 *
 *      Garbage-collect hash table on application exit. 
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
