
#
# tpool.tcl --
#
#   Tcl implementation of a thread pool paradigm in pure Tcl using the
#   Tcl threading extension 2.5 (or higer)
#
# Copyright (c) 2002 by Zoran Vasiljevic.
#
# See the file license.terms for usage and copying information.
# -----------------------------------------------------------------------------
# RCS: @(#) $Id: tpool.tcl,v 1.1 2002/11/24 17:40:11 vasiljevic Exp $
#

package require Tcl    8.4
package require Thread 2.5
package provide Tpool  1.0

namespace eval tpool {
    variable afterevent "" ; # Idle timer event for worker threads
    variable result        ; # Stores result from the worker thread
    variable waiter        ; # Waits for an idle worker thread
    variable jobsdone      ; # Accumulates results from worker threads
}

#
# tpool::create --
#
#   Creates instance of a named thread pool.
#
# Arguments:
#   name   Name of the thread pool
#   args   Variable number of key/value arguments, as follows:
#
#          -minworkers  minimum # of worker threads (def = 0)
#          -maxworkers  maximum # of worker threads (def = 4)
#          -workeridle  # of sec worker is idle before exiting (def = 300)
#          -workerinit. script used to initialize new worker thread
#
# Side Effects:
#   Might create many new threads if "-minworkers" option is > 0.
#
# Results:
#   The id of the newly created thread pool.
#

proc tpool::create {name args} {
    
    #
    # Check if the pool is already created
    #

    set tpid [namespace current]::$name
    tsv::eval $tpid {
        if {[tsv::exists $tpid name]} {
            error "thread pool \"$name\" already exists"
        }
        tsv::set $tpid name $name
    }
    if {[llength $args] % 2} {
        error "requires even number of arguments"
    }

    #
    # Setup default pool data.
    #

    tsv::array set $tpid {
         thrworkers  ""
         thrwaiters  ""
         jobcounter  0
         numworkers  0
        -minworkers  0
        -maxworkers  4
        -workeridle  300000
        -workerinit  {source tpool.tcl}
    }

    #
    # Override with user-supplied data
    #

    foreach {arg val} $args {
        switch -- $arg {
            -minworkers -
            -maxworkers {tsv::set $tpid $arg $val}
            -workeridle {tsv::set $tpid $arg [expr {$val*1000}]}
            -workerinit {tsv::append $tpid $arg \n $val}
            default {
                error "unsupported pool option \"$arg\""
            }
        }
    }

    #
    # This command kicks the thread into the event loop.
    # This must be the last command of the init script.
    #

    tsv::append $tpid -workerinit \n thread::wait

    #
    # Start initial (minimum) number of worker threads.
    #

    for {set ii 0} {$ii < [tsv::set $tpid -minworkers]} {incr ii} {
        Worker $tpid
    }

    return $tpid
}

#
# tpool::send --
#
#   Submits the new job to the thread pool. The caller might pass
#   the job in two modes: synchronous and asynchronous.
#   For the synchronous mode, the pool implementation will retain
#   the result of the passed script until the caller collects it 
#   using the thread::wait command.
#   For the asynchronous mode, the result of the script is ignored.
#
# Arguments:
#   args   Variable # of arguments with the following syntax:
#          tpool::send ?-async? tpid script
#
#          where:
#            -async  flag to turn the async operation (ignore result)
#            tpid    the id of the thread pool 
#            script  script to pass to the worker thread for execution
#
# Side Effects:
#   Depends on the passed script.
#
# Results:
#   The id of the posted job. This id is used later on to collect
#   result of the job and set local variables accordingly.
#   For asynchronously posted jobs, the return result is ignored
#   and this function returns zero.
#

proc tpool::send {args} {

    #
    # Parse the command arguments. This command syntax 
    # closely resembles the thread::send command.
    #
    
    set ns [namespace current]
    set usage "wrong \# of args: should be \"\
               [info level 1] ?-async? tpid cmd\""

    switch [llength $args] {
        2 {
            set async 0
            set tpid  [lindex $args 0]
            set cmd   [lindex $args 1]
        }
        3 {
            if {[lindex $args 0] != "-async"} {
                error $usage
            }
            set async 1
            set tpid  [lindex $args 1]
            set cmd   [lindex $args 2]            
        }
        default { error $usage }
    }

    #
    # Find idle (or create new) worker thread. This is relatively
    # a complex issue, since we must honour the limits about number 
    # of allowed worker threads imposed to us by the caller.
    #

    set tid [tsv::lpop $tpid thrworkers]
    if {$tid == "" || [catch {thread::preserve $tid}]} {
        tsv::eval $tpid {
            set tid [tsv::lpop $tpid thrworkers]
            if {$tid == "" || [catch {thread::preserve $tid}]} {
                set tid ""
                if {[tsv::set $tpid numworkers]<[tsv::set $tpid -maxworkers]} {
                    Worker $tpid
                    set tid [tsv::lpop $tpid thrworkers]
                    thread::preserve $tid
                } else {
                    tsv::lpush $tpid thrwaiters [thread::id] end
                }
            }
        }
        if {$tid == ""} {
            variable waiter
            vwait ${ns}::waiter
            set tid $waiter
            thread::preserve $tid
        }
    }

    #
    # Post the command to the worker thread
    #

    if {$async} {
        set j 0
        thread::send -async $tid [list ${ns}::Run $tpid $j $cmd]
    } else {
        set j [tsv::incr $tpid jobcounter]
        thread::send -async $tid [list ${ns}::Run $tpid $j $cmd] ${ns}::result
    }

    return $j
}

