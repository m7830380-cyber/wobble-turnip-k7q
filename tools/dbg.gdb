set pagination off
set libthread-db-search-path /nonexistent
set auto-load off
handle SIGSEGV stop nopass
handle SIGBUS stop nopass
handle SIGILL stop nopass
handle SIGABRT stop nopass
handle SIGPIPE nostop noprint pass
handle SIGTRAP nostop print nopass
break run_main.c:44
run
continue
bt 30
info registers
x/8i $pc-16
info threads
thread apply all bt 10
