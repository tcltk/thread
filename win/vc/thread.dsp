# Microsoft Developer Studio Project File - Name="thread" - Package Owner=<4>
# Microsoft Developer Studio Generated Build File, Format Version 5.00
# ** DO NOT EDIT **

# TARGTYPE "Win32 (x86) External Target" 0x0106

CFG=thread - Win32 Release DLL
!MESSAGE This is not a valid makefile. To build this project using NMAKE,
!MESSAGE use the Export Makefile command and run
!MESSAGE 
!MESSAGE NMAKE /f "thread.mak".
!MESSAGE 
!MESSAGE You can specify a configuration when running NMAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE 
!MESSAGE NMAKE /f "thread.mak" CFG="thread - Win32 Release DLL"
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "thread - Win32 Release DLL" (based on "Win32 (x86) External Target")
!MESSAGE "thread - Win32 Release LIB" (based on "Win32 (x86) External Target")
!MESSAGE "thread - Win32 Release LIB _use msvcrt_" (based on\
 "Win32 (x86) External Target")
!MESSAGE "thread - Win32 Debug DLL" (based on "Win32 (x86) External Target")
!MESSAGE "thread - Win32 Debug LIB" (based on "Win32 (x86) External Target")
!MESSAGE "thread - Win32 Debug LIB _use msvcrtd_" (based on\
 "Win32 (x86) External Target")
!MESSAGE 

# Begin Project
# PROP Scc_ProjName ""
# PROP Scc_LocalPath ""

!IF  "$(CFG)" == "thread - Win32 Release DLL"

# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir ""
# PROP BASE Intermediate_Dir ""
# PROP BASE Cmd_Line ""
# PROP BASE Rebuild_Opt ""
# PROP BASE Bsc_Name ""
# PROP BASE Target_Dir ""
# PROP Use_Debug_Libraries 0
# PROP Output_Dir ""
# PROP Intermediate_Dir ""
# PROP Cmd_Line "NMAKE -nologo -f makefile.vc DEBUG=0"
# PROP Rebuild_Opt "-a"
# PROP Target_File "thread24.dll"
# PROP Bsc_Name ""
# PROP Target_Dir ""

!ELSEIF  "$(CFG)" == "thread - Win32 Release LIB"

# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir ""
# PROP BASE Intermediate_Dir ""
# PROP BASE Cmd_Line ""
# PROP BASE Rebuild_Opt ""
# PROP BASE Bsc_Name ""
# PROP BASE Target_Dir ""
# PROP Use_Debug_Libraries 0
# PROP Output_Dir ""
# PROP Intermediate_Dir ""
# PROP Cmd_Line "NMAKE -nologo -f makefile.vc DEBUG=0 STATIC_BUILD=1"
# PROP Rebuild_Opt "-a"
# PROP Target_File "thread24.lib"
# PROP Bsc_Name ""
# PROP Target_Dir ""

!ELSEIF  "$(CFG)" == "thread - Win32 Release LIB _use msvcrt_"

# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir ""
# PROP BASE Intermediate_Dir ""
# PROP BASE Cmd_Line ""
# PROP BASE Rebuild_Opt ""
# PROP BASE Bsc_Name ""
# PROP BASE Target_Dir ""
# PROP Use_Debug_Libraries 0
# PROP Output_Dir ""
# PROP Intermediate_Dir ""
# PROP Cmd_Line "NMAKE -nologo -f makefile.vc DEBUG=0 STATIC_BUILD=1 NOMSVCRT=0"
# PROP Rebuild_Opt "-a"
# PROP Target_File "thread24x.lib"
# PROP Bsc_Name ""
# PROP Target_Dir ""

!ELSEIF  "$(CFG)" == "thread - Win32 Debug DLL"

