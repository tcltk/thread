/*
 * Implementation of most standard Tcl list processing commands
 * suitable for operation on thread shared (list) variables.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: threadSvListCmd.c,v 1.2 2002/06/17 20:22:55 vasiljevic Exp $
 * ----------------------------------------------------------------------------
 */

#include "threadSvCmd.h"

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

static Tcl_ObjCmdProc SvLpopObjCmd;      /* lpop        */
static Tcl_ObjCmdProc SvLpushObjCmd;     /* lpush       */
static Tcl_ObjCmdProc SvLappendObjCmd;   /* lappend     */
static Tcl_ObjCmdProc SvLreplaceObjCmd;  /* lreplace    */
static Tcl_ObjCmdProc SvLlengthObjCmd;   /* llength     */
static Tcl_ObjCmdProc SvLindexObjCmd;    /* lindex      */
static Tcl_ObjCmdProc SvLinsertObjCmd;   /* linsert     */
static Tcl_ObjCmdProc SvLrangeObjCmd;    /* lrange      */
static Tcl_ObjCmdProc SvLsearchObjCmd;   /* lsearch     */

/*
 * These two are copied verbatim from the tclUtil.c
 * since not found in the public stubs table.
 * I was just too lazy to rewrite them from scratch.
 */

static int SvCheckBadOctal(Tcl_Interp*, char *);
static int SvGetIntForIndex(Tcl_Interp*,  Tcl_Obj *, int, int*);

/*
 * Inefficient list duplicator function which,
 * however, produces deep list copies, unlike
 * the original, which just makes shallow copies.
 */

