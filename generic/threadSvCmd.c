/*
 * This file implements a family of commands for sharing variables
 * between threads.
 *
 * Initial code is taken from nsd/tclvar.c found in AOLserver 3.+
 * distribution and modified to support Tcl 8.0+ command object interface
 * and internal storage in private shared Tcl objects.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: threadSvCmd.c,v 1.21 2002/07/20 10:48:41 vasiljevic Exp $
 * ----------------------------------------------------------------------------
 */

#include "threadSvCmd.h"
#include "threadSvListCmd.h" /* Adds shared variants of list commands */

#ifdef NS_AOLSERVER
# include "aolstub.cpp"
# define HIDE_DOTNAMES       /* tsv::names cmd does not list .<name> arrays */
#endif

/*
 * Compatiblity with the AOLserver config.
 * We have yet to decide how to make this tunable during runtime ...
 */

static Svconf svconf = {8};

/*
 * Number of object containers
 * to allocate in one shot.
 */

#define OBJS_TO_ALLOC_EACH_TIME 100

/*
 * Reference to Tcl object types used in object-copy code.
 * Those are referenced read-only, thus no mutex protection.
 */

static Tcl_ObjType* booleanObjTypePtr;
static Tcl_ObjType* byteArrayObjTypePtr;
static Tcl_ObjType* doubleObjTypePtr;
static Tcl_ObjType* intObjTypePtr;
static Tcl_ObjType* stringObjTypePtr;

/*
 * In order to be fully stub enabled, a small
 * hack is needed to query the tclEmptyStringRep
 * global symbol defined by Tcl. See Sv_Init.
 */

char *Sv_tclEmptyStringRep = NULL;

/*
 * Global variables used within this file.
 */

static Bucket*    buckets;      /* Array of buckets. */
static Tcl_Mutex  bucketsMutex; /* Protects the array of buckets */

static SvCmdInfo* svCmdInfo;    /* Linked list of registered commands */
static RegType*   regType;      /* Linked list of registered obj types */
static Tcl_Mutex  typesMutex;   /* Protects inserts into above lists */

/*
 * The standard commands found in AOLserver nsv_* interface. 
 * For sharp-eye readers: the implementaion of the "lappend" command
 * is moved to new list-command package, since it realy belongs there.
 */

static Tcl_ObjCmdProc SvObjObjCmd;
static Tcl_ObjCmdProc SvAppendObjCmd;
static Tcl_ObjCmdProc SvIncrObjCmd;
static Tcl_ObjCmdProc SvSetObjCmd;
static Tcl_ObjCmdProc SvExistsObjCmd;
static Tcl_ObjCmdProc SvGetObjCmd;
static Tcl_ObjCmdProc SvArrayObjCmd;
static Tcl_ObjCmdProc SvUnsetObjCmd;
static Tcl_ObjCmdProc SvNamesObjCmd;

/*
 * New commands added to 
 * standard set of nsv_*
 */

static Tcl_ObjCmdProc SvPopObjCmd;
static Tcl_ObjCmdProc SvMoveObjCmd;

/*
 * Forward declarations for functions to
 * manage buckets, arrays and shared objects.
 */

static Container* CreateContainer(Array*, Tcl_HashEntry*, Tcl_Obj*);
static Container* LocateContainer(Array*, Tcl_Obj*, int);
static void       DeleteContainer(Container*);
static Array*     CreateArray(Bucket*, char*);
static Array*     LockArray(Tcl_Interp*, char*, int);
static void       FlushArray(Array*);
static void       DeleteArray(Array*);
static void       SvAllocateContainers(Bucket*);
static void       SvFinalizeContainers(Bucket*);
static void       SvRegisterStdCommands(void);
static void       SvFinalize(ClientData);
static Tcl_ObjCmdProc SvObjDispatchObjCmd;

/*
 *-----------------------------------------------------------------------------
 *
 * Sv_RegisterCommand --
 *
 *      Utility to register commands to be loaded at module start.
 *
 * Results:
 *      None.
 *
 * Side effects;
 *      New command will be added to a linked list of registered commands.
 *
 *-----------------------------------------------------------------------------
 */