# PROP BASE Use_Debug_Libraries 1
# PROP BASE Output_Dir ""
# PROP BASE Intermediate_Dir ""
# PROP BASE Cmd_Line ""
# PROP BASE Rebuild_Opt ""
# PROP BASE Bsc_Name ""
# PROP BASE Target_Dir ""
# PROP Use_Debug_Libraries 1
# PROP Output_Dir ""
# PROP Intermediate_Dir ""
# PROP Cmd_Line "NMAKE -nologo -f makefile.vc DEBUG=1"
# PROP Rebuild_Opt "-a"
# PROP Target_File "thread24d.dll"
# PROP Bsc_Name ""
# PROP Target_Dir ""

!ELSEIF  "$(CFG)" == "thread - Win32 Debug LIB"

# PROP BASE Use_Debug_Libraries 1
# PROP BASE Output_Dir ""
# PROP BASE Intermediate_Dir ""
# PROP BASE Cmd_Line ""
# PROP BASE Rebuild_Opt ""
# PROP BASE Bsc_Name ""
# PROP BASE Target_Dir ""
# PROP Use_Debug_Libraries 1
# PROP Output_Dir ""
# PROP Intermediate_Dir ""
# PROP Cmd_Line "NMAKE -nologo -f makefile.vc DEBUG=1 STATIC_BUILD=1"
# PROP Rebuild_Opt "-a"
# PROP Target_File "thread24sd.lib"
# PROP Bsc_Name ""
# PROP Target_Dir ""

!ELSEIF  "$(CFG)" == "thread - Win32 Debug LIB _use msvcrtd_"

# PROP BASE Use_Debug_Libraries 1
# PROP BASE Output_Dir ""
# PROP BASE Intermediate_Dir ""
# PROP BASE Cmd_Line ""
# PROP BASE Rebuild_Opt ""
# PROP BASE Bsc_Name ""
# PROP BASE Target_Dir ""
# PROP Use_Debug_Libraries 1
# PROP Output_Dir ""
# PROP Intermediate_Dir ""
# PROP Cmd_Line "NMAKE -nologo -f makefile.vc DEBUG=1 STATIC_BUILD=1 NOMSVCRT=0"
# PROP Rebuild_Opt "-a"
# PROP Target_File "thread24xd.lib"
# PROP Bsc_Name ""
# PROP Target_Dir ""

!ENDIF 

# Begin Target

# Name "thread - Win32 Release DLL"
# Name "thread - Win32 Release LIB"
# Name "thread - Win32 Release LIB _use msvcrt_"
# Name "thread - Win32 Debug DLL"
# Name "thread - Win32 Debug LIB"
# Name "thread - Win32 Debug LIB _use msvcrtd_"

!IF  "$(CFG)" == "thread - Win32 Release DLL"

!ELSEIF  "$(CFG)" == "thread - Win32 Release LIB"

!ELSEIF  "$(CFG)" == "thread - Win32 Release LIB _use msvcrt_"

!ELSEIF  "$(CFG)" == "thread - Win32 Debug DLL"

!ELSEIF  "$(CFG)" == "thread - Win32 Debug LIB"

!ELSEIF  "$(CFG)" == "thread - Win32 Debug LIB _use msvcrtd_"

!ENDIF 

# Begin Group "build"

# PROP Default_Filter ""
# Begin Source File

SOURCE=.\config.vc
# End Source File
# Begin Source File

SOURCE=.\makefile.vc
# End Source File
# End Group
# Begin Group "generic"

# PROP Default_Filter ""
# Begin Source File

SOURCE=..\..\generic\tclThread.h
# End Source File
# Begin Source File

SOURCE=..\..\generic\threadCmd.c
# End Source File
# Begin Source File

SOURCE=..\..\generic\threadSpCmd.c
# End Source File
# Begin Source File

SOURCE=..\..\generic\threadSvCmd.c
# End Source File
# Begin Source File

SOURCE=..\..\generic\threadPoolCmd.c
# End Source File
# Begin Source File

SOURCE=..\..\generic\threadSvListCmd.c
# End Source File
# End Group
# Begin Source File

SOURCE=..\thread.rc
# End Source File
# Begin Source File

SOURCE=..\threadWin.c
# End Source File
# End Target
# End Project
