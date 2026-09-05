# ninit

### small init

compiles '/etc/ninit.d' into a binary dependency graph. pid 1 mmaps it and runs the services in dependency order, as many in parallel as possible

boot with 'init=/usr/sbin/ninit'. 'ninit_graph=/path' on the kernel command line picks another graph file. it mounts /proc /sys /dev (devtmpfs) /run /dev/pts and /dev/shm itself if they aren't mounted already

### service files

one file per service in '/etc/ninit.d' named after the service. it is a bash script. directives are '#%key: value' comments and must come before the first command. a directive after code is an error

```
#!/bin/bash
#%type: daemon            oneshot (default), daemon, target (default if empty)
#%depon: fs, udev         services that must be complete first
#%depof: multi-user       services that must wait for this one
#%onfail: stop            warn, stop, shell
#%restart: always         daemon only: respawn it whenever it exits (default no)
#%notify: 3               daemon only: ready once it writes a newline to this fd
```

examples are in docs/ninit.d

types:
- oneshot: complete when it exits 0
- daemon: complete when it writes a newline to its notify fd, or right after spawn if it has none. exiting later is logged, and respawned if 'restart: always'
- target: no commands. complete when its dependencies are

a failed service is retried once, then 'onfail' decides. without 'onfail' the policy is inferred from how much of the graph depends on it

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

```
make
make USE="quiet busybox"
sudo make install
```
USE= quiet: only WARN/FAIL on the console, busybox: try /bin/busybox first for the emergency shell

### ninitctl

```
ninitctl init            # recompile or compile /etc/ninit.d into /etc/ninit.d/depgraph
ninitctl show -v         # print the compiled graph and scripts
ninitctl add|del NAME... # move services between /etc/ninit.d and /etc/ninit.d/unused
```

ninit checks the depgraph format version exactly. re-run 'ninitctl init' after upgrading ninit and before rebooting. an older graph is refused with 'version mismatch' and the boot lands in the emergency shell

### shutting down

ninit sends SIGTERM to everything. waits 5s, sends SIGKILL, syncs, remounts filesystems read-only. here:

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

if the graph is missing or corrupt, or a service with 'onfail: shell' fails twice, a root shell runs on the console with the terminal reset to a sane state and is respawned when it exits. '/bin/sh', or busybox when built with it

### contact

if you have any questions contact me: ninit@nburch.org

### license

GNU General Public Licence v3.0