void
Sv_RegisterCommand(cmdName, objProc, delProc, clientData)
    char *cmdName;                      /* Name of command to register */
    Tcl_ObjCmdProc *objProc;            /* Object-based command procedure */
    Tcl_CmdDeleteProc *delProc;         /* Command delete procedure */
    ClientData clientData;              /* Private data ptr to pass to cmd */
{
    int len = strlen(cmdName) + strlen(N);
    SvCmdInfo *newCmd = (SvCmdInfo*)Tcl_Alloc(sizeof(SvCmdInfo) + len + 1);

    /*
     * Setup new command structure
     */

    newCmd->cmdName = (char*)((char*)newCmd + sizeof(SvCmdInfo));

    newCmd->objProcPtr = objProc;
    newCmd->delProcPtr = delProc;
    newCmd->nextPtr    = NULL;
    newCmd->clientData = clientData;

    /*
     * Rewrite command name. This is needed so we can
     * easily turn-on the compatiblity with AOLserver
     * command names.
     */

    strcpy(newCmd->cmdName, N);
    strcat(newCmd->cmdName, cmdName);
    newCmd->name = newCmd->cmdName + strlen(N);

    /*
     * Plug-in to shared list of commands.
     */

    Tcl_MutexLock(&typesMutex);
    if (svCmdInfo == NULL) {
        svCmdInfo = newCmd;
    } else {
        newCmd->nextPtr = svCmdInfo;
        svCmdInfo = newCmd;
    }
    Tcl_MutexUnlock(&typesMutex);

    return;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Sv_RegisterObjType --
 *
 *      Registers custom object duplicator function for a specific
 *      object type. Registered function will be called by the
 *      private object creation routine every time an object is
 *      plugged out or in the shared array. This way we assure that
 *      Tcl objects do not get shared between threads.
 *
 * Results:
 *      None.
 *
 * Side effects;
 *      Memory gets allocated.
 *
 *-----------------------------------------------------------------------------
 */

void
Sv_RegisterObjType(typePtr, dupProc)
    Tcl_ObjType *typePtr;               /* Type of object to register */
    Tcl_DupInternalRepProc *dupProc;    /* Custom object duplicator */
{
    RegType *newType = (RegType*)Tcl_Alloc(sizeof(RegType));

    /*
     * Setup new type structure
     */

    newType->typePtr = typePtr;
    newType->dupIntRepProc = dupProc;

    /*
     * Plug-in to shared list
     */

    Tcl_MutexLock(&typesMutex);
    if (regType == NULL) {
        regType = newType;
        regType->nextPtr = NULL;
    } else {
        newType->nextPtr = regType;
        regType = newType;
    }
    Tcl_MutexUnlock(&typesMutex);

    return;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Sv_Container --
 *
 *      This is the workhorse of the module. It returns the container
 *      with the shared Tcl object. It also locks the container, so 
 *      when finished with operation on the Tcl object, one has to unlock
 *      the container with Sv_Unlock().
 *      If instructed, this command might also create new container with 
 *      empty Tcl object.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      New container might be created.
 *
 *-----------------------------------------------------------------------------
 */

int
Sv_Container(interp, objc, objv, retObj, offset, flags)
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
    Container **retObj;                 /* OUT: shared object container */
    int *offset;                        /* Shift in argument list */
    int flags;                          /* Options for locking shared array */
{
    char *array, *key;

    if (*retObj == NULL) {
        Array *arrayPtr = NULL;

        /*
         * Parse mandatory arguments: <cmd> array key
         */

        if (objc < 3) {
            Tcl_WrongNumArgs(interp, 1, objv, "array key ?args?");
            return TCL_ERROR;
        }

        array = Tcl_GetString(objv[1]);
        key   = Tcl_GetString(objv[2]);

        *offset = 3; /* Consumed three arguments: cmd, array, key */

        /*
         * Lock the shared array and locate the shared object
         */

        arrayPtr = LockArray(interp, array, flags);
        if (arrayPtr == NULL) {
            return TCL_BREAK;
        }
        *retObj = LocateContainer(arrayPtr, objv[2], flags);
        if (*retObj == NULL) {
            UnlockArray(arrayPtr);
            Tcl_AppendResult(interp, "no key ", array, "(", key, ")", NULL);
            return TCL_BREAK;
        }
    } else {
        Tcl_HashTable *handles = &((*retObj)->bucketPtr->handles);

        /*
         * Lock the shared array only, since object is known
         */

        Sv_Lock(*retObj);
        if (Tcl_FindHashEntry(handles, (char*)(*retObj)) == NULL) {
            Sv_Unlock(*retObj);
            Tcl_AppendResult(interp, "key has been deleted", NULL);
            return TCL_BREAK;
        }
        *offset = 2; /* Consumed two arguments: object, cmd */
    }

    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * LocateContainer --
 *
 *      Finds a variable within an array and returns it's container.
 *
 * Results:
 *      Pointer to variable object.
 *
 * Side effects;
 *      New variable may be created.
 *
 *-----------------------------------------------------------------------------
 */

static Container *
LocateContainer(arrayPtr, key, flags)
    Array *arrayPtr;
    Tcl_Obj *key;
    int flags;
{
    int new;
    char *varName = Tcl_GetString(key);
    Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&arrayPtr->vars, varName);

    if (hPtr == NULL) {
        if (!(flags & FLAGS_CREATEVAR)) {
            return NULL;
        }
        hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, varName, &new);
        Tcl_SetHashValue(hPtr, CreateContainer(arrayPtr, hPtr, Tcl_NewObj()));
    }

    return (Container*)Tcl_GetHashValue(hPtr);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CreateContainer --
 *
 *      Creates new shared container holding Tcl object to be stored
 *      in the shared array.
 *
 * Results:
 *      The container pointer.
 *
 * Side effects;
 *      Memory gets allocated.
 *
 *-----------------------------------------------------------------------------
 */

static Container *
CreateContainer(arrayPtr, entryPtr, tclObj)
    Array *arrayPtr;
    Tcl_HashEntry *entryPtr;
    Tcl_Obj *tclObj;
{
    Container *svObj;

    if (arrayPtr->bucketPtr->freeCt == NULL) {
        SvAllocateContainers(arrayPtr->bucketPtr);
    }

    svObj = arrayPtr->bucketPtr->freeCt;
    arrayPtr->bucketPtr->freeCt = svObj->nextPtr;

    svObj->arrayPtr  = arrayPtr;
    svObj->bucketPtr = arrayPtr->bucketPtr;
    svObj->tclObj    = tclObj;
    svObj->entryPtr  = entryPtr;
    svObj->handlePtr = NULL;

    if (svObj->tclObj) {
        Tcl_IncrRefCount(svObj->tclObj);
    }

    return svObj;
}

/*
 *-----------------------------------------------------------------------------
 *
 * DeleteContainer --
 *
 *      Destroys the container and the Tcl object within it.
 *
 * Results:
 *      None.
 *
 * Side effects;
 *      Memory gets reclaimed.
 *
 *-----------------------------------------------------------------------------
 */

static void
DeleteContainer(svObj)
    Container *svObj;
{
    if (svObj->tclObj) {
        Tcl_DecrRefCount(svObj->tclObj);
    }
    if (svObj->handlePtr) {
        Tcl_DeleteHashEntry(svObj->handlePtr);
    }
    if (svObj->entryPtr) {
        Tcl_DeleteHashEntry(svObj->entryPtr);
    }

    svObj->arrayPtr  = NULL;
    svObj->entryPtr  = NULL;
    svObj->handlePtr = NULL;
    svObj->tclObj    = NULL;

    svObj->nextPtr = svObj->bucketPtr->freeCt;
    svObj->bucketPtr->freeCt = svObj;
}

/*
 *-----------------------------------------------------------------------------
 *
 * LockArray --
 *
 *      Find (or create) the array structure for shared array and lock it.
 *      Array structure must be later unlocked with UnlockArray.
 *
 * Results:
 *      TCL_OK or TCL_ERROR if no such array.
 *
 * Side effects;
 *      Sets *arrayPtrPtr with Array pointer or leave error in given interp.
 *
 *-----------------------------------------------------------------------------
 */

static Array *
LockArray(interp, array, flags)
    Tcl_Interp *interp;                 /* Interpreter to leave result. */
    char *array;                        /* Name of array to lock */
    int flags;                          /* FLAGS_CREATEARRAY/FLAGS_NOERRMSG*/
{
    register char *p;
    register unsigned int result;
    register int i;
    Bucket *bucketPtr;
    Array *arrayPtr;

    /*
     * Compute a hash to map an array to a bucket.
     */

    p = array;
    result = 0;
    while (*p++) {
        i = *p;
        result += (result << 3) + i;
    }
    i = result % svconf.numbuckets;
    bucketPtr = &buckets[i];

    /*
     * Lock the bucket and find the array, or create a new one.
     * The bucket will be left locked on success.
     */

    Tcl_MutexLock(&bucketPtr->lock); /* Note: no matching unlock below ! */
    if (flags & FLAGS_CREATEARRAY) {
        arrayPtr = CreateArray(bucketPtr, array);
    } else {
        Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&bucketPtr->arrays, array);
        if (hPtr == NULL) {
            Tcl_MutexUnlock(&bucketPtr->lock);
            if (!(flags & FLAGS_NOERRMSG)) {
                Tcl_AppendResult(interp, "\"", array,
                                 "\" is not a thread shared array", NULL);
            }
            return NULL;
        }
        arrayPtr = (Array*)Tcl_GetHashValue(hPtr);
    }

    return arrayPtr;
}
/*
 *-----------------------------------------------------------------------------
 *
 * FlushArray --
 *
 *      Unset all keys in an array.
 *
 * Results:
 *      None.
 *
 * Side effects
 *      Array is cleaned but it's variable hash-hable still lives.
 *
 *-----------------------------------------------------------------------------
 */

static void
FlushArray(arrayPtr)
    Array *arrayPtr;                    /* Name of array to flush */
{
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    
    for (hPtr = Tcl_FirstHashEntry(&arrayPtr->vars, &search);
         hPtr != NULL;hPtr = Tcl_NextHashEntry(&search)) {
        DeleteContainer((Container*)Tcl_GetHashValue(hPtr));
    }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CreateArray --
 *
 *      Creates new shared array instance.
 *
 * Results:
 *      Pointer to the newly created array
 *
 * Side effects;
 *      Memory gets allocated
 *
 *-----------------------------------------------------------------------------
 */

static Array *
CreateArray(bucketPtr, arrayName)
    Bucket *bucketPtr;
    char *arrayName;
{
    int new;
    Array *arrayPtr;
    Tcl_HashEntry *hPtr;

    hPtr = Tcl_CreateHashEntry(&bucketPtr->arrays, arrayName, &new);
    if (!new) {
        return (Array*)Tcl_GetHashValue(hPtr);
    }

    arrayPtr = (Array*)Tcl_Alloc(sizeof(Array));
    arrayPtr->bucketPtr = bucketPtr;
    arrayPtr->entryPtr  = hPtr;

    Tcl_InitHashTable(&arrayPtr->vars, TCL_STRING_KEYS);
    Tcl_SetHashValue(hPtr, arrayPtr);

    return arrayPtr;
}

/*
 *-----------------------------------------------------------------------------
 *
 * DeleteArray --
 *
 *      Deletes the shared array.
 *
 * Results:
 *      None..
 *
 * Side effects;
 *      Memory gets reclaimed.
 *
 *-----------------------------------------------------------------------------
 */

static void
DeleteArray(arrayPtr)
    Array *arrayPtr;
{
    if (arrayPtr->entryPtr) {
        Tcl_DeleteHashEntry(arrayPtr->entryPtr);
    }
    FlushArray(arrayPtr);
    Tcl_DeleteHashTable(&arrayPtr->vars);

    UnlockArray(arrayPtr);
    Tcl_Free((char*)arrayPtr);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvAllocateContainers --
 *
 *      Any similarity with the Tcl AllocateFreeObj function is purely
 *      coincidental... Just joking; this is (almost) 100% copy of it! :-)
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Allocates memory for many containers at the same time
 *
 *-----------------------------------------------------------------------------
 */

static void
SvAllocateContainers(bucketPtr)
    Bucket *bucketPtr;
{
    Container tmp[2];
    size_t objSizePlusPadding = ((int)(&(tmp[1]))-(int)(&(tmp[0])));
    size_t bytesToAlloc = (OBJS_TO_ALLOC_EACH_TIME * objSizePlusPadding);
    char *basePtr;
    register Container *prevPtr = NULL, *objPtr = NULL;
    register int i;

    basePtr = (char*)Tcl_Alloc(bytesToAlloc);
    memset(basePtr, 0, bytesToAlloc);

    objPtr = (Container*)basePtr;
    objPtr->chunkAddr = basePtr; /* Mark chunk address for reclaim */

    for (i = 0; i < OBJS_TO_ALLOC_EACH_TIME; i++) {
        objPtr->nextPtr = prevPtr;
        prevPtr = objPtr;
        objPtr = (Container*)(((char*)objPtr) + objSizePlusPadding);
    }
    bucketPtr->freeCt = prevPtr;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvFinalizeContainers --
 *
 *    Reclaim memory for free object containers per bucket.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Memory gets reclaimed
 *
 *-----------------------------------------------------------------------------
 */

static void
SvFinalizeContainers(bucketPtr)
    Bucket *bucketPtr;
{
   Container *tmpPtr, *objPtr = bucketPtr->freeCt;

    while (objPtr) {
        if (objPtr->chunkAddr == (char*)objPtr) {
            tmpPtr = objPtr->nextPtr;
            Tcl_Free((char*)objPtr);
            objPtr = tmpPtr;
        } else {
            objPtr = objPtr->nextPtr;
        }
    }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Sv_DuplicateObj --
 *
 *      Create and return a new object that is (mostly!) a duplicate of the
 *      argument object. We take care that the duplicate object is either
 *      a proper object copy, i.e. w/o hidden references to original object
 *      elements or a plain string object, i.e one w/o internal rep.
 *
 *      Decision about wether to produce a real duplicate or a string object
 *      is done as follows:
 *
 *      1) Scalar Tcl object types are properly copied by default;
 *         these include: boolean, int double, string and byteArray types.
 *      2) Object registered with Sv_RegisterObjType are duplicated
 *         using custom duplicator function which is guaranteed to
 *         produce a proper deep copy of the object in question.
 *      3) All other object types are stringified; these include
 *         miscelaneous Tcl objects (cmdName, nsName, bytecode, etc, etc)
 *         and all user-defined objects. Latter may or may not have the
 *         "sv"-savvy obj dup function, but to be sure, we do not use it.
 *
 * Results:
 *      The return value is a pointer to a newly created Tcl_Obj. This
 *      object has reference count 0 and the same type, if any, as the
 *      source object objPtr. So:
 *
 *      1) If the source object has a valid string rep, we copy it;
 *         otherwise, the duplicate's string rep is set to NULL to mark
 *         it invalid.
 *      2) If the source object has an internal representation (i.e. its
 *         typePtr is non-NULL), the new object's internal rep is set to
 *         a copy; otherwise the new internal rep is marked invalid.
 *         (see discussion above)
 *
 * Side effects:
 *      Some object may, when copied, loose their type, i.e. will become
 *      just plain string objects.
 *
 *-----------------------------------------------------------------------------
 */

