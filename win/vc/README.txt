The files in this vc subdirectory may be useful if you have not set
up your TEA (i.e., Cygwin) environment.  They have been contributed
by David Gravereaux <davygrvy@bigfoot.com>
If you have TEA, then ignore these files;
you should be able to run "make" from the thread/win directory

The MSVC++ v5 project files (thread.dsw and thread.dsp) are just for convienience.
The real build instructions are in makefile.vc with the user editable information
contained in config.vc.  Before building the extension, open config.vc in a text
editor and change your paths as appropriate.  You might need to run vcvars32.bat,
contained in the bin directory of Visual C++, to setup environment variables and
paths needed by VC++ tools before invoking nmake from the commandline.

There are 2 ways to build the extension:

  -- From the commandline
     C:\...\thread\win\vc\> nmake -f makefile.vc

  -- or open the workspace file (thread.dsw) into MSDev.

Using the project file invokes makefile.vc anyways, so any edits to makefile.vc
and config.vc affect the IDE build as well.  The only purpose of having the project
files is to make it easy to run the extension in the debugger, jump to build errors,
and open files easier.
