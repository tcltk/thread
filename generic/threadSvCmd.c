/* 
 * threadSvCmd.c --
 *
 * This file implements the thread::sv_* family of commands.
 * Code is taken from nsd/tclvar.c found in AOLserver 3.0 distribution
 * and modified to support Tcl 8.0+ command object interface.

 * Copyright (c) 1998 by Sun Microsystems, Inc.
 * Copyright (c) 1999,2000 by Scriptics Corporation.
 * Copyright (c) 1999 America Online, Inc. All Rights Reserved.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: threadSvCmd.c,v 1.5 2000/10/16 21:27:26 zoran Exp $
 */

#include "thread.h"

/*
 * The following structure defines a collection of arrays.
 * Only the arrays within a given bucket share a lock, allowing
 * for more concurency.
 */

typedef struct Bucket {
    Tcl_Mutex *lock;
    Tcl_HashTable arrays;
} Bucket;

/*
 * The following structure maintains the context for each variable array.
 */

typedef struct Array {
    Bucket *bucketPtr;        /* Array bucket. */
    Tcl_HashEntry *entryPtr;  /* Entry in bucket array table. */
    Tcl_HashTable vars;       /* Table of variables. */
} Array;

#define FLAGS_CREATE   1
#define FLAGS_NOERRMSG 2

/*
 * This value is taken verbatim from AOLserver config. It defines number
 * of bucket to map shared arrays into, and is subject to change, depending
 * on bucket-contention issues. We may have to find a way to tune it
 * dynamicaly.
 */ 

#define NUMBUCKETS 8

/*
 * Forward declarations for commands and routines defined in this file.
 */

static void   SetVar     _ANSI_ARGS_((Array *, char *key, char *value));
static void   UpdateVar  _ANSI_ARGS_((Tcl_HashEntry *hPtr, char *value));
static void   FlushArray _ANSI_ARGS_((Array *arrayPtr));
static Array *LockArray  _ANSI_ARGS_((Tcl_Interp*, char *arrayObj, int flags));
static void   FinalizeSv _ANSI_ARGS_((ClientData clientData));

#define UnlockArray(arrayPtr) \
    Tcl_MutexUnlock(((arrayPtr)->bucketPtr->lock));

/*
 * Global variables used within this file.
 */

static Bucket *buckets;       /* Array of buckets. */
static Tcl_Mutex masterMutex; /* Protects the array of buckets */


/*
 *----------------------------------------------------------------------
 *
 * ThreadSvGetObjCmd --
 *
 *      This procedure is invoked to process "thread::sv_set" and
 *      "thread::sv_exists" commands.
 *      See the user documentation for details on what it does.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *----------------------------------------------------------------------
 */

    /* ARGSUSED */
int
ThreadSvGetObjCmd(arg, interp, objc, objv)
    ClientData arg;                     /* 'e' - "exists", 's' - "set" */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    Tcl_HashEntry *hPtr;
    Array *arrayPtr;
    int cmd = (int)arg;
    char *key, *arrayName;
    Tcl_Obj *valObj;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key");
        return TCL_ERROR;
    }

    /*
     * First, lock the array and try to find the key there
     */

    arrayName = Tcl_GetString(objv[1]);
    arrayPtr  = LockArray(interp, arrayName, 0);
    if (arrayPtr == NULL) {
        return TCL_ERROR;
    }
    key  = Tcl_GetString(objv[2]);
    hPtr = Tcl_FindHashEntry(&arrayPtr->vars, key);
    if (hPtr == NULL) {
        UnlockArray(arrayPtr);
        if (cmd != 'e') {
            Tcl_AppendResult(interp, "no key \"", key, "\" in array \"",
                    arrayName, "\"", NULL);
            return TCL_ERROR;
        }
        Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
        return TCL_OK;
    }

    /*
     * Depending on flag passed via client data we return
     * 1 (exists) or the variable's value (set)
     */

    if (cmd == 'e') {
        valObj = Tcl_NewIntObj(1);
    } else {
        valObj = Tcl_NewStringObj((char *) Tcl_GetHashValue(hPtr), -1);
    }
    Tcl_SetObjResult(interp, valObj);
    UnlockArray(arrayPtr);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadSvSetObjCmd --
 *
 *      This procedure is invoked to process the "thread::sv_set"
 *      command. See the user documentation for details on what it does.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *----------------------------------------------------------------------
 */

    /* ARGSUSED */