Tcl_Obj *
Sv_DuplicateObj(objPtr)
    register Tcl_Obj *objPtr;        /* The object to duplicate. */
{
    register Tcl_Obj *dupPtr = Tcl_NewObj();

    /*
     * Handle the internal rep
     */

    if (objPtr->typePtr != NULL) {
        if (objPtr->typePtr->dupIntRepProc == NULL) {
            dupPtr->internalRep = objPtr->internalRep;
            dupPtr->typePtr = objPtr->typePtr;
        } else {
            if (   objPtr->typePtr == booleanObjTypePtr    \
                || objPtr->typePtr == byteArrayObjTypePtr  \
                || objPtr->typePtr == doubleObjTypePtr     \
                || objPtr->typePtr == intObjTypePtr        \
                || objPtr->typePtr == stringObjTypePtr) {
               /*
                * Cover all standard obj types
                */
              (*objPtr->typePtr->dupIntRepProc)(objPtr, dupPtr);
            } else {
                register RegType *regPtr;
                int duplicated = 0;
               /*
                * Cover special registered types. Assume not
                * very many of those, so this sequential walk
                * should be fast enough.
                */
                for (regPtr = regType; regPtr->nextPtr;
                     regPtr = regPtr->nextPtr) {
                    if (objPtr->typePtr == regPtr->typePtr) {
                        (*regPtr->dupIntRepProc)(objPtr, dupPtr);
                        duplicated = 1;
                        break;
                    }
                }
                /*
                 * If not able to duplicate, assure valid string
                 * representation is available at least.
                 */
                if (duplicated == 0) {
                    if (objPtr->typePtr->updateStringProc != NULL) {
                        (*objPtr->typePtr->updateStringProc)(objPtr);
                    }
                }
            }
        }
    }

    /*
     * Handle the string rep.
     */

    if (objPtr->bytes == NULL) {
        dupPtr->bytes = NULL;
    } else if (objPtr->bytes != Sv_tclEmptyStringRep) {
        /* A copy of TclInitStringRep macro */
        dupPtr->bytes = (char*)Tcl_Alloc((unsigned)objPtr->length + 1);
        if (objPtr->length > 0) {
            memcpy((void*)dupPtr->bytes,(void*)objPtr->bytes,
                   (unsigned)objPtr->length);
        }
        dupPtr->length = objPtr->length;
        dupPtr->bytes[objPtr->length] = '\0';        
    } else if (dupPtr->typePtr && dupPtr->typePtr->updateStringProc) {
        Tcl_InvalidateStringRep(dupPtr);
    }

    return dupPtr;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvObjDispatchObjCmd --
 *
 *      The method command for dispatching sub-commands of the shared
 *      object.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Depends on the dispatched command
 *
 *-----------------------------------------------------------------------------
 */

static int
SvObjDispatchObjCmd(arg, interp, objc, objv)
    ClientData arg;                     /* Just passed to the command. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    char *cmdName;
    SvCmdInfo *cmdPtr;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "args");
        return TCL_ERROR;
    }

    cmdName = Tcl_GetString(objv[1]);

    /*
     * Do simple linear search. We may later replace this list
     * with the hash table to gain speed. Currently, the list
     * of registered commands is so small, so this will work
     * fast enough.
     */

    for (cmdPtr = svCmdInfo; cmdPtr; cmdPtr = cmdPtr->nextPtr) {
        if (!strcmp(cmdPtr->name, cmdName)) {
            return (*cmdPtr->objProcPtr)(arg, interp, objc, objv);
        }
    }

    Tcl_AppendResult(interp, "invalid command name \"", cmdName, "\"", NULL);
    return TCL_ERROR;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvObjObjCmd --
 *
 *      Creates the object command for a shared object.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      New Tcl command gets created.
 *
 *-----------------------------------------------------------------------------
 */

static int
SvObjObjCmd(dummy, interp, objc, objv)
    ClientData dummy;                   /* Not used. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    int new, off, ret, flg;
    char buf[128];
    Tcl_Obj *val = NULL;
    Container *svObj = NULL;
    Tcl_HashTable *handles;

    /*
     * Syntax: tsv::object array key ?var?
     */

    ret = Sv_Container(interp, objc, objv, &svObj, &off, 0);
    switch (ret) {
    case TCL_BREAK:
        if ((objc - off)) {
            val = objv[off];
        }
        Tcl_ResetResult(interp);
        flg = FLAGS_CREATEARRAY | FLAGS_CREATEVAR;
        ret = Sv_Container(interp, objc, objv, &svObj, &off, flg);
        if (ret != TCL_OK) {
            return TCL_ERROR;
        }
        Tcl_DecrRefCount(svObj->tclObj);
        svObj->tclObj = Sv_DuplicateObj(val?val:Tcl_NewObj());
        Tcl_IncrRefCount(svObj->tclObj);
        break;
    case TCL_ERROR:
        return TCL_ERROR;
    }

    handles = &svObj->arrayPtr->bucketPtr->handles;
    if (svObj->handlePtr == NULL) {
        svObj->handlePtr = Tcl_CreateHashEntry(handles, (char*)svObj, &new);
    }

    /*
     * Format the command name
     */

    sprintf(buf, "::%p", (int*)svObj);
    Tcl_CreateObjCommand(interp, buf, SvObjDispatchObjCmd, (int*)svObj, NULL);
    Tcl_ResetResult(interp);
    Tcl_SetStringObj(Tcl_GetObjResult(interp), buf, -1);

    Sv_Unlock(svObj);
    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvArrayObjCmd --
 *
 *      This procedure is invoked to process the "tsv::array" command.
 *      See the user documentation for details on what it does.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *-----------------------------------------------------------------------------
 */

static int
SvArrayObjCmd(arg, interp, objc, objv)
    ClientData arg;                     /* Pointer to object container. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    int i, argx = 0, lobjc = 0, index, ret = TCL_OK;
    char *arrayName = NULL;
    Array *arrayPtr = NULL;
    Tcl_Obj **lobjv = NULL;
    Container *svObj, *elObj = NULL;

    static CONST84 char *opts[] = {
        "set", "reset", "get", "names", "size", "exists", NULL};
    enum options {
        ASET,  ARESET,  AGET,  ANAMES,  ASIZE,  AEXISTS
    };

    svObj = (Container*)arg;

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "option array");
        return TCL_ERROR;
    }

    arrayName = Tcl_GetString(objv[2]);
    arrayPtr  = LockArray(interp, arrayName, FLAGS_NOERRMSG);

    if (objc > 3) {
        argx = 3;
    }

    if (Tcl_GetIndexFromObj(interp, objv[1], opts, 
            "option", 0, &index) != TCL_OK) {
        ret = TCL_ERROR;
        goto cmdExit;
    } else if (index == AEXISTS) {
        Tcl_ResetResult(interp);
        Tcl_SetBooleanObj(Tcl_GetObjResult(interp), arrayPtr ? 1: 0);
    } else if (index == ASIZE) {
        Tcl_ResetResult(interp);
        if (arrayPtr == NULL) {
            Tcl_SetIntObj(Tcl_GetObjResult(interp), 0);
        } else {
            Tcl_SetLongObj(Tcl_GetObjResult(interp),arrayPtr->vars.numEntries);
        }
    } else if (index == ASET || index == ARESET) {
        if (argx == (objc - 1)) {
            if (argx && Tcl_ListObjGetElements(interp, objv[argx], &lobjc,
                    &lobjv) != TCL_OK) {
                ret = TCL_ERROR;
                goto cmdExit;
            }
        } else {
            lobjc = objc - 3;
            lobjv = (Tcl_Obj **) &objv[3];
        }
        if (lobjc & 1) {
            Tcl_AppendResult(interp, "list must have an even number"
                    " of elements", NULL);
            ret = TCL_ERROR;
            goto cmdExit;
        }
        if (arrayPtr == NULL) {
            arrayPtr = LockArray(interp, arrayName, FLAGS_CREATEARRAY);
        }
        if (index == ARESET) {
            FlushArray(arrayPtr);
        }
        for (i = 0; i < lobjc; i += 2) {
            elObj = LocateContainer(arrayPtr, lobjv[i], FLAGS_CREATEVAR);
            Tcl_DecrRefCount(elObj->tclObj);
            elObj->tclObj = Sv_DuplicateObj(lobjv[i+1]);
            Tcl_IncrRefCount(elObj->tclObj);
        }
    } else if (index == AGET || index == ANAMES) {
        if (arrayPtr) {
            Tcl_HashSearch search;
            Tcl_Obj *resObj = Tcl_NewListObj(0, NULL);
            char *pattern = (argx == 0) ? NULL : Tcl_GetString(objv[argx]);
            Tcl_HashEntry *hPtr = Tcl_FirstHashEntry(&arrayPtr->vars, &search);
            while (hPtr) {
                char *key = Tcl_GetHashKey(&arrayPtr->vars, hPtr);
                if (pattern == NULL || Tcl_StringMatch(key, pattern)) {
                    Tcl_ListObjAppendElement(interp, resObj,
                            Tcl_NewStringObj(key, -1));
                    if (index == AGET) {
                        elObj = (Container*)Tcl_GetHashValue(hPtr);
                        Tcl_ListObjAppendElement(interp, resObj,
                                Sv_DuplicateObj(elObj->tclObj));
                    }
                }
                hPtr = Tcl_NextHashEntry(&search);
            }
            Tcl_SetObjResult(interp, resObj);
        }
    }

 cmdExit:
    if (arrayPtr) {
        UnlockArray(arrayPtr);
    }

    return ret;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvUnsetObjCmd --
 *
 *      This procedure is invoked to process the "tsv::unset" command.
 *      See the user documentation for details on what it does.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *-----------------------------------------------------------------------------
 */

static int
SvUnsetObjCmd(dummy, interp, objc, objv)
    ClientData dummy;                   /* Not used. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    int ii;
    char *arrayName;
    Array *arrayPtr;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "array ?key ...?");
        return TCL_ERROR;
    }

    arrayName = Tcl_GetString(objv[1]);
    arrayPtr  = LockArray(interp, arrayName, 0);

    if (arrayPtr == NULL) {
        return TCL_ERROR;
    }
    if (objc == 2) {
        DeleteArray(arrayPtr); /* Unlocks it as well */
    } else {
        for (ii = 2; ii < objc; ii++) {
            char *key = Tcl_GetString(objv[ii]);
            Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&arrayPtr->vars, key);
            if (hPtr) {
                DeleteContainer((Container*)Tcl_GetHashValue(hPtr));
            } else {
                UnlockArray(arrayPtr);
                Tcl_AppendResult(interp,"no key ",arrayName,"(",key,")",NULL);
                return TCL_ERROR;
            }
        }
        UnlockArray(arrayPtr);
    }

    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvNamesObjCmd --
 *
 *      This procedure is invoked to process the "tsv::names" command.
 *      See the user documentation for details on what it does.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *-----------------------------------------------------------------------------
 */

