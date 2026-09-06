# ninit

### small init

compiles '/etc/ninit.d' into a binary dependency graph. pid 1 mmaps it and runs the services in dependency order, as many in parallel as possible

boot with 'init=/usr/sbin/ninit'. 'ninit_graph=/path' on the kernel command line picks another graph file. it mounts /proc /sys /dev (devtmpfs, and does the dev symlinks) /run /dev/pts and /dev/shm itself if they aren't mounted already

### service files

one file per service in '/etc/ninit.d' named after the service. it is a bash script. directives are '#%key: value' comments and must come before the first command. a directive after code is an error

```
#!/bin/bash
#%type: daemon            oneshot (default), daemon, target (default if empty)
#%depon: fs, udev         services that must be complete first
#%depof: multi-user       services that must wait for this one
#%onfail: stop            warn, stop, shell
#%restart: always         daemon only: respawn it after it has reported ready
#%notify: 3               daemon only: ready once it writes a newline to this fd
#%start-timeout: 90s      how long it may take to become ready (default 90s)
#%stop-timeout: 5s        how long it gets after SIGTERM before SIGKILL (5s)
#%start-tries: 2          attempts before onfail decides (oneshot 1, daemon 2)
#%start-delay: 0s         wait this long between those attempts (default none)
#%deps: uptime            uptime (default) or order, see below
```

durations take `ms`, `s`, `m` or `h`, or a bare count of milliseconds

'start-tries' is the startup budget. 'restart' is supervision, and only begins
once a daemon has reported ready. they are separate on purpose: a daemon that
keeps dying before it is ready is a broken daemon, not one to respawn forever

'deps: uptime' means a dependent may only start while this service is up. if it
completes and then dies, anything below it that has not started yet is cut, and
waits instead if the service is set to restart. 'deps: order' is the looser
reading: the dependent only needs this service to have started once

examples are in docs/ninit.d

types:
- oneshot: complete when it exits 0
- daemon: complete when it writes a newline to its notify fd, or right after spawn if it has none. exiting later is logged, and respawned if 'restart: always'
- target: no commands. complete when its dependencies are

a daemon must run in the foreground: 'exec' the real binary with whatever flag stops it daemonising ('-n', '--nofork', '--foreground'). the process pid 1 starts is the service, and its exit is the service exiting. a script that forks and returns is reported complete and then immediately treated as dead

a readiness probe belongs in a background subshell of the same script, which writes the newline and exits while the main shell execs the daemon. docs/ninit.d/dbus and docs/ninit.d/udev do this

a oneshot runs once and a daemon is tried twice, then 'onfail' decides; both are
set per service with 'start-tries'. a oneshot is not repeated by default because
repeating a script repeats its side effects. without 'onfail' the policy is
'stop' when anything depends on the service and 'warn' when nothing does; it
never depends on how large the rest of the graph is

a daemon with no 'notify' is only known to have started, not to be ready, so
'ninitctl init' warns when such a daemon has dependents. it is held until its
shell has actually exec'd, so a missing interpreter is caught, but nothing can
tell pid 1 that the program inside the script came up

services are placed in their own cgroup under '/sys/fs/cgroup/ninit.services'
and killed with 'cgroup.kill', so a descendant that calls setsid() cannot walk
out of the way the process group alone would allow. without cgroup v2 it falls
back to the process group and says so

scripts are compiled as programs for '/bin/bash' and there is no fallback to
another shell: the same text under a different shell is a different program.
build with 'make NINIT_SHELL=/bin/dash' to change the interpreter

service info:
- they run with stdin on /dev/null and stdout and stderr through pid 1
- they run with only 'PATH' 'HOME=/' and 'TERM=linux'
- they run in their own session with no controlling terminal

console output is prefixed with the service name. the last KiB is printed on failure

a shell on the console has to take the tty itself:

```
#%type: daemon
exec setsid -c bash -i < /dev/console > /dev/console 2>&1
```

or use a real getty

### building it

warning: the makefile builds with -march=native so if you are going to distribute a binary remove it.

```
make
make USE="quiet busybox"
sudo make install
```
USE= quiet: only WARN/FAIL on the console, busybox: try /bin/busybox first for
the emergency shell, authshell: run sulogin for recovery when root has a usable
password hash, falling back to a plain shell when it does not

