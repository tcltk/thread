/*
 * aolstub.cpp --
 *
 * Adds interface for loading the extension into the AOLserver.
 *
 * Copyright (c) 2002 by Zoran Vasiljevic.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * Rcsid: @(#)$Id: aolstub.cpp,v 1.3 2002/12/19 09:55:43 vasiljevic Exp $
 * ---------------------------------------------------------------------------
 */

#ifdef NS_AOLSERVER
#include <ns.h>

int Ns_ModuleVersion = 1;

/*
 *----------------------------------------------------------------------------
 *
 * NsThread_Init --
 *
 *    Loads the package for the first time, i.e. in the startup thread.
 *
 * Results:
 *    Standard Tcl result
 *
 * Side effects:
 *    Package initialized. Tcl commands created.
 *
 *----------------------------------------------------------------------------
 */

static int
NsThread_Init (Tcl_Interp *interp, void *context)
{
    int ret = Thread_Init(interp);

    if (ret != TCL_OK) {
        Ns_Log(Warning, "can't load module %s: %s", 
               (char *)context, Tcl_GetStringResult(interp));
    }

    return ret;
}

/*
 *----------------------------------------------------------------------------
 *
 * Ns_ModuleInit --
 *
 *    Called by the AOLserver when loading shared object file.
 *
 * Results:
 *    Standard AOLserver result
 *
 * Side effects:
 *    Many. Depends on the package.
 *
 *----------------------------------------------------------------------------
 */

int
Ns_ModuleInit(char *hServer, char *hMod)
{
    return (Ns_TclInitInterps(hServer, NsThread_Init, (void*)hMod) == TCL_OK)
        ? NS_OK : NS_ERROR; 
}

#endif /* NS_AOLSERVER */

/* EOF $RCSfile: aolstub.cpp,v $ */

/* Emacs Setup Variables */
/* Local Variables:      */
/* mode: C               */
/* indent-tabs-mode: nil */
/* c-basic-offset: 4     */
/* End:                  */
