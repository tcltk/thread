
The files in this vc subdirectory may be useful if you have not set
up your TEA (i.e., MinGW) environment.  They have been contributed
by David Gravereaux <davygrvy@pobox.com>

The MSVC++ v5 project files (thread.dsw/thread.dsp) are just for convienience.
The real build instructions are in makefile.vc with the user editable
information contained in config.vc.  Before building the extension, 
open config.vc in a text editor and change your paths as appropriate.

There are 2 ways to build the extension:

  -- From the commandline
     C:\...\thread\win\vc\> nmake -f makefile.vc

    The following macros can be added to the end of the commandline to effect
    the build:
        DEBUG=(0|1)
            Set to one for a symbols build.  Defaults to non-symbols
            when left out.
        STATIC_BUILD=(0|1)
            Will make a static library instead of a dll.
        NOMSVCRT=(0|1)
            Will reference libcmt(d).lib for the runtime when set to one.
            This is zero by default unless STATIC_BUILD is set to one.
        OUT_DIR=<someDir>
            You may specify where the output is placed.  Defaults to
            the build directory.

  -- or open the workspace file (thread.dsw) into MSDev and press F7.


Using the project file invokes makefile.vc anyways, so any edits to 
makefile.vc and config.vc affect the IDE build as well.  
The only purpose of having the project files is to make it easy to run
the extension in the debugger, jump to build errors, and open files easier.

