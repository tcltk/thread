
Files in this directory may be useful if you have not set up
your TEA (i.e., MinGW) environment and you're using the MSVC++
from Micro$oft.

To build the extension invoke the following command:

    nmake -f makefile.vc TCLDIR=<path>

You would need to give the <path> of the Tcl distribution where
tcl.h and other needed Tcl files are located.
Please look into the makefile.vc file for more information.

Alternatively, you can open the extension workspace and project
files (thread.dsw and thread.dsp) from within the MSVC++ and
press F7 to build the extension under the control of the MSVC IDE.

-EOF-
