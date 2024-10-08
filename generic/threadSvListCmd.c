/*
 * Implementation of most standard Tcl list processing commands
 * suitable for operation on thread shared (list) variables.
 *
 * Copyright (c) 2002 by Zoran Vasiljevic.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 * ----------------------------------------------------------------------------
 */

#include "tclThreadInt.h"
#include "threadSvCmd.h"
#include "threadSvListCmd.h"

/*
 * Implementation of list commands for shared variables.
 * Most of the standard Tcl list commands are implemented.
 * There are also two new commands: "lpop" and "lpush".
 * Those are very convenient for simple stack operations.
 *
 * Main difference to standard Tcl commands is that our commands
 * operate on list variable per-reference instead per-value.
 * This way we avoid frequent object shuffling between shared
 * containers and current interpreter, thus increasing speed.
 */

static Tcl_ObjCmdProc2 SvLpopObjCmd;      /* lpop        */
static Tcl_ObjCmdProc2 SvLpushObjCmd;     /* lpush       */
static Tcl_ObjCmdProc2 SvLappendObjCmd;   /* lappend     */
static Tcl_ObjCmdProc2 SvLreplaceObjCmd;  /* lreplace    */
static Tcl_ObjCmdProc2 SvLlengthObjCmd;   /* llength     */
static Tcl_ObjCmdProc2 SvLindexObjCmd;    /* lindex      */
static Tcl_ObjCmdProc2 SvLinsertObjCmd;   /* linsert     */
static Tcl_ObjCmdProc2 SvLrangeObjCmd;    /* lrange      */
static Tcl_ObjCmdProc2 SvLsearchObjCmd;   /* lsearch     */
static Tcl_ObjCmdProc2 SvLsetObjCmd;      /* lset        */

/*
 * Inefficient list duplicator function which,
 * however, produces deep list copies, unlike
 * the original, which just makes shallow copies.
 */

static void DupListObjShared(Tcl_Obj*, Tcl_Obj*);

/*
 * This mutex protects a static variable which tracks
 * registration of commands and object types.
 */

static Tcl_Mutex initMutex;

/*
 * Functions for implementing the "lset" list command
 */

static Tcl_Obj*
SvLsetFlat(Tcl_Interp *interp, Tcl_Obj *listPtr, Tcl_Size indexCount,
	   Tcl_Obj **indexArray, Tcl_Obj *valuePtr);


/*
 *-----------------------------------------------------------------------------
 *
 * Sv_RegisterListCommands --
 *
 *      Register list commands with shared variable module.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Memory gets allocated
 *
 *-----------------------------------------------------------------------------
 */

