# all.tcl --
#
# This file contains a top-level script to run all of the Tcl
# tests.  Execute it by invoking "source all.test" when running tcltest
# in this directory.
#
# Copyright (c) 1998-1999 by Scriptics Corporation.
# All rights reserved.
# 
# RCS: @(#) $Id: all.tcl,v 1.3 2002/03/20 01:31:44 hobbs Exp $

package require tcltest
namespace import -force ::tcltest::*

set ::tcltest::testSingleFile false
set ::tcltest::testsDirectory [file dir [info script]]

# We need to ensure that the testsDirectory is absolute
::tcltest::normalizePath ::tcltest::testsDirectory

puts stdout "Tcl $tcl_patchLevel tests running in interp:  [info nameofexecutable]"
puts stdout "Tests running in working dir:  $::tcltest::testsDirectory"
if {[llength $::tcltest::skip] > 0} {
    puts stdout "Skipping tests that match:  $::tcltest::skip"
}
if {[llength $::tcltest::match] > 0} {
    puts stdout "Only running tests that match:  $::tcltest::match"
}

if {[llength $::tcltest::skipFiles] > 0} {
    puts stdout "Skipping test files that match:  $::tcltest::skipFiles"
}
if {[llength $::tcltest::matchFiles] > 0} {
    puts stdout "Only sourcing test files that match:  $::tcltest::matchFiles"
}

set timeCmd {clock format [clock seconds]}
puts stdout "Tests began at [eval $timeCmd]"


# These tests need to know which is the main thread

# Require the accurate version for the tests so we don't pick up
# older Thread packages by accident
if {$::tcl_version == 8.3} {
    package require Thread 2.1.4
} else {
    package require Thread 2.4
}
set ::tcltest::mainThread [thread::id]

puts stdout "Thread [package provide Thread]"
puts stdout "Mainthread id is $::tcltest::mainThread"

# source each of the specified tests
foreach file [lsort [::tcltest::getMatchingFiles]] {
    set tail [file tail $file]
    puts stdout $tail
    if {[catch {source $file} msg]} {
	puts stdout $msg
    }
}

# cleanup
puts stdout "\nTests ended at [eval $timeCmd]"
::tcltest::cleanupTests 1
return

