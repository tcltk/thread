/* 
 * This is the header file for the module that implements some missing
 * synchronization priomitives from the Tcl API.
 *
 * Copyright (c) 2002 by Zoran Vasiljevic.
 *
 * See the file "license.txt" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * Rcsid: @(#)$Id: threadSpCmd.h,v 1.1 2003/12/22 21:26:11 vasiljevic Exp $
 * ---------------------------------------------------------------------------
 */

#ifndef _SP_H_
#define _SP_H_

#include <tcl.h>

/*
 * Implementation of the recursive mutex.
 */

typedef struct Sp_RecursiveMutex_ {
    Tcl_Mutex lock;             /* Regular mutex */
    Tcl_Condition cond;         /* Wait here to be allowed to lock */
    Tcl_ThreadId owner;         /* Current lock owner */
    unsigned int lrcnt;         /* Lock ref count */
} Sp_RecursiveMutex_;

typedef Sp_RecursiveMutex_* Sp_RecursiveMutex;

/*
 * Implementation of the readwriter mutex.
 */

typedef struct Sp_ReadWriteMutex_ {
    Tcl_Mutex lock;             /* Regular mutex */
    Tcl_Condition rcond;        /* Reader lockers wait here */
    Tcl_Condition wcond;        /* Writer lockers wait here */
    unsigned int numrd;	        /* # of readers waiting for lock */
    unsigned int numwr;         /* # of writers waiting for lock */
    int lrcnt;                  /* Lock ref count, > 0: # of shared
                                 * readers, -1: exclusive writer */
} Sp_ReadWriteMutex_;

typedef Sp_ReadWriteMutex_* Sp_ReadWriteMutex;

/*
 * API for recursive mutex.
 */

void Sp_RecursiveMutexLock(Sp_RecursiveMutex *mutexPtr);
void Sp_RecursiveMutexUnlock(Sp_RecursiveMutex *mutexPtr);
void Sp_RecursiveMutexFinalize(Sp_RecursiveMutex *mutexPtr);

/*
 * API for reader/writer mutex.
 */

void Sp_ReadWriteMutexRLock(Sp_ReadWriteMutex *mutexPtr);
void Sp_ReadWriteMutexWLock(Sp_ReadWriteMutex *mutexPtr);
void Sp_ReadWriteMutexUnlock(Sp_ReadWriteMutex *mutexPtr);
void Sp_ReadWriteMutexFinalize(Sp_ReadWriteMutex *mutexPtr);

#endif /* _SP_H_ */

/* EOF $RCSfile: threadSpCmd.h,v $ */

/* Emacs Setup Variables */
/* Local Variables:      */
/* mode: C               */
/* indent-tabs-mode: nil */
/* c-basic-offset: 4     */
/* End:                  */