static void DupListObjShared(Tcl_Obj*, Tcl_Obj*);


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
    Sv_RegisterCommand("lpop",     SvLpopObjCmd,     NULL, NULL);
    Sv_RegisterCommand("lpush",    SvLpushObjCmd,    NULL, NULL);
    Sv_RegisterCommand("lappend",  SvLappendObjCmd,  NULL, NULL);
    Sv_RegisterCommand("lreplace", SvLreplaceObjCmd, NULL, NULL);
    Sv_RegisterCommand("linsert",  SvLinsertObjCmd,  NULL, NULL);
    Sv_RegisterCommand("llength",  SvLlengthObjCmd,  NULL, NULL);
    Sv_RegisterCommand("lindex",   SvLindexObjCmd,   NULL, NULL);
    Sv_RegisterCommand("lrange",   SvLrangeObjCmd,   NULL, NULL);
    Sv_RegisterCommand("lsearch",  SvLsearchObjCmd,  NULL, NULL);

    /*
     * We must use our own (inefficient) list duplicator
     */

    Sv_RegisterObjType(Tcl_GetObjType("list"), DupListObjShared);
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
SvLpopObjCmd (arg, interp, objc, objv)
    ClientData arg;
    Tcl_Interp *interp;
    int objc;
    Tcl_Obj *CONST objv[];
{
    int ret, off, llen, index = 0, iarg = 0;
    Tcl_Obj *elPtr = NULL;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::lpop array key ?index?
     *          $list lpop ?index?
     */

    ret = Sv_Container(interp, objc, objv, &svObj, &off, 0);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }
    if ((objc - off) > 1) {
        Tcl_WrongNumArgs(interp, off, objv, "?index?");
        goto cmd_err;
    }
    if ((objc - off) == 1) {
        iarg = off;
    }
    ret = Tcl_ListObjLength(interp, svObj->tclObj, &llen);
    if (ret != TCL_OK) {
        goto cmd_err;
    }
    if (iarg) {
        ret = SvGetIntForIndex(interp, objv[iarg], llen-1, &index);
        if (ret != TCL_OK) {
            goto cmd_err;
        }
    }
    if (index < 0 || index >= llen) {
        goto cmd_ok; /* Ignore out-of bounds, like Tcl does */
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

 cmd_ok:
    Sv_Unlock(svObj);
    return TCL_OK;

 cmd_err:
    Sv_Unlock(svObj);
    return TCL_ERROR;
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
SvLpushObjCmd (arg, interp, objc, objv)
    ClientData arg;
    Tcl_Interp *interp;
    int objc;
    Tcl_Obj *CONST objv[];
{
    int off, ret, flg, llen, index = 0;
    Tcl_Obj *args[1];
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::lpush array key element ?index?
     *          $list lpush element ?index?
     */

    flg = FLAGS_CREATEARRAY | FLAGS_CREATEVAR;
    ret = Sv_Container(interp, objc, objv, &svObj, &off, flg);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }
    if ((objc - off) < 1) {
        Tcl_WrongNumArgs(interp, off, objv, "element ?index?");
        goto cmd_err;
    }
    ret = Tcl_ListObjLength(interp, svObj->tclObj, &llen);
    if (ret != TCL_OK) {
        goto cmd_err;
    }
    if ((objc - off) == 2) {
        ret = SvGetIntForIndex(interp, objv[off+1], llen, &index);
        if (ret != TCL_OK) {
            goto cmd_err;
        }
        if (index < 0) {
            index = 0;
        } else if (index > llen) {
            index = llen;
        }
    }

    args[0] = objv[off];
    ret = Tcl_ListObjReplace(interp, svObj->tclObj, index, 0, 1, args);
    if (ret != TCL_OK) {
        goto cmd_err;
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
SvLappendObjCmd(arg, interp, objc, objv)
    ClientData arg;
    Tcl_Interp *interp;
    int objc;
    Tcl_Obj *CONST objv[];
{
    int i, ret, flg, off;
    Tcl_Obj *dup;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::lappend array key value ?value ...?
     *          $list lappend value ?value ...?
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
    for (i = off; i < objc; i++) {
        dup = Sv_DuplicateObj(objv[i]);
        ret = Tcl_ListObjAppendElement(interp, svObj->tclObj, dup);
        if (ret != TCL_OK) {
            goto cmd_err;
        }
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
SvLreplaceObjCmd (arg, interp, objc, objv)
    ClientData arg;
    Tcl_Interp *interp;
    int objc;
    Tcl_Obj *CONST objv[];
{
    char *firstArg;
    int argLen, ret, off, llen, first, last, ndel, nargs, i, j;
    Tcl_Obj **args = NULL;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::lreplace array key first last ?element ...?
     *          $list lreplace first last ?element ...?
     */

    ret = Sv_Container(interp, objc, objv, &svObj, &off, 0);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }
    if ((objc - off) < 2) {
        Tcl_WrongNumArgs(interp, off, objv, "first last ?element ...?");
        goto cmd_err;
    }
    ret = Tcl_ListObjLength(interp, svObj->tclObj, &llen);
    if (ret != TCL_OK) {
        goto cmd_err;
    }
    ret = SvGetIntForIndex(interp, objv[off], llen-1, &first);
    if (ret != TCL_OK) {
        goto cmd_err;
    }
    ret = SvGetIntForIndex(interp, objv[off+1], llen-1, &last);
    if (ret != TCL_OK) {
        goto cmd_err;
    }

    firstArg = Tcl_GetStringFromObj(objv[off], &argLen);
    if (first < 0)  {
        first = 0;
    }
    if (llen && first >= llen && strncmp(firstArg, "end", argLen)) {
        Tcl_AppendResult(interp, "list doesn't have element ", firstArg, NULL);
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

    nargs = objc - (off + 2);
    if (nargs) {
        args = (Tcl_Obj**)Tcl_Alloc(nargs * sizeof(Tcl_Obj*));
        for(i = off + 2, j = 0; i < objc; i++, j++) {
            args[j] = Sv_DuplicateObj(objv[i]);
        }
    }

    ret = Tcl_ListObjReplace(interp, svObj->tclObj, first, ndel, nargs, args);
    if (args) {
        Tcl_Free((char*)args);
    }

    Sv_Unlock(svObj);
    return ret;

 cmd_err:
    Sv_Unlock(svObj);
    return TCL_ERROR;
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
SvLrangeObjCmd (arg, interp, objc, objv)
    ClientData arg;
    Tcl_Interp *interp;
    int objc;
    Tcl_Obj *CONST objv[];
{
    int ret, off, llen, first, last, nargs, i, j;
    Tcl_Obj **elPtrs, **args;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::lrange array key first last
     *          $list lrange first last
     */

    ret = Sv_Container(interp, objc, objv, &svObj, &off, 0);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }
    if ((objc - off) != 2) {
        Tcl_WrongNumArgs(interp, off, objv, "first last");
        goto cmd_err;
    }
    ret = Tcl_ListObjGetElements(interp, svObj->tclObj, &llen, &elPtrs);
    if (ret != TCL_OK) {
        goto cmd_err;
    }
    ret = SvGetIntForIndex(interp, objv[off], llen-1, &first);
    if (ret != TCL_OK) {
        goto cmd_err;
    }
    ret = SvGetIntForIndex(interp, objv[off+1], llen-1, &last);
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
    args  = (Tcl_Obj**)Tcl_Alloc(nargs * sizeof(Tcl_Obj*));
    for (i = first, j = 0; i <= last; i++, j++) {
        args[j] = Sv_DuplicateObj(elPtrs[i]);
    }

    Tcl_SetListObj(Tcl_GetObjResult(interp), nargs, args);
    Tcl_Free((char*)args);

 cmd_ok:
    Sv_Unlock(svObj);
    return TCL_OK;

 cmd_err:
    Sv_Unlock(svObj);
    return TCL_ERROR;
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
SvLinsertObjCmd (arg, interp, objc, objv)
    ClientData arg;
    Tcl_Interp *interp;
    int objc;
    Tcl_Obj *CONST objv[];
{
    int off, ret, flg, llen, nargs, index = 0, i, j;
    Tcl_Obj **args;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::linsert array key index element ?element ...?
     *          $list linsert element ?element ...?
     */

    flg = FLAGS_CREATEARRAY | FLAGS_CREATEVAR;
    ret = Sv_Container(interp, objc, objv, &svObj, &off, flg);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }
    if ((objc - off) < 2) {
        Tcl_WrongNumArgs(interp, off, objv, "index element ?element ...?");
        goto cmd_err;
    }
    ret = Tcl_ListObjLength(interp, svObj->tclObj, &llen);
    if (ret != TCL_OK) {
        goto cmd_err;
    }
    ret = SvGetIntForIndex(interp, objv[off], llen, &index);
    if (ret != TCL_OK) {
        goto cmd_err;
    }
    if (index < 0) {
        index = 0;
    } else if (index > llen) {
        index = llen;
    }

    nargs = objc - (off + 1);
    args  = (Tcl_Obj**)Tcl_Alloc(nargs * sizeof(Tcl_Obj*));
    for (i = off + 1, j = 0; i < objc; i++, j++) {
         args[j] = Sv_DuplicateObj(objv[i]);
    }
    ret = Tcl_ListObjReplace(interp, svObj->tclObj, index, 0, nargs, args);
    Tcl_Free((char*)args);
    if (ret != TCL_OK) {
        goto cmd_err;
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
SvLlengthObjCmd (arg, interp, objc, objv)
    ClientData arg;
    Tcl_Interp *interp;
    int objc;
    Tcl_Obj *CONST objv[];
{
    int llen, off, ret;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::llength array key
     *          $list llength
     */

    ret = Sv_Container(interp, objc, objv, &svObj, &off, 0);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }
    ret = Tcl_ListObjLength(interp, svObj->tclObj, &llen);
    Sv_Unlock(svObj);
    if (ret == TCL_OK) {
        Tcl_SetIntObj(Tcl_GetObjResult(interp), llen);
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
SvLsearchObjCmd (arg, interp, objc, objv)
    ClientData arg;
    Tcl_Interp *interp;
    int objc;
    Tcl_Obj *CONST objv[];
{
    int ret, off, listc, mode, imode, ipatt, length, index, match, i;
    char *patBytes;
    Tcl_Obj **listv;
    Container *svObj = (Container*)arg;

    static CONST char *modes[] = {"-exact", "-glob", "-regexp", NULL};
    enum {LS_EXACT, LS_GLOB, LS_REGEXP};

    mode = LS_GLOB;

    /*
     * Syntax:
     *          tsv::lsearch array key ?mode? pattern
     *          $list lsearch ?mode? pattern
     */

    ret = Sv_Container(interp, objc, objv, &svObj, &off, 0);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }
    if ((objc - off) == 2) {
        imode = off;
        ipatt = off + 1;
    } else if ((objc - off) == 1) {
        imode = 0;
        ipatt = off;
    } else {
        Tcl_WrongNumArgs(interp, off, objv, "?mode? pattern");
        goto cmd_err;
    }
    if (imode) {
        ret = Tcl_GetIndexFromObj(interp, objv[imode], modes, "search mode",
                0, &mode);
        if (ret != TCL_OK) {
            goto cmd_err;
        }
    }
    ret = Tcl_ListObjGetElements(interp, svObj->tclObj, &listc, &listv);
    if (ret != TCL_OK) {
        goto cmd_err;
    }

    index = -1;
    patBytes = Tcl_GetStringFromObj(objv[ipatt], &length);

    for (i = 0; i < listc; i++) {
        match = 0;
        switch (mode) {
        case LS_GLOB:
            match = Tcl_StringMatch(Tcl_GetString(listv[i]), patBytes);
            break;

        case LS_EXACT: {
            int elemLen;
            char *bytes = Tcl_GetStringFromObj(listv[i], &elemLen);
            if (length == elemLen) {
                match = (memcmp(bytes, patBytes, (size_t)length) == 0);
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

    Tcl_SetIntObj(Tcl_GetObjResult(interp), index);

    Sv_Unlock(svObj);
    return TCL_OK;

 cmd_err:
    Sv_Unlock(svObj);
    return TCL_ERROR;
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
SvLindexObjCmd (arg, interp, objc, objv)
    ClientData arg;
    Tcl_Interp *interp;
    int objc;
    Tcl_Obj *CONST objv[];
{
    Tcl_Obj **elPtrs;
    int ret, off, llen, index;
    Container *svObj = (Container*)arg;

    /*
     * Syntax:
     *          tsv::lindex array key index
     *          $list lindex index
     */

    ret = Sv_Container(interp, objc, objv, &svObj, &off, 0);
    if (ret != TCL_OK) {
        return TCL_ERROR;
    }
    if ((objc - off) != 1) {
        Tcl_WrongNumArgs(interp, off, objv, "index");
        goto cmd_err;
    }
    ret = Tcl_ListObjGetElements(interp, svObj->tclObj, &llen, &elPtrs);
    if (ret != TCL_OK) {
        goto cmd_err;
    }
    ret = SvGetIntForIndex(interp, objv[off], llen-1, &index);
    if (ret != TCL_OK) {
        goto cmd_err;
    }
    if (index >= 0 && index < llen) {
        Tcl_SetObjResult(interp, Sv_DuplicateObj(elPtrs[index]));
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
DupListObjShared(srcPtr, copyPtr)
    Tcl_Obj *srcPtr;            /* Object with internal rep to copy. */
    Tcl_Obj *copyPtr;           /* Object with internal rep to set. */
{
    int i, llen;
    Tcl_Obj *elObj, **newObjList;

    Tcl_ListObjLength(NULL, srcPtr, &llen);
    newObjList = (Tcl_Obj**)Tcl_Alloc(llen*sizeof(Tcl_Obj*));

    for (i = 0; i < llen; i++) {
        Tcl_ListObjIndex(NULL, srcPtr, i, &elObj);
        newObjList[i] = Sv_DuplicateObj(elObj);
    }

    Tcl_SetListObj(copyPtr, llen, newObjList);
    Tcl_Free((char*)newObjList);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvCheckBadOctal --
 *
 *  Exact copy from the TclCheckBadOctal found in tclUtil.c
 *  since this is not in the stubs table.
 *
 *-----------------------------------------------------------------------------
 */

static int
SvCheckBadOctal(interp, value)
    Tcl_Interp *interp;     /* Interpreter to use for error reporting.
                             * If NULL, then no error message is left
                             * after errors. */
    char *value;            /* String to check. */
{
    register char *p = value;

    /*
     * A frequent mistake is invalid octal values due to an unwanted
     * leading zero. Try to generate a meaningful error message.
     */

    while (isspace((unsigned char)(*p))) { /* INTL: ISO space. */
        p++;
    }
    if (*p == '+' || *p == '-') {
        p++;
    }
    if (*p == '0') {
        while (isdigit((unsigned char)(*p))) { /* INTL: digit. */
            p++;
        }
        while (isspace((unsigned char)(*p))) { /* INTL: ISO space. */
            p++;
        }
        if (*p == '\0') {
            /* Reached end of string */
            if (interp != NULL) {
                Tcl_AppendResult(interp, " (looks like invalid octal number)",
                        (char *) NULL);
            }
            return 1;
        }
    }
    return 0;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SvGetIntForIndex --
 *
 *  Exact copy from the TclGetIntForIndex found in tclUtil.c
 *  since this is not in the stubs table.
 *
 *-----------------------------------------------------------------------------
 */

static int
SvGetIntForIndex(interp, objPtr, endValue, indexPtr)
    Tcl_Interp *interp;     /* Interpreter to use for error reporting.
                             * If NULL, then no error message is left
                             * after errors. */
    Tcl_Obj *objPtr;        /* Points to an object containing either
                             * "end" or an integer. */
    int endValue;           /* The value to be stored at "indexPtr" if
                             * "objPtr" holds "end". */
    int *indexPtr;          /* Location filled in with an integer
                             * representing an index. */
{
    char *bytes;
    int length, offset;

    bytes = Tcl_GetStringFromObj(objPtr, &length);

    if ((*bytes != 'e')
        || (strncmp(bytes, "end",(size_t)((length > 3) ? 3 : length)) != 0)) {
        if (Tcl_GetIntFromObj(NULL, objPtr, &offset) != TCL_OK) {
            goto intforindex_error;
        }
        *indexPtr = offset;
        return TCL_OK;
    }
    if (length <= 3) {
        *indexPtr = endValue;
    } else if (bytes[3] == '-') {
        /*
         * This is our limited string expression evaluator
         */
        if (Tcl_GetInt(interp, bytes+3, &offset) != TCL_OK) {
            return TCL_ERROR;
        }
        *indexPtr = endValue + offset;
    } else {
  intforindex_error:
        if (interp != NULL) {
            Tcl_AppendStringsToObj(Tcl_GetObjResult(interp), "bad index \"",
                    bytes, "\": must be integer or end?-integer?",(char*)NULL);
            SvCheckBadOctal(interp, bytes);
        }
        return TCL_ERROR;
    }
    return TCL_OK;
}

/* EOF $RCSfile: threadSvListCmd.c,v $ */

/* Emacs Setup Variables */
/* Local Variables:      */
/* mode: C               */
/* indent-tabs-mode: nil */
/* c-basic-offset: 4     */
/* End:                  */