static int
SvNamesObjCmd(dummy, interp, objc, objv)
    ClientData dummy;                   /* Not used. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    int i, len;
    char *pattern = NULL;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    Tcl_Obj *resObj;

    if (objc > 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?pattern?");
        return TCL_ERROR;
    }
    if (objc == 2) {
        pattern = Tcl_GetStringFromObj(objv[1], &len);
    }

    resObj = Tcl_NewListObj(0, NULL);

    for (i = 0; i < svconf.numbuckets; i++) {
        Bucket *bucketPtr = &buckets[i];
        Tcl_MutexLock(&bucketPtr->lock);
        hPtr = Tcl_FirstHashEntry(&bucketPtr->arrays, &search);
        while (hPtr) {
            char *key = Tcl_GetHashKey(&bucketPtr->arrays, hPtr);
#ifdef HIDE_DOTNAMES
            if (*key != '.' /* Hide .<name> arrays */ &&
#else
            if (1 &&
#endif
                (pattern == NULL || Tcl_StringMatch(key, pattern))) {
                Tcl_ListObjAppendElement(interp, resObj,
                        Tcl_NewStringObj(key, -1));
            }
            hPtr = Tcl_NextHashEntry(&search);
        }
        Tcl_MutexUnlock(&bucketPtr->lock);
    }

    Tcl_SetObjResult(interp, resObj);

    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvGetObjCmd --
 *
 *      This procedure is invoked to process "tsv::get" command.
 *      See the user documentation for details on what it does.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *-----------------------------------------------------------------------------
 */

