/* 
 * threadWin.c --
 *
 *	Windows specific aspects for the thread extension.
 *
 *	see http://dev.ajubasolutions.com/doc/howto/thread_model.html
 *
 *	Some of this code is based on work done by Richard Hipp on behalf of
 *	Conservation Through Innovation, Limited, with their permission.
 *
 * Copyright (c) 1998 by Sun Microsystems, Inc.
 * Copyright (c) 1999,2000 by Scriptics Corporation.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: threadWin.c,v 1.1 2001/09/05 23:01:12 davygrvy Exp $
 */

#include "tclThread.h"
#include <windows.h>
#include <process.h>

void
ThreadKill (long id)
{
    HANDLE hThread = 0;

    /* get handle from id here... how? */

    TerminateThread(hThread, 666);
}
