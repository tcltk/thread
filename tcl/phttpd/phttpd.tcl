
#############################################################################t
#
# phttpd.tcl --
#
#    Simple Sample httpd/1.0 server in 250 lines of Tcl.
#    Stephen Uhler / Brent Welch (c) 1996 Sun Microsystems.
#
#    Modified to use namespaces and direct url-to-procedure access (zv).
#    Eh, due to this, and nicer indenting, it's now 150 lines longer :-)
#
#    See the file "license.terms" for information on usage and
#    redistribution of this file, and for a DISCLAIMER OF ALL WARRANTIES.
#
# Rcsid: @(#)$Id: phttpd.tcl,v 1.1 2002/11/24 22:33:54 vasiljevic Exp $
#
##############################################################################

namespace eval phttpd {

    variable Httpd;           # Internal server state and config params
    variable MimeTypes;       # Cache of file-extension/mime-type
    variable HttpCodes;       # Portion of well-known http return codes
    variable ErrorPage;       # Format of error response page in html

    array set Httpd {
        -name    phttpd
        -vers    1.1
        -root    ""
        -block   0
        -bufsize 32768
        -index   index.htm
    }
    array set HttpCodes {
        400  "Bad Request"
        401  "Not Authorized"
        404  "Not Found"
        500  "Server error"
    }
    array set MimeTypes {
        {}   "text/plain"
        .txt "text/plain"
        .htm "text/html"
        .htm "text/html"
        .gif "image/gif"
        .jpg "image/jpeg"
        .png "image/png"
    }
    set ErrorPage {
        <title>Error: %1$s %2$s</title>
        <h1>%3$s</h1>
        <p>Problem in accessing "%4$s" on this server.</p>
        <hr>
        <i>%5$s/%6$s Server at %7$s Port %8$s</i>
    }
}

#---------------------------------------------------------------------------

proc phttpd::server {port args} {

    # Start the server by listening for connections on the desired port.

    variable Httpd 
    set arglen [llength $args]

    if {$arglen} {
        if {$arglen % 2} {
            error "wrong \# arguments, should be: key1 val1 key2 val2..."
        }
        set opts [array names Httpd]
        foreach {arg val} $args {
            if {[lsearch $opts $arg] == -1} {
                error "unknown option \"$arg\""
            }
            set Httpd($arg) $val
        }
    }

    set Httpd(port) $port
    set Httpd(host) [info hostname]

    socket -server [namespace current]::Accept $port
}

#---------------------------------------------------------------------------

proc phttpd::respond {s status contype data {length 0}} {
    
    puts $s "HTTP/1.0 $status"
    puts $s "Date: [Date]"
    puts $s "Content-Type: $contype"

    if {$length} {
        puts $s "Content-Length: $length" 
    } else {
        puts $s "Content-Length: [string length $data]"
    }

    puts $s ""
    puts $s $data 
}

#---------------------------------------------------------------------------
	
proc phttpd::Accept {newsock ipaddr port} {

    # @c Accept a new connection from the client.

    variable Httpd
    upvar \#0 [namespace current]::Httpd$newsock data

    fconfigure $newsock -blocking $Httpd(-block) \
        -buffersize $Httpd(-bufsize) -translation {auto crlf}

    set data(ipaddr) $ipaddr
    fileevent $newsock readable [list [namespace current]::Read $newsock]
}

#---------------------------------------------------------------------------

proc phttpd::Read {s} {

    # @c Read data from client

    variable Httpd
    upvar \#0 [namespace current]::Httpd$s data

    if {[catch {gets $s line} readCount] || [eof $s]} {
        Log error "client closed connection prematurely: %s" $readCount
        return [SockDone $s]
    }
    if {$readCount == -1} {
        return ;# Insufficient data on non-blocking socket !
    }
    if {![info exists data(state)]} {
        set pat {(POST|GET) ([^?]+)\??([^ ]*) HTTP/1\.[0-9]}
        if {[regexp $pat $line x data(proto) data(url) data(query)]} {
            return [set data(state) mime]
        } else {
            Log error "bad request line: %s" $line
            Error $s 400
            return [SockDone $s]
        }
    }

    # string compare $readCount 0 maps -1 to -1, 0 to 0, and > 0 to 1

    set state [string compare $readCount 0],$data(state),$data(proto)
    switch -- $state {
        "0,mime,GET" - "0,query,POST" {
            Respond $s
        }
        "0,mime,POST" {
            set data(state) query
            set data(query) ""
        }
        "1,mime,POST" - "1,mime,GET" {
            if [regexp {([^:]+):[   ]*(.*)}  $line dummy key value] {
                set data(mime,[string tolower $key]) $value
            }
        }
        "1,query,POST" {
            append data(query) $line
            set clen $data(mime,content-length)
            if {($clen - [string length $data(query)]) <= 0} {
                Respond $s
            }
        }
        default {
            if [eof $s] {
                Log error "unexpected eof; client closed connection"
                return [SockDone $s]
            } else {
                Log error "bad http protocol state: %s" $state
                Error $s 400
                return [SockDone $s]
            }
        }
    }
}

