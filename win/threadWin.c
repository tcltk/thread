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
 * RCS: @(#) $Id: threadWin.c,v 1.3 2002/01/21 23:58:36 davygrvy Exp $
 */

#include "../generic/tclThread.h"
#include <windows.h>
#include <process.h>

/* EOF $RCSfile: threadWin.c,v $ */

/* Emacs Setup Variables */
/* Local Variables:      */
/* mode: C               */
/* indent-tabs-mode: nil */
/* c-basic-offset: 4     */
/* End:                  */
