set pagination off
set libthread-db-search-path /nonexistent
set auto-load off
handle SIGSEGV stop nopass
handle SIGBUS stop nopass
handle SIGILL stop nopass
handle SIGABRT stop nopass
handle SIGPIPE nostop noprint pass
break run_main.c:44
run
break new_do_write
commands
silent
printf "WRITE f=%p n=%d\n", $x0, $x2
bt 5
continue
end
continue
bt 30
info registers
x/8i $pc-16
info threads
thread apply all bt 10