static int
SvGetObjCmd(arg, interp, objc, objv)
    ClientData arg;                     /* Pointer to object container. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    int off, ret;
    Tcl_Obj *res;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::get array key ?var?
     *          $object get ?var?
     */

    ret = Sv_Container(interp, objc, objv, &svObj, &off, 0);
    switch (ret) {
    case TCL_BREAK:
        if ((objc - off) == 0) {
            return TCL_ERROR;
        } else {
            Tcl_ResetResult(interp);
            Tcl_SetIntObj(Tcl_GetObjResult(interp), 0);
            return TCL_OK;
        }
    case TCL_ERROR:
        return TCL_ERROR;
    }

    res = Sv_DuplicateObj(svObj->tclObj);

    if ((objc - off) == 0) {
        Tcl_SetObjResult(interp, res);
    } else {
        if (Tcl_ObjSetVar2(interp, objv[off], NULL, res, 0) == NULL) {
            Tcl_DecrRefCount(res);
            goto cmd_err;
        }
        Tcl_ResetResult(interp);
        Tcl_SetIntObj(Tcl_GetObjResult(interp), 1);
    }

    Sv_Unlock(svObj);
    return TCL_OK;

 cmd_err:
    Sv_Unlock(svObj);
    return TCL_ERROR;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvExistsObjCmd --
 *
 *      This procedure is invoked to process "tsv::exists" command.
 *      See the user documentation for details on what it does.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *-----------------------------------------------------------------------------
 */

