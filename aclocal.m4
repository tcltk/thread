# Pull in the standard Tcl autoconf macros.
# If you don't have the "tclconfig" subdirectory, it is a dependent CVS module.
# Either "cvs -d <root> checkout tclconfig" right here, or re-checkout the
# thread module
builtin(include,tclconfig/tcl.m4)