### ninitctl

```
ninitctl init            # recompile or compile /etc/ninit.d into /etc/ninit.d/depgraph
ninitctl init -n         # check everything, write nothing
ninitctl show -v         # print the compiled graph and scripts
ninitctl add|del NAME... # move services between /etc/ninit.d and /etc/ninit.d/unused
```

'init' checks each script with 'bash -n' before publishing, so a syntax error is
caught at build time rather than at boot. '--no-check' skips it. one exclusive
lock covers reading the sources and publishing, and 'add' and 'del' take the
same lock, so a build always sees one coherent revision of the directory. the
graph is written no more readable than the least readable service it contains

the running system is controlled through pid 1 over '/run/ninit/control',
which is root-only:

```
ninitctl status [NAME]   # what pid 1 is running
ninitctl start NAME      # start it, or retry it after a failure
ninitctl stop NAME       # stop it and keep it stopped
ninitctl restart NAME    # stop it, then start it again
ninitctl resume          # retry every failed and skipped service
```

'stop' records the desired state before it signals anything, and only answers
once the service has actually gone. pid 1 reads its graph once, at boot: a
rebuilt graph takes effect on the next boot, and 'resume' is how you retry
services with the graph already loaded

the depgraph format is version 7. ninit checks it exactly. re-run 'ninitctl init' after upgrading ninit and before rebooting. an older graph is refused with 'version mismatch' and the boot lands in the emergency shell

### shutting down

services are stopped in dependency order: nothing is signalled until everything
that depends on it has gone, and independent branches stop in parallel. each
service gets its own 'stop-timeout' before SIGKILL. after that ninit sends
SIGTERM to whatever is left, waits 5s, sends SIGKILL, syncs, and remounts
filesystems read-only. a filesystem that will not go read-only is unmounted
properly if it can be; a lazy detach is a last resort and says so

```
kill -TERM 1   # reboot (also ctrl-alt-del)
kill -USR2 1   # poweroff
kill -USR1 1   # halt
```

busybox 'reboot' 'poweroff' and 'halt' send these

'tools/shutdown.c' builds one binary 'ninit-shutdown' that answers to 'shutdown' 'poweroff' 'halt' 'reboot' and 'telinit' by looking at 'argv[0]' the way sysvinit and busybox do:
- 'reboot' is 'shutdown -r'. 'poweroff' is 'shutdown -h'. 'halt' is 'shutdown -H'. 'telinit' takes only 0 and 6
- TIME is 'now' '+MINUTES' or 'HH:MM'. a delayed shutdown waits in the foreground and ctrl-c cancels it
- '-f' skips pid 1 and calls 'reboot(2)' after a sync, for when pid 1 is wedged
- root signals pid 1 directly. an unprivileged caller cannot, so the tool asks elogind over D-Bus through 'dbus-send' and lets polkit decide. this is what systemd's own 'poweroff' does
- if pid 1 is not ninit it hands over to the saved 'NAME.old' binary, passing the original name as 'argv[0]'. so these are safe to leave installed on a machine that also boots another init

```
make tools_install   # save the originals as NAME.old, install ours
make tools_uninstall # put the originals back
```

'tools_install' never overwrites an existing 'NAME.old' so running it twice is safe. it rewrites saved symlinks so sysvinit's 'poweroff -> halt' becomes 'poweroff.old -> halt.old' rather than pointing back at ninit's tool. it leaves '/sbin/init' alone so another init on the same machine still boots

a desktop needs none of this directly. KDE's buttons call elogind over D-Bus and elogind runs '/sbin/poweroff' '/sbin/reboot' or '/sbin/halt' itself

### emergency shell

if the graph is missing or corrupt, or a service with 'onfail: shell' runs out of
start-tries, a root shell runs on the console with the terminal reset to a sane
state and is respawned when it exits. '/bin/sh', or busybox when built with it

by default this is an unauthenticated root shell: anyone who can reach the
console can use it. that is the same bargain sysvinit and busybox make, but if
your console is a serial port or a BMC, treat it as a root credential. build
with 'make USE=authshell' to put sulogin in front of it

pid 1 does not block while the shell starts, and 'ninitctl' still works from it

### contact

if you have any questions contact me: ninit@nburch.org

### license

GNU General Public Licence v3.0