static int
SvExistsObjCmd(arg, interp, objc, objv)
    ClientData arg;                     /* Pointer to object container. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    int off, ret;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::exists array key
     *          $object exists
     */

    ret = Sv_Container(interp, objc, objv, &svObj, &off, 0);
    switch (ret) {
    case TCL_BREAK:
        Tcl_ResetResult(interp);
        Tcl_SetBooleanObj(Tcl_GetObjResult(interp), 0); 
        return TCL_OK;
    case TCL_ERROR:
        return TCL_ERROR;
    }

    Sv_Unlock(svObj);
    Tcl_ResetResult(interp);
    Tcl_SetBooleanObj(Tcl_GetObjResult(interp), 1);

    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvSetObjCmd --
 *
 *      This procedure is invoked to process the "tsv::set" command.
 *      See the user documentation for details on what it does.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *-----------------------------------------------------------------------------
 */

static int
SvSetObjCmd(arg, interp, objc, objv)
    ClientData arg;                     /* Pointer to object container */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    int ret, off, flg;
    Tcl_Obj *val;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::set array key ?value?
     *          $object set ?value?
     */

    ret = Sv_Container(interp, objc, objv, &svObj, &off, 0);
    switch (ret) {
    case TCL_BREAK:
        if ((objc - off) == 0) {
            return TCL_ERROR;
        } else {
            Tcl_ResetResult(interp);
            flg = FLAGS_CREATEARRAY | FLAGS_CREATEVAR;
            ret = Sv_Container(interp, objc, objv, &svObj, &off, flg);
            if (ret != TCL_OK) {
                return TCL_ERROR;
            }
        }
        break;
    case TCL_ERROR:
        return TCL_ERROR;
    }
    if ((objc - off)) {
        val = objv[off];
        Tcl_DecrRefCount(svObj->tclObj);
        svObj->tclObj = Sv_DuplicateObj(val);
        Tcl_IncrRefCount(svObj->tclObj);
    } else {
        val = Sv_DuplicateObj(svObj->tclObj);
    }

    Sv_Unlock(svObj);
    Tcl_SetObjResult(interp, val);

    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvIncrObjCmd --
 *
 *      This procedure is invoked to process the "tsv::incr" command.
 *      See the user documentation for details on what it does.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *-----------------------------------------------------------------------------
 */

