
Thread extension supports two build scenarios under Windows:


o. Microsoft MSVC++ build:
--------------------------

You should use the project and nmake files for the MSVC++ in
the vc/ directory. You'll have to edit those files according 
to the vc/README.txt file. 
Thanks do David Gravereaux for providing them.

This is the preferred way of building the extension for the
Windows platform. It works out of the box and somebody says
that MSVC compiler produces better/faster code. Maybe true.


o. Cygwin/MinGW builds:
-----------------------

Using Cygwin/MinGW for building the extension has proved to 
be a tedious task. You first need to have the Tcl core 
compiled with those. This can be an interesting endeavour.
Heh, especially if your're under stress. Just joking...

Having compiled the Tcl core (you lucky guy!) you can now 
compile the extension by running configure/make from this
directory. You can use the CONFIG script to do that.
This should go smoothly, once you got Tcl core compiled ok.

