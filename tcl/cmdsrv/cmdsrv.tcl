
#
# cmdsrv.tcl --
#
#   Simple socket command server. Supports many simultaneous sessions.
#   Works in thread mode with each new connection receiving the new thread.
#   Has idle timer on per-thread basis.
#  
#   Usage:
#      cmdsrv::create port ?-idletimer value? ?-initcmd cmd?
# 
#      port         Tcp port where the server listens
#      -idletimer   # of ms to idle before tearing down socket
#                   (default: 300000 = 5 minutes)
#      -initcmd     script to evaluate in new command interpreter at start
#                   (default: empty)
#   Example:
#
#      % cmdsrv::create 5000 -idletimer 60000
#      % vwait forever
#
#      Starts the server on the port 5000, sets idle timer
#      to 1 minute
#
#   See the file "license.terms" for information on usage and
#   redistribution of this file, and for a DISCLAIMER OF ALL WARRANTIES.
# -----------------------------------------------------------------------------
# RCS: @(#) $Id: cmdsrv.tcl,v 1.1 2002/11/24 22:33:53 vasiljevic Exp $
#

# package require Thread 2.5
# package provide Cmdsrv 1.0

load ./libthread2.5.so

namespace eval cmdsrv {
    variable data
}

#
# cmdsrv::create --
#
#	Start the server on the given Tcp port.
#
# Arguments:
#   port   Port where the server is listening
#   args   Variable number of arguments
#
# Side Effects:
#	None.
#
# Results:
#	None.
#

proc cmdsrv::create {port args} {

    variable data

    if {[llength $args] % 2} {
        error "wrong \# arguments, should be: key1 val1 key2 val2..."
    }

    #
    # Setup default pool data.
    #

    array set data {
        -initcmd   {source cmdsrv.tcl}
        -idletimer 300000
    }
    
    #
    # Override with user-supplied data
    #

    foreach {arg val} $args {
        switch -- $arg {
            -idletimer {set data($arg) [expr {$val*1000}]}
            -initcmd   {append data($arg) \n $val}
            default {
                error "unsupported pool option \"$arg\""
            }
        }
    }

    #
    # Start the server on the given port. Note that we wrap
    # the actual accept with a helper after/idle callback.
    # This is a workaround for a well-known Tcl bug.
    #
    
    socket -server [namespace current]::_Accept $port
}

proc cmdsrv::_Accept {s ipaddr port} {
    after idle [list [namespace current]::Accept $s $ipaddr $port]
}

proc cmdsrv::Accept {s ipaddr port} {

    variable data
    
    #
    # Configure socket for sane operation
    #
    
    fconfigure $s -blocking 0 -buffering none -translation {auto crlf}
    
    #
    # Emit the prompt
    #
    
    puts -nonewline $s "% "

    #
    # Create worker thread and transfer socket ownership
    #
 
    set tid [thread::create [append data(-initcmd) \n thread::wait]]
    thread::transfer $tid $s ; # This flushes the socket as well

    #
    # Start event-loop in the remote thread
    # and start the idle timer
    #

    thread::send -async $tid [subst {
        array set [namespace current]::data {[array get data]}
        fileevent $s readable {[namespace current]::Read $s}
        proc exit args {[namespace current]::SockDone $s}
        [namespace current]::StartIdleTimer $s
    }]
}

proc cmdsrv::Read {s} {

    variable data

    StopIdleTimer $s

    #
    # Cover client closing connection
    #

    if {[eof $s] || [catch {gets $s line} readCount]} {
        return [SockDone $s]
    }

    #
    # Cover some strange cases
    #
    
    if {$readCount == -1} {
        return [StartIdleTimer $s]
    }

    #
    # Construct command line to eval
    #

    append data(cmd) $line

    if {[info complete $data(cmd)] == 0} {
        puts -nonewline $s "> "
        return [StartIdleTimer $s]
    }

    #
    # Run the command
    #

    if {[string length $data(cmd)]} {
        catch {uplevel \#0 $data(cmd)} ret
        if {[info exists data] == 0} {
            return
        } else {
            puts $s $ret
            set data(cmd) ""
        }
    }

    puts -nonewline $s "% "
    StartIdleTimer $s
}

proc cmdsrv::SockDone {s} {

    catch {close $s}
    thread::release
}

proc cmdsrv::StopIdleTimer {s} {

    variable data

    if {[info exists data(idleevent)]} {
        after cancel $data(idleevent)
        unset data(idleevent)
    }
}

proc cmdsrv::StartIdleTimer {s} {

    variable data

    set data(idleevent) \
        [after $data(-idletimer) [list [namespace current]::SockDone $s]]
}

################################# End of file ################################