#
# tpool::wait --
#
#   Waits for a job sent with "thread::send" to finish.
#
# Arguments:
#   tpid   Name of the pool shared array.
#   jobid  Id of the previously posted job.
#
# Side Effects:
#   Might eventually enter the event loop while waiting
#   for the job result to arrive from the worker thread.
#
# Results:
#   Result of the job. If the job resulted in error, it sets
#   the global errorInfo and errorCode variables accordingly.
#

proc tpool::wait {tpid jobid} {
    
    variable result
    variable jobsdone

    #
    # Look if the job has already been collected.
    # If not, wait for the next job to finish.
    #

    if {[info exists jobsdone($jobid)]} {
        set result $jobsdone($jobid)
        unset jobsdone($jobid)
    } else {
        while {1} {
            vwait [namespace current]::result
            set doneid [lindex $result 0]
            if {$jobid == $doneid} {
                break
            }
            set jobsdone($doneid) $result ; # Not our job; store and wait 
        }
    }

    #
    # Set the result in the current thread.
    #

    if {[lindex $result 1] != 0} {
        eval error [lrange $result 2 end]
    }

    return [lindex $result 2]
}

#
# Private internal procedures.
#

#
# tpool::Worker --
#
#   Creates new worker thread. This procedure must be executed
#   under the tsv lock.
#
# Arguments:
#   tpid  Name of the pool shared array.
#
# Side Effects:
#   Depends on the thread initialization script.
#
# Results:
#   None.
#

proc tpool::Worker {tpid} {

    set tid [thread::create [tsv::set $tpid -workerinit]]
    thread::preserve $tid
    tsv::incr  $tpid numworkers
    tsv::lpush $tpid thrworkers $tid end
}

#
# tpool::Timer --
#
#   This procedure should be executed within the worker thread only.
#   It registers the callback for terminating the idle thread.
#
# Arguments:
#   tpid  Name of the pool shared array.
#
# Side Effects:
#   Thread may eventually exit.
#
# Results:
#   None.
#

proc tpool::Timer {tpid} {

    tsv::eval $tpid {
        if {[tsv::set $tpid  numworkers] > [tsv::set $tpid -minworkers]} {
            
            #
            # We have more workers than needed, so kill this one.
            # We first splice ourselves from the list of active
            # aorkers, adjust the number of workers and release 
            # this thread, which may exit eventually.
            #

            set x [tsv::lsearch $tpid thrworkers [thread::id]]
            tsv::lreplace $tpid thrworkers $x $x
            tsv::incr $tpid numworkers -1
            thread::release
        }
    }
}

#
# tpool::Run --
#
#   This procedure should be executed within the worker thread only.
#   It performs the actual command execution in the worker thread.
#
# Arguments:
#   tpid  Name of the pool shared array.
#   jid   The job id
#   cmd   The command to execute
#
# Side Effects:
#   Many, depending of the passed command
#
# Results:
#   List for passing the evaluation result and status back.
#

proc tpool::Run {tpid jid cmd} {

    #
    # Cancel the idle timer callback, if any.
    #

    variable afterevent
    if {$afterevent != ""} {
        after cancel $afterevent
    }
    
    #
    # Evaluate passed command and build the result list.
    #

    set code [catch {uplevel \#0 $cmd} ret]
    if {$code == 0} {
        set res [list $jid 0 $ret]
    } else {
        set res [list $jid $code $ret $::errorInfo $::errorCode]
    }

    #
    # Check to see if any caller is waiting to be serviced.
    # If yes, pass him our thread id and kick it.
    # If no, just push us to the list of the idle workers.
    #
    
    set ns [namespace current]

    tsv::eval $tpid {
        set waiter [tsv::lpop $tpid thrwaiters]
        if {$waiter != ""} {
            thread::send -async $waiter [subst {
                set ${ns}::waiter [thread::id]
            }]
        } else {
            tsv::lpush $tpid thrworkers [thread::id] end
        }
    }

    #
    # Release the thread. If this turns out to be 
    # the last refcount held, don't bother to do
    # any more work, since thread will soon exit.
    #

    if {[thread::release] <= 0} {
        return $res
    }

    #
    # Register the idle timer again.
    #

    set afterevent [after [tsv::set $tpid -workeridle] [subst {
        ${ns}::Timer $tpid
    }]]

    return $res
}

# EOF $RCSfile: tpool.tcl,v $

# Emacs Setup Variables
# Local Variables:
# mode: Tcl
# indent-tabs-mode: nil
# tcl-basic-offset: 4
# End:

