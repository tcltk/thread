# Microsoft Developer Studio Project File - Name="thread" - Package Owner=<4>
# Microsoft Developer Studio Generated Build File, Format Version 5.00
# ** DO NOT EDIT **

# TARGTYPE "Win32 (x86) External Target" 0x0106

CFG=thread - Win32 Debug
!MESSAGE This is not a valid makefile. To build this project using NMAKE,
!MESSAGE use the Export Makefile command and run
!MESSAGE 
!MESSAGE NMAKE /f "thread.mak".
!MESSAGE 
!MESSAGE You can specify a configuration when running NMAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE 
!MESSAGE NMAKE /f "thread.mak" CFG="thread - Win32 Debug"
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "thread - Win32 Release" (based on "Win32 (x86) External Target")
!MESSAGE "thread - Win32 Debug" (based on "Win32 (x86) External Target")
!MESSAGE 

# Begin Project
# PROP Scc_ProjName ""
# PROP Scc_LocalPath ""

!IF  "$(CFG)" == "thread - Win32 Release"

# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "Release"
# PROP BASE Intermediate_Dir "Release"
# PROP BASE Cmd_Line "NMAKE /f thread.mak"
# PROP BASE Rebuild_Opt "/a"
# PROP BASE Target_File "thread.exe"
# PROP BASE Bsc_Name "thread.bsc"
# PROP BASE Target_Dir ""
# PROP Use_Debug_Libraries 0
# PROP Output_Dir "Release"
# PROP Intermediate_Dir "Release"
# PROP Cmd_Line "NMAKE /f thread.mak"
# PROP Rebuild_Opt "/a"
# PROP Target_File "thread.exe"
# PROP Bsc_Name "thread.bsc"
# PROP Target_Dir ""

!ELSEIF  "$(CFG)" == "thread - Win32 Debug"

# PROP BASE Use_Debug_Libraries 1
# PROP BASE Output_Dir "Debug"
# PROP BASE Intermediate_Dir "Debug"
# PROP BASE Cmd_Line "NMAKE /f thread.mak"
# PROP BASE Rebuild_Opt "/a"
# PROP BASE Target_File "thread.exe"
# PROP BASE Bsc_Name "thread.bsc"
# PROP BASE Target_Dir ""
# PROP Use_Debug_Libraries 1
# PROP Output_Dir "Debug"
# PROP Intermediate_Dir "Debug"
# PROP Cmd_Line "NMAKE -nologo -f makefile.vc"
# PROP Rebuild_Opt "/a"
# PROP Target_File "threadd.dll"
# PROP Bsc_Name "threadd.bsc"
# PROP Target_Dir ""

!ENDIF 

# Begin Target

# Name "thread - Win32 Release"
# Name "thread - Win32 Debug"

!IF  "$(CFG)" == "thread - Win32 Release"

!ELSEIF  "$(CFG)" == "thread - Win32 Debug"

!ENDIF 

# Begin Source File

SOURCE=.\config.vc
# End Source File
# Begin Source File

SOURCE=.\makefile.vc
# End Source File
# Begin Source File

SOURCE=..\generic\thread.h
# End Source File
# Begin Source File

SOURCE=.\thread.rc
# End Source File
# Begin Source File

SOURCE=..\generic\threadCmd.c
# End Source File
# End Target
# End Project
