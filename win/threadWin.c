/*
 * threadWin.c --
 *
 * Windows specific aspects for the thread extension.
 *
 * see http://dev.activestate.com/doc/howto/thread_model.html
 *
 * Some of this code is based on work done by Richard Hipp on behalf of
 * Conservation Through Innovation, Limited, with their permission.
 *
 * Copyright (c) 1998 by Sun Microsystems, Inc.
 * Copyright (c) 1999,2000 by Scriptics Corporation.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: threadWin.c,v 1.4 2002/01/26 05:52:01 davygrvy Exp $
 */

#include "../generic/tclThread.h"
#include <windows.h>
#include <process.h>

#if 0
/* only Windows 2000 (or XP??) has this function */
HANDLE (WINAPI *winOpenThreadProc)(DWORD, BOOL, HANDLE);

void
ThreadpInit (void)
{
    HINSTANCE hKernel = LoadLibrary("kernel32");
    winOpenThreadProc = GetProcAddress("OpenThread", hKernel);
}

int
ThreadpKill (Tcl_Interp *interp, long id)
{
    HANDLE hThread;
    int result = TCL_OK;

    if (winOpenThreadProc) {
	hThread = winOpenThreadProc(THREAD_TERMINATE, FALSE, id);

	/* not to be misunderstood as "devilishly clever", but evil in it's pure form. */
	TerminateThread(hThread, 666);
    } else {
	Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		"Can't kill threads on this OS, sorry.", NULL);
	result = TCL_ERROR;
    }
    return result;
}
#endif