int
ThreadSvSetObjCmd(dummy, interp, objc, objv)
    ClientData dummy;                   /* Not used. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    Array *arrayPtr;
    char *arrayName, *key, *val;

    /*
     * Cover only setting the real value here. Interrogating value
     * i.e. 'set' w/o value argument, is handled by the get command.
     */

    if (objc != 3 && objc != 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key ?value?");
        return TCL_ERROR;
    } else if (objc == 3) {
        return ThreadSvGetObjCmd((ClientData)'s', interp, objc, objv);
    }
    arrayName = Tcl_GetString(objv[1]);

    arrayPtr = LockArray(interp, arrayName, FLAGS_CREATE);
    key = Tcl_GetString(objv[2]);
    val = Tcl_GetString(objv[3]);
    SetVar(arrayPtr, key, val);
    UnlockArray(arrayPtr);

    Tcl_SetObjResult(interp, objv[3]);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadSvIncrObjCmd --
 *
 *      This procedure is invoked to process the "thread::sv_incr"
 *      command. See the user documentation for details on what it does.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *----------------------------------------------------------------------
 */

    /* ARGSUSED */
int
ThreadSvIncrObjCmd(dummy, interp, objc, objv)
    ClientData dummy;                   /* Not used. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    Array *arrayPtr;
    int count, current, result;
    char buf[32], *value, *key, *arrayName;
    Tcl_HashEntry *hPtr;

    if (objc != 3 && objc != 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key ?count?");
        return TCL_ERROR;
    }
    if (objc == 3)  {
        count = 1;
    } else if (Tcl_GetIntFromObj(interp, objv[3], &count) != TCL_OK) {
        return TCL_ERROR;
    }

    arrayName = Tcl_GetString(objv[1]);
    arrayPtr  = LockArray(interp, arrayName, 0);
    if (arrayPtr == NULL) {
        return TCL_ERROR;
    }

    result = TCL_ERROR;

    key = Tcl_GetString(objv[2]);
    hPtr = Tcl_FindHashEntry(&arrayPtr->vars, key);
    if (hPtr != NULL) {
        value  = (char *) Tcl_GetHashValue(hPtr);
        result = Tcl_GetInt(interp, value, &current);
        if (result == TCL_OK) {
            current += count;
            sprintf(buf, "%d", current);
            SetVar(arrayPtr, key, buf);
        }
    }
    UnlockArray(arrayPtr);
    if (hPtr == NULL) {
        Tcl_AppendResult(interp, "no key \"", key, "\" in array ",
                arrayName, NULL);        
    } else if (result == TCL_OK) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(buf,-1));
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadSvAppendObjCmd --
 *
 *      This procedure is invoked to process the "thread::sv_append" 
 *      command. See the user documentation for details on what it does.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *----------------------------------------------------------------------
 */

    /* ARGSUSED */
