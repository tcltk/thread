/* 
 * This is the header file for the module that implements shared variables.
 * for protected multithreaded access.
 *
 * See the file "license.txt" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * Rcsid: @(#)$Id: threadSvCmd.h,v 1.7 2002/11/24 17:01:27 vasiljevic Exp $
 * ---------------------------------------------------------------------------
 */

#ifndef _SV_H_
#define _SV_H_

#include <tcl.h>
#include <ctype.h>
#include <string.h>

/*
 * Starting from 8.4 core, Tcl API is CONST'ified
 */

#if (TCL_MAJOR_VERSION == 8) && (TCL_MINOR_VERSION <= 3) 
# define CONST84
#endif

/*
 * Uncomment following line to get command-line
 * compatibility with AOLserver nsv_* commands
 */

#define NSV_COMPAT 1

/*
 * Uncomment following line to force command-line
 * compatibility with older thread::sv_ commands
 * If you leave it commented-out, the older style
 * command is going to be included in addition to
 * the new tsv::* style.
 */

/* #define OLD_COMPAT 1 */

#ifdef NS_AOLSERVER
# ifdef NSV_COMPAT
#  define N "nsv_"  /* Compatiblity prefix for AOLserver */
# else
#  define N "sv_"   /* Regular command prefix for AOLserver */
# endif
#else
# ifdef OLD_COMPAT
#  define N  "thread::sv_" /* Old command prefix for Tcl */
# else
#  define N  "tsv::" /* Regular command prefix for Tcl */
# endif
#endif

/*
 * This structure is used to simplify interaction with
 * AOLserver. The AOLserver can tune number of shared
 * arrays per bucket, which can be useful when tuning
 * MT-performance of shared arrays. 
 * In AOLserver environment, this structure is filled
 * in by reading server's current configuration.
 * In regular Tcl, we have no easy mechanism for this
 * so we just make it constant (8 buckets).
 */

typedef struct Svconf {
    int numbuckets;
} Svconf;

/*
 * Used when creating arrays/variables
 */

#define FLAGS_CREATEARRAY  1   /* Create the array in bucket if none found */
#define FLAGS_NOERRMSG     2   /* Do not format error message */
#define FLAGS_CREATEVAR    4   /* Create the array variable if none found */

/*
 * The following structure defines a collection of arrays.
 * Only the arrays within a given bucket share a lock, allowing for more
 * concurency.
 */

typedef struct Bucket {
    Tcl_Mutex lock;            /* Lock when accessing arrays in this bucket */
    Tcl_HashTable arrays;      /* Hash table of all arrays in bucket */
    Tcl_HashTable handles;     /* Hash table of given-out handles in bucket */
    struct Container *freeCt;  /* List of free Tcl-object containers */
} Bucket;

/*
 * The following structure maintains the context for each variable array.
 */

typedef struct Array {
    Bucket *bucketPtr;         /* Array bucket. */
    Tcl_HashEntry *entryPtr;   /* Entry in bucket array table. */
    Tcl_HashEntry *handlePtr;  /* Entry in handles table */
    Tcl_HashTable vars;        /* Table of variables. */
} Array;

/*
 * The object container for Tcl-objects stored within shared arrays.
 */

typedef struct Container {
    Bucket *bucketPtr;         /* Bucket holding the array below */
    Array *arrayPtr;           /* Array with the object container*/
    Tcl_HashEntry *entryPtr;   /* Entry in array table. */
    Tcl_HashEntry *handlePtr;  /* Entry in handles table */
    Tcl_Obj *tclObj;           /* Tcl object to hold shared values */
    char *chunkAddr;           /* Address of one chunk of object containers */
    struct Container *nextPtr; /* Next object container in the free list */
} Container;

/*
 * Structure for generating command names in Tcl
 */

typedef struct SvCmdInfo {
    char *name;                 /* The short name of the command */
    char *cmdName;              /* Real (rewritten) name of the command */
    Tcl_ObjCmdProc *objProcPtr; /* The object-based command procedure */
    Tcl_CmdDeleteProc *delProcPtr; /* Pointer to command delete function */
    ClientData clientData;     /* Pointer passed to above command */
    struct SvCmdInfo *nextPtr;  /* Next in chain of registered commands */
} SvCmdInfo;

/*
 * Structure for registering special object duplicator functions.
 * Some regular Tcl object duplicators produce shallow instead of
 * proper deep copies of the object. While this is considered ok
 * in single-threaded apps, a multithreaded app could have problems
 * when accessing objects which live in (i.e. are accessed from) 
 * different interpreters.
 * So, for each object type which should be stored in shared object
 * pools, we must assure that the object is copied properly.
 */

typedef struct RegType {
    Tcl_ObjType *typePtr;       /* Type of the registered object */
    Tcl_DupInternalRepProc *dupIntRepProc; /* Special deep-copy duper */
    struct RegType *nextPtr;    /* Next in chain of registered types */
} RegType;

/*
 * Limited API functions
 */

void Sv_RegisterCommand(char*,Tcl_ObjCmdProc*,Tcl_CmdDeleteProc*,ClientData);
void Sv_RegisterObjType(Tcl_ObjType*, Tcl_DupInternalRepProc*);
int  Sv_Container(Tcl_Interp*,int,Tcl_Obj*CONST objv[],Container**,int*,int);

/*
 * Private version of Tcl_DuplicateObj which takes care about
 * copying objects when loaded to and retrieved from shared array.
 */

Tcl_Obj* Sv_DuplicateObj(Tcl_Obj*);

#define LOCK_BUCKET(a)   if ((a)->lock != (Tcl_Mutex)-1) \
                             Tcl_MutexLock(&(a)->lock)
#define UNLOCK_BUCKET(a) if ((a)->lock != (Tcl_Mutex)-1) \
                             Tcl_MutexUnlock(&(a)->lock)

#define Sv_Lock(a)       if ((a)->bucketPtr->lock != (Tcl_Mutex)-1) \
                             Tcl_MutexLock(&(a)->bucketPtr->lock)
#define Sv_Unlock(a)     if ((a)->bucketPtr->lock != (Tcl_Mutex)-1) \
                             Tcl_MutexUnlock(&(a)->bucketPtr->lock)

#define UnlockArray(a)    if ((a)->bucketPtr->lock != (Tcl_Mutex)-1) \
                             Tcl_MutexUnlock(&((a)->bucketPtr->lock))

/*
 * Needed when copying objects. This is something not exported
 * from Tcl, therefore we must use tricks to get this value.
 * See in sv.c for details.
 */

extern char* tclEmptyStringRep;

#undef  TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLIMPORT

#endif /* _SV_H_ */

/* EOF $RCSfile: threadSvCmd.h,v $ */

/* Emacs Setup Variables */
/* Local Variables:      */
/* mode: C               */
/* indent-tabs-mode: nil */
/* c-basic-offset: 4     */
/* End:                  */

