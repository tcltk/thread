
Thread extension supports two build scenarios under Windows:


o. Microsoft MSVC++ build:
--------------------------

You should use the project and nmake files for the MSVC++ in
the vc/ directory. You'll have to edit those files according 
to the vc/README.txt file. 
Thanks to David Gravereaux for providing them.

This is the preferred way of building the extension for the
Windows platform. It works out of the box and somebody says
that MSVC compiler produces better/faster code. Maybe true.


o. Cygwin/MinGW builds:
-----------------------

The extension can be compiled under Windows using either 
MinGW (http://www.mingw.org) or The Cygwin environment. 
(http://www.cygwin.com). Note that the Cygwin-supplied gcc
compiler is not supported. You should use the gcc compiler
as provided with the MinGW environment.

Please note that Cygwin/MinGW builds are working only for 
Tcl 8.4b1 or later. Earlier versions of Tcl core had many 
problems when building under one of those environments.

Using Cygwin/MinGW for building the extension has proved to 
be a tedious task. You first need to have the Tcl core 
compiled with those. This can be an interesting endeavour.
Heh, especially if your're under stress. Just joking...

Having compiled the Tcl core (you lucky guy!) you can now 
compile the extension by running configure/make from this
directory. You can use the CONFIG script to do that.
This should go smoothly, once you got Tcl core compiled ok.

There are also some (pretty promising) efforts in using 
MinGW on Linux to cross-compile the Tcl core for Windows 
under Linux operating system.
In general, you might want to take following steps:

  1. Install the MinGW cross compiler. Use the binaries from
     http://www.stats.ox.ac.uk/pub/Rtools/mingw-cross.tar.bz2
     You need a Linux with at least gnu libc 2.2 for that.

  2. Go to win directory of the tcl8.4b1 source distribution.

  3. Create the "config.site" file with the following contents:

     CC=mingw32-gcc
     AR=mingw32-ar
     RANLIB=mingw32-ranlib
     RC=mingw32-windres

  4. Run "CONFIG_SITE=config.site ./configure --enable-threads"

  5. Run "make"

After some (noisy) warnings you will get a set of *.exe and
*.dll files you can transfer to a Windows machine. 
Don't forget to copy Tcl libraries as well, otherwise Tcl 
shell won't start correctly.

After building the Tcl core, repeat steps 3-5 above from this
directory for building the Threading extension.

-EOF-
