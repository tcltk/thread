#!/usr/local/bin/tclsh
set mydir [file dirname [info script]]
lappend auto_path /usr/local/lib
package req doctools
doctools::new dt
set wd [pwd]
cd $mydir
set code [catch {
    set f [open man.macros]
    set m [read $f]
    close $f
    file rename html htm
    foreach xx {thread tsv tpool} {
        set f [open $xx.man]
        set t [read $f]
        close $f
        foreach {fmt ext dir} {nroff n man html html htm} {
            dt configure -format $fmt
            set o [dt format $t]
            set f [open $dir/$xx.$ext w]
            if {$fmt == "nroff"} {
                puts $f $m
            }
            puts $f $o
            close $f
        }
    }
} err]
file rename htm html
cd $wd
if {$code} {
    error $err
}
exit 0