static int
SvIncrObjCmd(arg, interp, objc, objv)
    ClientData arg;                     /* Pointer to object container */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    int off, ret;
    long incrValue = 1, currValue;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::incr array key ?increment?
     *          $object incr ?increment?
     */

    ret = Sv_Container(interp, objc, objv, &svObj, &off, 0);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }
    if ((objc - off)) {
        ret = Tcl_GetLongFromObj(interp, objv[off], &incrValue);
        if (ret != TCL_OK) {
            goto cmd_err;
        }
    }
    ret = Tcl_GetLongFromObj(interp, svObj->tclObj, &currValue);
    if (ret != TCL_OK) {
        goto cmd_err;
    }

    incrValue += currValue;
    Tcl_SetLongObj(svObj->tclObj, incrValue);
    Tcl_ResetResult(interp);
    Tcl_SetLongObj(Tcl_GetObjResult(interp), incrValue);

    Sv_Unlock(svObj);
    return TCL_OK;

 cmd_err:
    Sv_Unlock(svObj);
    return TCL_ERROR;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvAppendObjCmd --
 *
 *      This procedure is invoked to process the "tsv::append" command.
 *      See the user documentation for details on what it does.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *-----------------------------------------------------------------------------
 */

static int
SvAppendObjCmd(arg, interp, objc, objv)
    ClientData arg;                     /* Pointer to object container */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    int i, off, flg, ret;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::append array key value ?value ...?
     *          $object append value ?value ...?
     */

    flg = FLAGS_CREATEARRAY | FLAGS_CREATEVAR;
    ret = Sv_Container(interp, objc, objv, &svObj, &off, flg);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }
    if ((objc - off) < 1) {
        Tcl_WrongNumArgs(interp, off, objv, "value ?value ...?");
        goto cmd_err;
    }
    for (i = off; i < objc; ++i) {
        Tcl_AppendObjToObj(svObj->tclObj, Sv_DuplicateObj(objv[i]));
    }

    Tcl_SetObjResult(interp, Sv_DuplicateObj(svObj->tclObj));

    Sv_Unlock(svObj);
    return TCL_OK;

 cmd_err:
    Sv_Unlock(svObj);
    return TCL_ERROR;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvPopObjCmd --
 *
 *      This procedure is invoked to process "tsv::pop" command.
 *      See the user documentation for details on what it does.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *-----------------------------------------------------------------------------
 */

static int
SvPopObjCmd(arg, interp, objc, objv)
    ClientData arg;                     /* Pointer to object container */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    int ret, off;
    Tcl_Obj *retObj;
    Array *arrayPtr = NULL;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::pop array key
     *          $object pop
     *
     * Note: the object command will run into error next time !
     */

    ret = Sv_Container(interp, objc, objv, &svObj, &off, 0);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }

    arrayPtr = svObj->arrayPtr;

    retObj = svObj->tclObj;
    svObj->tclObj = NULL;

    DeleteContainer(svObj);

    Tcl_SetObjResult(interp, retObj);
    Tcl_DecrRefCount(retObj);

    UnlockArray(arrayPtr);

    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvMoveObjCmd --
 *
 *      This procedure is invoked to process the "tsv::move" command.
 *      See the user documentation for details on what it does.
 *
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *-----------------------------------------------------------------------------
 */

static int
SvMoveObjCmd(arg, interp, objc, objv)
    ClientData arg;                     /* Pointer to object container. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    int ret, off, new;
    char *toKey;
    Tcl_HashEntry *hPtr;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::move array key to
     *          $object move to
     */

    ret = Sv_Container(interp, objc, objv, &svObj, &off, 0);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }

    toKey = Tcl_GetString(objv[off]);
    hPtr = Tcl_CreateHashEntry(&svObj->arrayPtr->vars, toKey, &new);

    if (!new) {
        Tcl_AppendResult(interp, "key \"", toKey, "\" exists", NULL);
        goto cmd_err;
    }
    if (svObj->entryPtr) {
        Tcl_DeleteHashEntry(svObj->entryPtr);
    }

    svObj->entryPtr = hPtr;
    Tcl_SetHashValue(hPtr, svObj);

    Sv_Unlock(svObj);
    return TCL_OK;

 cmd_err:
    Sv_Unlock(svObj);
    return TCL_ERROR;;

}

/*
 *-----------------------------------------------------------------------------
 *
 * Sv_RegisterStdCommands --
 *
 *      Register standard shared variable commands
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Memory gets allocated
 *
 *-----------------------------------------------------------------------------
 */