#---------------------------------------------------------------------------

proc phttpd::SockDone {s} {

    # @c Close the connection socket and discard token

    close $s
    unset [namespace current]::Httpd$s
}

#---------------------------------------------------------------------------

proc phttpd::Respond {s} {

    # @c Respond to the query.

    variable Httpd
    upvar \#0 [namespace current]::Httpd$s data

    if {[uplevel \#0 info proc $data(url)] == $data(url)} {

        #
        # Service URL-procedure first
        #

        if {[catch {
            puts $s "HTTP/1.0 200 OK"
            puts $s "Date: [Date]"
            puts $s "Last-Modified: [Date]"
        } err]} {
            Log error "client closed connection prematurely: %s" $err
            return [SockDone $s]
        }

        #
        # Setup links to the http state token and current 
        # socket, so procedures can referr to them w/o
        # passing all stuff over all procedure calls.
        #

        uplevel \#0 set dataptr [namespace current]::Httpd$s
        uplevel \#0 set sock $s

        if {[catch {uplevel \#0 $data(url)]} err]} {
            Log error "%s: %s" $data(url) $err
        }

    } else {

        #
        # Service regular file path next.
        #

        set mypath [Url2File $data(url)]
        if {![catch {open $mypath} i]} {
            if {[catch {
                puts $s "HTTP/1.0 200 OK"
                puts $s "Date: [Date]"
                puts $s "Last-Modified: [Date [file mtime $mypath]]"
                puts $s "Content-Type: [ContentType $mypath]"
                puts $s "Content-Length: [file size $mypath]"
                puts $s ""
                fconfigure $s -translation binary -blocking $Httpd(-block)
                fconfigure $i -translation binary
                fcopy $i $s
                close $i
            } err]} {
                Log error "client closed connection prematurely: %s" $err
            }
        } else {
            Log error "%s: %s" $data(url) $i
            Error $s 404
        }
    }

    SockDone $s
}

#---------------------------------------------------------------------------

proc phttpd::ContentType {path} {

    # @c Convert the file suffix into a mime type.

    variable MimeTypes

    set type "text/plain"
    catch {set type $MimeTypes([file extension $path])}
    
    return $type
}

#---------------------------------------------------------------------------

proc phttpd::Error {s code} {

    # @c Emit error page.

    variable Httpd
    variable HttpCodes
    variable ErrorPage

    upvar \#0 [namespace current]::Httpd$s data

    append data(url) ""
    set msg \
        [format $ErrorPage     \
             $code             \
             $HttpCodes($code) \
             $HttpCodes($code) \
             $data(url)        \
             $Httpd(-name)     \
             $Httpd(-vers)     \
             $Httpd(host)      \
             $Httpd(port)      \
            ]
    if {[catch {
        puts $s "HTTP/1.0 $code $HttpCodes($code)"
        puts $s "Date: [Date]"
        puts $s "Content-Length: [string length $msg]"
        puts $s ""
        puts $s $msg
    } err]} {
        Log error "client closed connection prematurely: %s" $err
    }
}

#---------------------------------------------------------------------------

proc phttpd::Date {{seconds 0}} {

    # @c Generate a date string in HTTP format.

    if {$seconds == 0} {
        set seconds [clock seconds]
    }
    clock format $seconds -format {%a, %d %b %Y %T %Z} -gmt 1
}

#---------------------------------------------------------------------------

proc phttpd::Log {reason format args} {
    
    # @c Log an httpd transaction.

    set messg [eval format [list $format] $args]
    set stamp [clock format [clock seconds] -format "%d/%b/%Y:%H:%M:%S"]

    puts stderr "\[$stamp\] $reason: $messg"
}

#---------------------------------------------------------------------------

proc phttpd::Url2File {url} {

    # @c Convert a url into a pathname (this is probably not right)

    variable Httpd

    lappend pathlist $Httpd(-root)
    set level 0

    foreach part [split $url /] {
        set part [CgiMap $part]
        if [regexp {[:/]} $part] {
            return ""
        }
        switch -- $part {
            "." { }
            ".." {incr level -1}
            default {incr level}
        }
        if {$level <= 0} {
            return ""
        }
        lappend pathlist $part
    }

    set file [eval file join $pathlist]

    if {[file isdirectory $file]} {
        return [file join $file $Httpd(-index)]
    } else {
        return $file
    }
}

#---------------------------------------------------------------------------

proc phttpd::CgiMap {data} {

    # @c Decode url-encoded strings

    regsub -all {\+} $data { } data
    regsub -all {([][$\\])} $data {\\\1} data
    regsub -all {%([0-9a-fA-F][0-9a-fA-F])} $data {[format %c 0x\1]} data

    return [subst $data]
}

#---------------------------------------------------------------------------

proc phttpd::QueryMap {query} {

    # @c Decode url-encoded query into key/value pairs
    
    set res [list]

    regsub -all {[&=]} $query { }    query
    regsub -all {  }   $query { {} } query; # Othewise we lose empty values

    foreach {key val} $query {
        lappend res [CgiMap $key] [CgiMap $val]
    }
    return $res
}

################################# End of file ################################
