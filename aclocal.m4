# Pull in the standard Tcl autoconf macros.
# If you don't have the "config" subdirectory, it is a dependent CVS module.
# Either "cvs -d <root> checkout config" right here, or re-checkout the
# thread module
builtin(include,config/tcl.m4)
