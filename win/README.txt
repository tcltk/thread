
Thread extension supports two build scenarios under Windows:

o. Microsoft MSVC++ build:
You should use the project and nmake files for the MSVC++ in
the vc/ directory. You'll have to edit those files according 
to the vc/README.txt file. 
This is the preferred way of building the extension for the
Windows platform.

o. Cygwin/MinGW builds:
Cygwin build is not supported. You should be using the MinGW
to compile the extension. Look in the CONFIG file in this 
directory for more information.
Using MinGW for building the extension has proved to be a 
rather tedious task. I have succeded in compiling both Tcl
and the threading extension using MinGW 1.07 on Win2k PC but
it is not always simple to get all bits and pieces working.


-EOF-