static void
SvRegisterStdCommands(void)
{
    Sv_RegisterCommand("var",    SvObjObjCmd,    NULL, NULL); /* compat */
    Sv_RegisterCommand("object", SvObjObjCmd,    NULL, NULL);
    Sv_RegisterCommand("set",    SvSetObjCmd,    NULL, NULL);
    Sv_RegisterCommand("unset",  SvUnsetObjCmd,  NULL, NULL);
    Sv_RegisterCommand("get",    SvGetObjCmd,    NULL, NULL);
    Sv_RegisterCommand("incr",   SvIncrObjCmd,   NULL, NULL);
    Sv_RegisterCommand("exists", SvExistsObjCmd, NULL, NULL);
    Sv_RegisterCommand("append", SvAppendObjCmd, NULL, NULL);
    Sv_RegisterCommand("array",  SvArrayObjCmd,  NULL, NULL);
    Sv_RegisterCommand("names",  SvNamesObjCmd,  NULL, NULL);
    Sv_RegisterCommand("pop",    SvPopObjCmd,    NULL, NULL);
    Sv_RegisterCommand("move",   SvMoveObjCmd,   NULL, NULL);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Sv_Init --
 *
 *    Creates commands in current interpreter.
 *
 * Results:
 *    None.
 *
 * Side effects
 *    Many new command created in current interpreter. Global data
 *    structures used by them initialized as well.
 *
 *-----------------------------------------------------------------------------
 */

int
Sv_Init (interp)
    Tcl_Interp *interp;
{
    register int i;
    Bucket *bucketPtr;
    SvCmdInfo *cmdPtr;

    /*
     * Register commands
     */

    SvRegisterStdCommands();
    Sv_RegisterListCommands();

    /*
     * Get Tcl object types. These are used
     * in custom object duplicator function.
     */

    booleanObjTypePtr   = Tcl_GetObjType("boolean");
    byteArrayObjTypePtr = Tcl_GetObjType("bytearray");
    doubleObjTypePtr    = Tcl_GetObjType("double");
    intObjTypePtr       = Tcl_GetObjType("int");
    stringObjTypePtr    = Tcl_GetObjType("string");

    /*
     * Plug-in registered commands in current interpreter
     * These are new commands, located in tsv::* namespace.
     */

    for (cmdPtr = svCmdInfo; cmdPtr; cmdPtr = cmdPtr->nextPtr) {
        Tcl_CreateObjCommand(interp, cmdPtr->cmdName, cmdPtr->objProcPtr,
                (ClientData)cmdPtr->clientData, (Tcl_CmdDeleteProc*)0);
    }
#ifndef OLD_COMPAT
    /*
     * Plug-in commands for compatibility with thread::sv_* interface.
     * These are going to be dropped in 3.0 version.
     */

    for (cmdPtr = svCmdInfo; cmdPtr; cmdPtr = cmdPtr->nextPtr) {
        char buf[32];
	sprintf(buf, "thread::sv_%s", cmdPtr->name);
        Tcl_CreateObjCommand(interp, buf, cmdPtr->objProcPtr,
                (ClientData)cmdPtr->clientData, (Tcl_CmdDeleteProc*)0);
    }
#endif

    /*
     * Create array of buckets and initialize each bucket
     */

    if (buckets == NULL) {
        Tcl_MutexLock(&bucketsMutex);
        if (buckets == NULL) {
            buckets = (Bucket *)Tcl_Alloc(sizeof(Bucket) * svconf.numbuckets);
            for (i = 0; i < svconf.numbuckets; ++i) {
                bucketPtr = &buckets[i];
                memset(bucketPtr, 0, sizeof(Bucket));
                Tcl_InitHashTable(&bucketPtr->arrays, TCL_STRING_KEYS);
                Tcl_InitHashTable(&bucketPtr->handles, TCL_ONE_WORD_KEYS);
            }
            Tcl_CreateExitHandler((Tcl_ExitProc*)SvFinalize, NULL);
            /*
             * We use this trick to get the tclEmptyStringRep pointer
             * defined by Tcl without directly referencing it. If we
             * referenced the symbol directly, we could not link
             * under Windows. It would also break under Linux if
             * both Tcl and the Threads package were dynamically
             * loaded into an application.
             */
            {
                Tcl_Obj *dummy = Tcl_NewObj();
                Sv_tclEmptyStringRep = dummy->bytes;
                Tcl_DecrRefCount(dummy);
            }
        }
        Tcl_MutexUnlock(&bucketsMutex);
    }

    return TCL_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvFinalize --
 *
 *    Unset all arrays and reclaim all buckets.
 *
 * Results:
 *    None.
 *
 * Side effects
 *    Memory gets reclaimed.
 *
 *-----------------------------------------------------------------------------
 */

static void
SvFinalize (clientData)
    ClientData clientData;
{
    register int i;
    SvCmdInfo *cmdPtr;
    RegType *regPtr;

    Tcl_HashEntry *hashPtr;
    Tcl_HashSearch search;

    /*
     * Reclaim memory for shared arrays
     */

    if (buckets != NULL) {
        Tcl_MutexLock(&bucketsMutex);
        if (buckets != NULL) {
            for (i = 0; i < svconf.numbuckets; ++i) {
                Bucket *bucketPtr = &buckets[i];
                hashPtr = Tcl_FirstHashEntry(&bucketPtr->arrays, &search);
                while (hashPtr != NULL) {
                    DeleteArray((Array*)Tcl_GetHashValue(hashPtr));
                    hashPtr = Tcl_NextHashEntry(&search);
                }
                if (bucketPtr->lock) {
                    Tcl_MutexFinalize(&bucketPtr->lock);
                }
                SvFinalizeContainers(bucketPtr);
                Tcl_DeleteHashTable(&bucketPtr->handles);
                Tcl_DeleteHashTable(&bucketPtr->arrays);
            }
            Tcl_Free((char *)buckets), buckets = NULL;
        }
        buckets = NULL;
        Tcl_MutexUnlock(&bucketsMutex);
    }

    Tcl_MutexLock(&typesMutex);

    /*
     * Reclaim memory for registered commands
     */

    if (svCmdInfo != NULL) {
        cmdPtr = svCmdInfo;
        while (cmdPtr) {
            SvCmdInfo *tmpPtr = cmdPtr->nextPtr;
            Tcl_Free((char*)cmdPtr);
            cmdPtr = tmpPtr;
        }
        svCmdInfo = NULL;
    }

    /*
     * Reclaim memory for registered object types
     */

    if (regType != NULL) {
        regPtr = regType;
        while (regPtr) {
            RegType *tmpPtr = regPtr->nextPtr;
            Tcl_Free((char*)regPtr);
            regPtr = tmpPtr;
        }
        regType = NULL;
    }

    Tcl_MutexUnlock(&typesMutex);
}

/* EOF $RCSfile: threadSvCmd.c,v $ */

/* Emacs Setup Variables */
/* Local Variables:      */
/* mode: C               */
/* indent-tabs-mode: nil */
/* c-basic-offset: 4     */
/* End:                  */