int
ThreadSvAppendObjCmd(arg, interp, objc, objv)
    ClientData arg;                     /* 'l' - "lappend", 'a' - "append" */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    Array *arrayPtr;
    int cmd = (int)arg;
    char *key, *arrayName;
    int i = 0;
    Tcl_HashEntry *hPtr;
    
    if (objc < 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key ?value value ...?");
        return TCL_ERROR;
    }
    arrayName = Tcl_GetString(objv[1]);
    arrayPtr  = LockArray(interp, arrayName, 0);
    if (arrayPtr == NULL) {
        return TCL_ERROR;
    }
    key = Tcl_GetString(objv[2]);
    hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, key, &i);
    if (!i) {
        Tcl_SetResult(interp, (char *) Tcl_GetHashValue(hPtr), TCL_VOLATILE);
    }
    for (i = 3; i < objc; ++i) {
        char *elem = Tcl_GetString(objv[i]);
        switch (cmd) {
        case 'l': Tcl_AppendElement(interp, elem);      break;
        case 'a': Tcl_AppendResult(interp, elem, NULL); break;
        }
    }
    UpdateVar(hPtr, Tcl_GetStringResult(interp));
    UnlockArray(arrayPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadSvArrayObjCmd --
 *
 *      This procedure is invoked to process the "thread::sv_array"
 *      command. See the user documentation for details on what it does.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *----------------------------------------------------------------------
 */

    /* ARGSUSED */
int
ThreadSvArrayObjCmd(dummy, interp, objc, objv)
    ClientData dummy;                   /* Not used. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    Array *arrayPtr;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    char **largv, *pattern = NULL, *key, *arrayName;
    int i, largc, index;

    static char *opts[] = {
        "set", "reset", "get", "names", "size", "exists", NULL};
    enum options {
        ASET,  ARESET,  AGET,  ANAMES,  ASIZE,  AEXISTS
    };
    
    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "option array");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0, &index) 
            != TCL_OK) {
        return TCL_ERROR;
    }
    
    arrayName = Tcl_GetString(objv[2]);
   
    if (index == ASET || index == ARESET) {
        char *list;
        if (objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "array valueList");
            return TCL_ERROR;
        }
        list = Tcl_GetString(objv[3]);
        if (Tcl_SplitList(NULL, list, &largc, &largv) != TCL_OK) {
            return TCL_ERROR;
        }
        if (largc & 1) {
            Tcl_AppendResult(interp,
                    "list must have an even number of elements", NULL);
            Tcl_Free((char *)largv);
            return TCL_ERROR;
        }
        arrayPtr = LockArray(interp, arrayName, FLAGS_CREATE);
    } else {
        if (index == AGET || index == ANAMES) {
            if (objc != 3 && objc != 4) {
                Tcl_WrongNumArgs(interp, 2, objv, "array ?pattern?");
                return TCL_ERROR;
            }
            if (objc == 4) {
                pattern = Tcl_GetString(objv[3]);
            }
        } else if (index == ASIZE || index == AEXISTS) {
            if (objc != 3) {
                Tcl_WrongNumArgs(interp, 2, objv, "array");
                return TCL_ERROR;
            }
        }
        arrayPtr = LockArray(interp, arrayName, FLAGS_NOERRMSG);
        if (arrayPtr == NULL) {
            if (index == ASIZE || index == AEXISTS) {
                Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
            }
            return TCL_OK;
        }
    }
    
    switch (index) {
    case AEXISTS: 
        Tcl_SetObjResult(interp, Tcl_NewIntObj(1));
        break;

    case ASIZE:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(arrayPtr->vars.numEntries));
        break;

    case ARESET: 
        FlushArray(arrayPtr);
        /* FALL-THRU */

    case ASET:
        for (i = 0; i < largc; i += 2) {
            SetVar(arrayPtr, largv[i], largv[i+1]);
        }
        Tcl_Free((char *) largv);
        break;

    case AGET:
        /* FALL-THRU */

    case ANAMES:
        hPtr = Tcl_FirstHashEntry(&arrayPtr->vars, &search);
        while (hPtr != NULL) {
            key = Tcl_GetHashKey(&arrayPtr->vars, hPtr);
            if (pattern == NULL || Tcl_StringMatch(key, pattern)) {
                Tcl_AppendElement(interp, key);
                if (index == AGET) {
                    Tcl_AppendElement(interp, (char *) Tcl_GetHashValue(hPtr));
                }
            }
            hPtr = Tcl_NextHashEntry(&search);
        }
        break;
    }
    UnlockArray(arrayPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadSvUnsetObjCmd --
 *
 *      This procedure is invoked to process the "thread::sv_unset"
 *      command. See the user documentation for details on what it does.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *----------------------------------------------------------------------
 */

    /* ARGSUSED */
int
ThreadSvUnsetObjCmd(dummy, interp, objc, objv)
    ClientData dummy;                   /* Not used. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    Tcl_HashEntry *hPtr = NULL;
    Array *arrayPtr;
    char *key, *arrayName;
    
    if (objc != 2 && objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "array ?key?");
        return TCL_ERROR;
    }
    arrayName = Tcl_GetString(objv[1]);
    arrayPtr  = LockArray(interp, arrayName, 0);
    if (arrayPtr == NULL) {
        return TCL_ERROR;
    }
    if (objc == 2) {
        Tcl_DeleteHashEntry(arrayPtr->entryPtr);
    } else {
        key  = Tcl_GetString(objv[2]);
        hPtr = Tcl_FindHashEntry(&arrayPtr->vars, key);
        if (hPtr != NULL) {
            Tcl_Free((char *)Tcl_GetHashValue(hPtr));
            Tcl_DeleteHashEntry(hPtr);
        }
    }
    if (objc == 2) {
        FlushArray(arrayPtr);
        Tcl_DeleteHashTable(&arrayPtr->vars);
        UnlockArray(arrayPtr);
        Tcl_Free((char *)arrayPtr);
    } else if (hPtr == NULL) {
        UnlockArray(arrayPtr);
        Tcl_AppendResult(interp, "no key \"", key, "\" in array ",
                arrayName, NULL);
        return TCL_ERROR;
    } else {
        UnlockArray(arrayPtr);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * LockArray --
 *
 *      Find (or create) the Array structure for an array and lock it.
 *      Array structure must be later unlocked with UnlockArray.
 *
 * Results:
 *      TCL_OK or TCL_ERROR if no such array.
 *
 * Side effects;
 *      Sets *arrayPtrPtr with Array pointer or leave error
 *      in given interp.
 *
 *----------------------------------------------------------------------
 */
static Array *
LockArray(interp, array, flags)
    Tcl_Interp *interp;                 /* Interpreter to leave result. */
    char *array;                        /* Name of array to lock */
    int flags;                          /* FLAGS_CREATE or FLAGS_NOERRMSG */
{
    Bucket *bucketPtr;
    Tcl_HashEntry *hPtr;
    Array *arrayPtr;
    register char *p;
    register unsigned int result;
    register int i;
    int new;
   
    /*
     * Compute a hash to map an array to a bucket.
     */

    p = array;
    result = 0;
    while (*p++) {
        i = *p;
        result += (result<<3) + i;
    }
    i = result % NUMBUCKETS;
    bucketPtr = &buckets[i];
    
    /*
     * Now, lock the bucket, find the array, or create new one
     * depending on what we're asked for. The bucket will be left
     * locked on sucess.
     */

    Tcl_MutexLock(bucketPtr->lock);
    if (flags & FLAGS_CREATE) {
        hPtr = Tcl_CreateHashEntry(&bucketPtr->arrays, array, &new);
        if (!new) {
            arrayPtr = (Array *) Tcl_GetHashValue(hPtr);
        } else {
            arrayPtr = (Array *) Tcl_Alloc(sizeof(Array));
            arrayPtr->bucketPtr = bucketPtr;
            arrayPtr->entryPtr = hPtr;
            Tcl_InitHashTable(&arrayPtr->vars, TCL_STRING_KEYS);
            Tcl_SetHashValue(hPtr, arrayPtr);
        }
    } else {
        hPtr = Tcl_FindHashEntry(&bucketPtr->arrays, array);
        if (hPtr == NULL) {
            Tcl_MutexUnlock(bucketPtr->lock);
            if (!(flags & FLAGS_NOERRMSG)) {
                Tcl_AppendResult(interp, "no array \"", array, "\"", NULL);
            }
            return NULL;
        }
        arrayPtr = (Array *) Tcl_GetHashValue(hPtr);
    }
    return arrayPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * UpdateVar --
 *
 *      Update a variable entry.
 *
 * Results:
 *      None.
 *
 * Side effects;
 *      New value is set.
 *
 *----------------------------------------------------------------------
 */
static void
UpdateVar(hPtr, value)
    Tcl_HashEntry *hPtr;                /* Hash entry of var to update */
    char *value;                        /* Value to set for variable */
{
    char *oldVar, *newVar;
    size_t size;
    
    size = strlen(value) + 1;
    oldVar = (char *) Tcl_GetHashValue(hPtr);
    if (oldVar == NULL) {
        newVar = Tcl_Alloc(size);
    } else {
        newVar = Tcl_Realloc(oldVar, size);
    }
    memcpy(newVar, value, size);
    Tcl_SetHashValue(hPtr, newVar);
}

/*
 *----------------------------------------------------------------------
 *
 * SetVar --
 *
 *      Set (or reset) an array entry.
 *
 * Results:
 *      None.
 *
 * Side effects;
 *      New entry is created and updated.
 *
 *----------------------------------------------------------------------
 */
static void
SetVar(arrayPtr, key, value)
    Array *arrayPtr;                    /* Name of array containing var */
    char *key;                          /* Name of array key */
    char *value;                        /* Value to set for this key */
{
    Tcl_HashEntry *hPtr;
    int new;
    
    hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, key, &new);
    UpdateVar(hPtr, value);
}

/*
 *----------------------------------------------------------------------
 *
 * FlushArray --
 *
 *      Unset all keys in an array.
 *
 * Results:
 *      None.
 *
 * Side effects
 *      Array is cleaned but its variable hash-hable still lives.
 *
 *----------------------------------------------------------------------
 */
static void
FlushArray(arrayPtr)
    Array *arrayPtr;                    /* Name of array to flush */
{
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    
    hPtr = Tcl_FirstHashEntry(&arrayPtr->vars, &search);
    while (hPtr != NULL) {
        Tcl_Free((char *)Tcl_GetHashValue(hPtr));
        Tcl_DeleteHashEntry(hPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Initialize_Sv --
 *
 *      Creates thread::sv_* family of commands in current interpreter.
 *
 * Results:
 *    None.
 *
 * Side effects
 *    Many new command created in current interpreter. Global data
 *    structures used by them initialized as well.
 *
 *----------------------------------------------------------------------
 */
void
Initialize_Sv (interp)
    Tcl_Interp *interp;
{
    Bucket *bucketPtr;
    register int i;

    /*
     * Plug-in new commands
     */

    Tcl_CreateObjCommand(interp,"thread::sv_get",    ThreadSvGetObjCmd,
            (ClientData)'g', NULL);
    Tcl_CreateObjCommand(interp,"thread::sv_exists", ThreadSvGetObjCmd,
            (ClientData)'e', NULL);
    Tcl_CreateObjCommand(interp,"thread::sv_set",    ThreadSvSetObjCmd,
            (ClientData)NULL, NULL);
    Tcl_CreateObjCommand(interp,"thread::sv_incr",   ThreadSvIncrObjCmd,
            (ClientData)NULL, NULL);
    Tcl_CreateObjCommand(interp,"thread::sv_append", ThreadSvAppendObjCmd,
            (ClientData)'a', NULL);
    Tcl_CreateObjCommand(interp,"thread::sv_lappend",ThreadSvAppendObjCmd,
            (ClientData)'l', NULL);
    Tcl_CreateObjCommand(interp,"thread::sv_array",  ThreadSvArrayObjCmd,
            (ClientData)NULL, NULL);
    Tcl_CreateObjCommand(interp,"thread::sv_unset",  ThreadSvUnsetObjCmd,
            (ClientData)NULL, NULL);

    /*
     * Create array of buckets and initialize each of them
     */

    if (buckets == NULL) {
        Tcl_MutexLock(&masterMutex);
        if (buckets == NULL) {
            buckets = (Bucket *)Tcl_Alloc(sizeof(Bucket) * NUMBUCKETS);
            for (i = 0; i < NUMBUCKETS; ++i) {
                bucketPtr = &buckets[i];
                memset(bucketPtr, 0, sizeof(Bucket));
                bucketPtr->lock = (Tcl_Mutex *)Tcl_Alloc(sizeof(Tcl_Mutex));
                *(bucketPtr->lock) = 0;
                /* Tcl_MutexInitialize(bucketPtr->lock); (in core ?) */
                Tcl_InitHashTable(&bucketPtr->arrays, TCL_STRING_KEYS);
            }
            Tcl_CreateExitHandler((Tcl_ExitProc *)FinalizeSv, NULL);
        }
        Tcl_MutexUnlock(&masterMutex);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * FinalizeSv --
 *
 *    Unset all arrays and reclaim all buckets
 *
 * Results:
 *    None.
 *
 * Side effects
 *    Memory gets reclaimed.
 *
 *----------------------------------------------------------------------
 */
static void
FinalizeSv (clientData)
    ClientData clientData;
{
    Bucket *bucketPtr;
    Tcl_HashEntry *hashPtr;
    Tcl_HashSearch search;
    Array *arrayPtr;
    register int i;
  
    if (buckets != NULL) {
        Tcl_MutexLock(&masterMutex);
        if (buckets != NULL) {
            for (i = 0; i < NUMBUCKETS; ++i) {
                bucketPtr = &buckets[i];
                hashPtr = Tcl_FirstHashEntry(&bucketPtr->arrays, &search);
                while (hashPtr != NULL) {
                  arrayPtr = (Array *)Tcl_GetHashValue(hashPtr);
                  FlushArray(arrayPtr);
                  Tcl_DeleteHashTable(&arrayPtr->vars);
                  Tcl_Free((char *)arrayPtr);
                  Tcl_DeleteHashEntry(hashPtr);
                  hashPtr = Tcl_NextHashEntry(&search);
                }
                if (*(bucketPtr->lock)) {
                    Tcl_MutexFinalize(bucketPtr->lock);
                }
                Tcl_Free((char *)bucketPtr->lock);
                Tcl_DeleteHashTable(&bucketPtr->arrays);
            }
            Tcl_Free((char *)buckets), buckets = NULL;
        }
        Tcl_MutexUnlock(&masterMutex);
    }
}