void
Sv_RegisterListCommands(void)
{
    static int initialized = 0;

    if (initialized == 0) {
	Tcl_MutexLock(&initMutex);
	if (initialized == 0) {
	    /* Create list with 1 empty element. */
	    Tcl_Obj *listobj = Tcl_NewObj();
	    listobj = Tcl_NewListObj(1, &listobj);
	    Sv_RegisterObjType(listobj->typePtr, DupListObjShared);
	    Tcl_DecrRefCount(listobj);

	    Sv_RegisterCommand("lpop",     SvLpopObjCmd,     NULL, 0);
	    Sv_RegisterCommand("lpush",    SvLpushObjCmd,    NULL, 0);
	    Sv_RegisterCommand("lappend",  SvLappendObjCmd,  NULL, 0);
	    Sv_RegisterCommand("lreplace", SvLreplaceObjCmd, NULL, 0);
	    Sv_RegisterCommand("linsert",  SvLinsertObjCmd,  NULL, 0);
	    Sv_RegisterCommand("llength",  SvLlengthObjCmd,  NULL, 0);
	    Sv_RegisterCommand("lindex",   SvLindexObjCmd,   NULL, 0);
	    Sv_RegisterCommand("lrange",   SvLrangeObjCmd,   NULL, 0);
	    Sv_RegisterCommand("lsearch",  SvLsearchObjCmd,  NULL, 0);
	    Sv_RegisterCommand("lset",     SvLsetObjCmd,     NULL, 0);

	    initialized = 1;
	}
	Tcl_MutexUnlock(&initMutex);
    }
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvLpopObjCmd --
 *
 *      This procedure is invoked to process the "tsv::lpop" command.
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
SvLpopObjCmd (
    void *arg,
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[]
) {
    int ret;
    Tcl_Size off, llen, index = 0, iarg = 0;
    Tcl_Obj *elPtr = NULL;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::lpop array key ?index?
     *          $list lpop ?index?
     */

    ret = Sv_GetContainer(interp, objc, objv, &svObj, &off, 0);
    if (ret != TCL_OK) {
	return TCL_ERROR;
    }
    if (objc > 1 + off) {
	Tcl_WrongNumArgs(interp, off, objv, "?index?");
	goto cmd_err;
    }
    if (objc == 1 + off) {
	iarg = off;
    }
    ret = Tcl_ListObjLength(interp, svObj->tclObj, &llen);
    if (ret != TCL_OK) {
	goto cmd_err;
    }
    if (iarg) {
	ret = Tcl_GetIntForIndex(interp, objv[iarg], llen-1, &index);
	if (ret != TCL_OK) {
	    goto cmd_err;
	}
    }
    if ((index < 0) || (index >= llen)) {
	/* Ignore out-of bounds, like Tcl does */
	return Sv_PutContainer(interp, svObj, SV_UNCHANGED);
    }
    ret = Tcl_ListObjIndex(interp, svObj->tclObj, index, &elPtr);
    if (ret != TCL_OK) {
	goto cmd_err;
    }

    Tcl_IncrRefCount(elPtr);
    ret = Tcl_ListObjReplace(interp, svObj->tclObj, index, 1, 0, NULL);
    if (ret != TCL_OK) {
	Tcl_DecrRefCount(elPtr);
	goto cmd_err;
    }
    Tcl_SetObjResult(interp, elPtr);
    Tcl_DecrRefCount(elPtr);
    return Sv_PutContainer(interp, svObj, SV_CHANGED);

 cmd_err:
    return Sv_PutContainer(interp, svObj, SV_ERROR);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvLpushObjCmd --
 *
 *      This procedure is invoked to process the "tsv::lpush" command.
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
SvLpushObjCmd (
    void *arg,
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[]
) {
    int ret, flg;
    Tcl_Size off, llen, index = 0;
    Tcl_Obj *args[1];
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::lpush array key element ?index?
     *          $list lpush element ?index?
     */

    flg = FLAGS_CREATEARRAY | FLAGS_CREATEVAR;
    ret = Sv_GetContainer(interp, objc, objv, &svObj, &off, flg);
    if (ret != TCL_OK) {
	return TCL_ERROR;
    }
    if (objc < 1 + off) {
	Tcl_WrongNumArgs(interp, off, objv, "element ?index?");
	goto cmd_err;
    }
    ret = Tcl_ListObjLength(interp, svObj->tclObj, &llen);
    if (ret != TCL_OK) {
	goto cmd_err;
    }
    if (objc == 2 + off) {
	ret = Tcl_GetIntForIndex(interp, objv[off+1], llen, &index);
	if (ret != TCL_OK) {
	    goto cmd_err;
	}
	if (index < 0) {
	    index = 0;
	} else if (index > llen) {
	    index = llen;
	}
    }

    args[0] = Sv_DuplicateObj(objv[off]);
    ret = Tcl_ListObjReplace(interp, svObj->tclObj, index, 0, 1, args);
    if (ret != TCL_OK) {
	Tcl_DecrRefCount(args[0]);
	goto cmd_err;
    }

    return Sv_PutContainer(interp, svObj, SV_CHANGED);

 cmd_err:
    return Sv_PutContainer(interp, svObj, SV_ERROR);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvLappendObjCmd --
 *
 *      This procedure is invoked to process the "tsv::lappend" command.
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
SvLappendObjCmd(
    void *arg,
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[]
) {
    int ret, flg;
    Tcl_Size i, off;
    Tcl_Obj *dup;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::lappend array key value ?value ...?
     *          $list lappend value ?value ...?
     */

    flg = FLAGS_CREATEARRAY | FLAGS_CREATEVAR;
    ret = Sv_GetContainer(interp, objc, objv, &svObj, &off, flg);
    if (ret != TCL_OK) {
	return TCL_ERROR;
    }
    if (objc < 1 + off) {
	Tcl_WrongNumArgs(interp, off, objv, "value ?value ...?");
	goto cmd_err;
    }
    for (i = off; i < objc; i++) {
	dup = Sv_DuplicateObj(objv[i]);
	ret = Tcl_ListObjAppendElement(interp, svObj->tclObj, dup);
	if (ret != TCL_OK) {
	    Tcl_DecrRefCount(dup);
	    goto cmd_err;
	}
    }

    Tcl_SetObjResult(interp, Sv_DuplicateObj(svObj->tclObj));

    return Sv_PutContainer(interp, svObj, SV_CHANGED);

 cmd_err:
    return Sv_PutContainer(interp, svObj, SV_ERROR);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvLreplaceObjCmd --
 *
 *      This procedure is invoked to process the "tsv::lreplace" command.
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
SvLreplaceObjCmd(
    void *arg,
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[]
) {
    const char *firstArg;
    Tcl_Size off, llen, argLen, first, last, ndel, nargs, i, j;
    int ret;
    Tcl_Obj **args = NULL;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::lreplace array key first last ?element ...?
     *          $list lreplace first last ?element ...?
     */

    ret = Sv_GetContainer(interp, objc, objv, &svObj, &off, 0);
    if (ret != TCL_OK) {
	return TCL_ERROR;
    }
    if (objc < 2 + off) {
	Tcl_WrongNumArgs(interp, off, objv, "first last ?element ...?");
	goto cmd_err;
    }
    ret = Tcl_ListObjLength(interp, svObj->tclObj, &llen);
    if (ret != TCL_OK) {
	goto cmd_err;
    }
    ret = Tcl_GetIntForIndex(interp, objv[off], llen-1, &first);
    if (ret != TCL_OK) {
	goto cmd_err;
    }
    ret = Tcl_GetIntForIndex(interp, objv[off+1], llen-1, &last);
    if (ret != TCL_OK) {
	goto cmd_err;
    }

    firstArg = Tcl_GetStringFromObj(objv[off], &argLen);
    if (first < 0)  {
	first = 0;
    }
    if (llen && first >= llen && strncmp(firstArg, "end", argLen)) {
	Tcl_AppendResult(interp, "list doesn't have element ", firstArg, (void *)NULL);
	goto cmd_err;
    }
    if (last >= llen) {
	last = llen - 1;
    }
    if (first <= last) {
	ndel = last - first + 1;
    } else {
	ndel = 0;
    }

    nargs = objc - off - 2;
    if (nargs) {
	args = (Tcl_Obj**)Tcl_Alloc(nargs * sizeof(Tcl_Obj*));
	for(i = off + 2, j = 0; i < objc; i++, j++) {
	    args[j] = Sv_DuplicateObj(objv[i]);
	}
    }

    ret = Tcl_ListObjReplace(interp, svObj->tclObj, first, ndel, nargs, args);
    if (args) {
	if (ret != TCL_OK) {
	    for(i = off + 2, j = 0; i < objc; i++, j++) {
		Tcl_DecrRefCount(args[j]);
	    }
	}
	Tcl_Free(args);
    }

    return Sv_PutContainer(interp, svObj, SV_CHANGED);

 cmd_err:
    return Sv_PutContainer(interp, svObj, SV_ERROR);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvLrangeObjCmd --
 *
 *      This procedure is invoked to process the "tsv::lrange" command.
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
SvLrangeObjCmd(
    void *arg,
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[]
) {
    int ret;
    Tcl_Size first, last, i, off, llen, nargs, j;
    Tcl_Obj **elPtrs, **args;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::lrange array key first last
     *          $list lrange first last
     */

    ret = Sv_GetContainer(interp, objc, objv, &svObj, &off, 0);
    if (ret != TCL_OK) {
	return TCL_ERROR;
    }
    if (objc != 2 + off) {
	Tcl_WrongNumArgs(interp, off, objv, "first last");
	goto cmd_err;
    }
    ret = Tcl_ListObjGetElements(interp, svObj->tclObj, &llen, &elPtrs);
    if (ret != TCL_OK) {
	goto cmd_err;
    }
    ret = Tcl_GetIntForIndex(interp, objv[off], llen-1, &first);
    if (ret != TCL_OK) {
	goto cmd_err;
    }
    ret = Tcl_GetIntForIndex(interp, objv[off+1], llen-1, &last);
    if (ret != TCL_OK) {
	goto cmd_err;
    }
    if (first < 0)  {
	first = 0;
    }
    if (last >= llen) {
	last = llen - 1;
    }
    if (first > last) {
	goto cmd_ok;
    }

    nargs = last - first + 1;
    args  = (Tcl_Obj **)Tcl_Alloc(nargs * sizeof(Tcl_Obj *));
    for (i = first, j = 0; i <= last; i++, j++) {
	args[j] = Sv_DuplicateObj(elPtrs[i]);
    }

    Tcl_ResetResult(interp);
    Tcl_SetListObj(Tcl_GetObjResult(interp), nargs, args);
    Tcl_Free(args);

 cmd_ok:
    return Sv_PutContainer(interp, svObj, SV_UNCHANGED);

 cmd_err:
    return Sv_PutContainer(interp, svObj, SV_ERROR);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvLinsertObjCmd --
 *
 *      This procedure is invoked to process the "tsv::linsert" command.
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
SvLinsertObjCmd(
    void *arg,
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[]
) {
    int ret, flg;
    Tcl_Size off, nargs, i, j, llen, index = 0;
    Tcl_Obj **args;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::linsert array key index element ?element ...?
     *          $list linsert element ?element ...?
     */

    flg = FLAGS_CREATEARRAY | FLAGS_CREATEVAR;
    ret = Sv_GetContainer(interp, objc, objv, &svObj, &off, flg);
    if (ret != TCL_OK) {
	return TCL_ERROR;
    }
    if (objc < 2 + off) {
	Tcl_WrongNumArgs(interp, off, objv, "index element ?element ...?");
	goto cmd_err;
    }
    ret = Tcl_ListObjLength(interp, svObj->tclObj, &llen);
    if (ret != TCL_OK) {
	goto cmd_err;
    }
    ret = Tcl_GetIntForIndex(interp, objv[off], llen, &index);
    if (ret != TCL_OK) {
	goto cmd_err;
    }
    if (index < 0) {
	index = 0;
    } else if (index > llen) {
	index = llen;
    }

    nargs = objc - off - 1;
    args  = (Tcl_Obj **)Tcl_Alloc(nargs * sizeof(Tcl_Obj *));
    for (i = off + 1, j = 0; i < objc; i++, j++) {
	 args[j] = Sv_DuplicateObj(objv[i]);
    }
    ret = Tcl_ListObjReplace(interp, svObj->tclObj, index, 0, nargs, args);
    if (ret != TCL_OK) {
	for (i = off + 1, j = 0; i < objc; i++, j++) {
	    Tcl_DecrRefCount(args[j]);
	}
	Tcl_Free(args);
	goto cmd_err;
    }

    Tcl_Free(args);

    return Sv_PutContainer(interp, svObj, SV_CHANGED);

 cmd_err:
    return Sv_PutContainer(interp, svObj, SV_ERROR);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvLlengthObjCmd --
 *
 *      This procedure is invoked to process the "tsv::llength" command.
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
SvLlengthObjCmd(
    void *arg,
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[]
) {
    Tcl_Size llen, off;
    int ret;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::llength array key
     *          $list llength
     */

    ret = Sv_GetContainer(interp, objc, objv, &svObj, &off, 0);
    if (ret != TCL_OK) {
	return TCL_ERROR;
    }

    ret = Tcl_ListObjLength(interp, svObj->tclObj, &llen);
    if (ret == TCL_OK) {
	Tcl_SetObjResult(interp, Tcl_NewIntObj(llen));
    }
    if (Sv_PutContainer(interp, svObj, SV_UNCHANGED) != TCL_OK) {
	return TCL_ERROR;
    }

    return ret;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvLsearchObjCmd --
 *
 *      This procedure is invoked to process the "tsv::lsearch" command.
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
SvLsearchObjCmd(
    void *arg,
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[]
) {
    int ret, match;
    Tcl_Size off, index, length, i, listc, imode, ipatt;
    const char *patBytes;
    Tcl_Obj **listv;
    Container *svObj = (Container*)arg;

    static const char *const modes[] = {"-exact", "-glob", "-regexp", NULL};
    enum {LS_EXACT, LS_GLOB, LS_REGEXP} mode;

    mode = LS_GLOB;

    /*
     * Syntax:
     *          tsv::lsearch array key ?mode? pattern
     *          $list lsearch ?mode? pattern
     */

    ret = Sv_GetContainer(interp, objc, objv, &svObj, &off, 0);
    if (ret != TCL_OK) {
	return TCL_ERROR;
    }
    if (objc == 2 + off) {
	imode = off;
	ipatt = off + 1;
    } else if (objc == 1 + off) {
	imode = 0;
	ipatt = off;
    } else {
	Tcl_WrongNumArgs(interp, off, objv, "?mode? pattern");
	goto cmd_err;
    }
    if (imode) {
	ret = Tcl_GetIndexFromObjStruct(interp, objv[imode], modes, sizeof(char *), "search mode",
		0, &mode);
	if (ret != TCL_OK) {
	    goto cmd_err;
	}
    }
    ret = Tcl_ListObjGetElements(interp, svObj->tclObj, &listc, &listv);
    if (ret != TCL_OK) {
	goto cmd_err;
    }

    index = TCL_INDEX_NONE;
    patBytes = Tcl_GetStringFromObj(objv[ipatt], &length);

    for (i = 0; i < listc; i++) {
	match = 0;
	switch (mode) {
	case LS_GLOB:
	    match = Tcl_StringCaseMatch(Tcl_GetString(listv[i]), patBytes, 0);
	    break;

	case LS_EXACT: {
	    Tcl_Size len;
	    const char *bytes = Tcl_GetStringFromObj(listv[i], &len);
	    if (length == len) {
		match = (memcmp(bytes, patBytes, length) == 0);
	    }
	    break;
	}
	case LS_REGEXP:
	    match = Tcl_RegExpMatchObj(interp, listv[i], objv[ipatt]);
	    if (match < 0) {
		goto cmd_err;
	    }
	    break;
	}
	if (match) {
	    index = i;
	    break;
	}
    }

    Tcl_SetObjResult(interp, Tcl_NewWideIntObj(index));

    return Sv_PutContainer(interp, svObj, SV_UNCHANGED);

 cmd_err:
    return Sv_PutContainer(interp, svObj, SV_ERROR);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvLindexObjCmd --
 *
 *      This procedure is invoked to process the "tsv::lindex" command.
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
SvLindexObjCmd(
    void *arg,
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[]
) {
    Tcl_Obj **elPtrs;
    int ret;
    Tcl_Size llen, index, off;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::lindex array key index
     *          $list lindex index
     */

    ret = Sv_GetContainer(interp, objc, objv, &svObj, &off, 0);
    if (ret != TCL_OK) {
	return TCL_ERROR;
    }
    if (objc != 1 + off) {
	Tcl_WrongNumArgs(interp, off, objv, "index");
	goto cmd_err;
    }
    ret = Tcl_ListObjGetElements(interp, svObj->tclObj, &llen, &elPtrs);
    if (ret != TCL_OK) {
	goto cmd_err;
    }
    ret = Tcl_GetIntForIndex(interp, objv[off], llen-1, &index);
    if (ret != TCL_OK) {
	goto cmd_err;
    }
    if ((index >= 0) && (index < llen)) {
	Tcl_SetObjResult(interp, Sv_DuplicateObj(elPtrs[index]));
    }

    return Sv_PutContainer(interp, svObj, SV_UNCHANGED);

 cmd_err:
    return Sv_PutContainer(interp, svObj, SV_ERROR);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvLsetObjCmd --
 *
 *      This procedure is invoked to process the "tsv::lset" command.
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
SvLsetObjCmd(
    void *arg,
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[]
) {
    Tcl_Obj *lPtr;
    int ret;
    Tcl_Size argc, off;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::lset array key index ?index ...? value
     *          $list lset index ?index ...? value
     */

    ret = Sv_GetContainer(interp, objc, objv, &svObj, &off, 0);
    if (ret != TCL_OK) {
	return TCL_ERROR;
    }
    if (objc < 2 + off) {
	Tcl_WrongNumArgs(interp, off, objv, "index ?index...? value");
	goto cmd_err;
    }

    lPtr = svObj->tclObj;
    argc = objc - off - 1;

    if (!SvLsetFlat(interp, lPtr, argc, (Tcl_Obj**)objv+off,objv[objc-1])) {
	return TCL_ERROR;
    }

    Tcl_SetObjResult(interp, Sv_DuplicateObj(lPtr));

    return Sv_PutContainer(interp, svObj, SV_CHANGED);

 cmd_err:
    return Sv_PutContainer(interp, svObj, SV_ERROR);
}

/*
 *-----------------------------------------------------------------------------
 *
 * DupListObjShared --
 *
 *      Help function to make a proper deep copy of the list object.
 *      This is used as the replacement-hook for list object native
 *      DupInternalRep function. We need it since the native function
 *      does a shallow list copy, i.e. retains references to list
 *      element objects from the original list. This gives us trouble
 *      when making the list object shared between threads.
 *
 * Results:
 *      None.
 *
 * Side effects;
 *      This is not a very efficient implementation, but that's all what's
 *      available to Tcl API programmer. We could include the tclInt.h and
 *      get the copy more efficient using list internals, but ...
 *
 *-----------------------------------------------------------------------------
 */

static void
DupListObjShared(
    Tcl_Obj *srcPtr,           /* Object with internal rep to copy. */
    Tcl_Obj *copyPtr           /* Object with internal rep to set. */
) {
    Tcl_Size i, llen;
    Tcl_Obj *elObj, **newObjList;
    Tcl_Obj *buf[16];

    Tcl_ListObjLength(NULL, srcPtr, &llen);
    newObjList = (llen > 16) ? (Tcl_Obj**)Tcl_Alloc(llen*sizeof(Tcl_Obj *)) : &buf[0];

    for (i = 0; i < llen; i++) {
	Tcl_ListObjIndex(NULL, srcPtr, i, &elObj);
	newObjList[i] = Sv_DuplicateObj(elObj);
    }

    Tcl_SetListObj(copyPtr, llen, newObjList);

    if (newObjList != &buf[0]) {
	Tcl_Free(newObjList);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * SvLsetFlat --
 *
 *  Almost exact copy from the TclLsetFlat found in tclListObj.c.
 *  Simplified in a sense that thread shared objects are guaranteed
 *  to be non-shared.
 *
 *  Actual return value of this procedure is irrelevant to the caller,
 *  and it should be either NULL or non-NULL.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj*
SvLsetFlat(
     Tcl_Interp *interp,    /* Tcl interpreter */
     Tcl_Obj *listPtr,      /* Pointer to the list being modified */
     Tcl_Size indexCount,   /* Number of index args */
     Tcl_Obj **indexArray,
     Tcl_Obj *valuePtr      /* Value arg to 'lset' */
) {
    Tcl_Size i, elemCount, index;
    int result;
    Tcl_Obj **elemPtrs;
    Tcl_Obj *pendingInvalidates[10]; /* Assumed max nesting depth */
    Tcl_Obj **pendingInvalidatesPtr = pendingInvalidates;
    Tcl_Size numPendingInvalidates = 0;


    /*
     * Determine whether the index arg designates a list
     * or a single index.
     */

    if (indexCount == 1 &&
	Tcl_ListObjGetElements(interp, indexArray[0], &indexCount,
		&indexArray) != TCL_OK) {
	/*
	 * Index arg designates something that is neither an index
	 * nor a well formed list.
	 */

	return NULL;
    }

    /*
     * If there are no indices, then simply return the new value,
     * counting the returned pointer as a reference
     */

    if (indexCount == 0) {
	return valuePtr;
    }

    /* Allocate if static array for pending invalidations is too small */
    if (indexCount > (Tcl_Size)(sizeof(pendingInvalidates) /
	    sizeof(pendingInvalidates[0]))) {
	pendingInvalidatesPtr =
	    (Tcl_Obj **)Tcl_Alloc(indexCount * sizeof(*pendingInvalidatesPtr));
    }

    /*
     * Handle each index arg by diving into the appropriate sublist
     */

    for (i = 0; ; ++i) {

	/*
	 * Take the sublist apart.
	 */

	result = Tcl_ListObjGetElements(interp,listPtr,&elemCount,&elemPtrs);
	if (result != TCL_OK) {
	    break;
	}

	/*
	 * Determine the index of the requested element.
	 */

	result = Tcl_GetIntForIndex(interp, indexArray[i], elemCount-1, &index);
	if (result != TCL_OK) {
	    break;
	}

	/*
	 * Check that the index is in range.
	 */

	if ((index < 0) || (index >= elemCount)) {
	    Tcl_SetObjResult(interp,
		    Tcl_NewStringObj("list index out of range", TCL_INDEX_NONE));
	    result = TCL_ERROR;
	    break;
	}

	/*
	 * Remember list of Tcl_Objs that need invalidation of string reps.
	 */
	pendingInvalidatesPtr[numPendingInvalidates] = listPtr;
	++numPendingInvalidates;

	/*
	 * Break the loop after extracting the innermost sublist
	 */

	if (i + 1 >= indexCount) {
	    result = TCL_OK;
	    break;
	}

	listPtr = elemPtrs[index];
    }

    /*
     * At this point listPtr holds the sublist (which could even be the
     * top level list) whose element is to be modified.
     */

    if (result == TCL_OK) {
	result = Tcl_ListObjGetElements(interp,listPtr,&elemCount,&elemPtrs);
	if (result == TCL_OK) {
	    Tcl_DecrRefCount(elemPtrs[index]);
	    elemPtrs[index] = Sv_DuplicateObj(valuePtr);
	    Tcl_IncrRefCount(elemPtrs[index]);
	}
    }

    if (result == TCL_OK) {
	/*
	 * Since modification was successful, we need to invalidate string
	 * representations of all ancestors of the modified sublist.
	 */
	while (numPendingInvalidates > 0) {
	    --numPendingInvalidates;
	    Tcl_InvalidateStringRep(pendingInvalidatesPtr[numPendingInvalidates]);
	}
    }

    if (pendingInvalidatesPtr != pendingInvalidates) {
	Tcl_Free(pendingInvalidatesPtr);
    }

    /* Note return only matters as non-NULL vs NULL */
    return result == TCL_OK ? valuePtr : NULL;
}

/* EOF $RCSfile: threadSvListCmd.c,v $ */

/* Emacs Setup Variables */
/* Local Variables:      */
/* mode: C               */
/* indent-tabs-mode: nil */
/* c-basic-offset: 4     */
/* End:                  */

