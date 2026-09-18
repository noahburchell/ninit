# ninit
### small init for any linux distro
### get it: [gentoo](#if-youre-on-gentoo) | [source](#if-youre-on-something-else)

faster than sysvinit. one file per service, a compiled dependency graph, cgroup
tracking, and a control socket. this is the documentation:

### service files

one per service in `/etc/ninit.d`, named after the service. it's a bash script.
directives are `#%key: value` comments and must come before the first command
a directive after code is an error

```sh
#!/bin/bash
#%type: daemon            oneshot (default), daemon, or target
#%depon: fs, udev         services that must be complete first
#%depof: multi-user       services that must wait for this one
#%onfail: stop            warn, stop, shell
#%restart: always         daemon only: respawn after it has reported ready
#%notify: 3               daemon only: ready once it writes a newline to this fd
#%start-timeout: 90s      how long it may take to become ready (default 90s)
#%stop-timeout: 5s        how long it gets after SIGTERM before SIGKILL (5s)
#%start-tries: 2          attempts before onfail decides (oneshot 1, daemon 2)
#%start-delay: 0s         wait between those attempts (default none)
#%deps: uptime            uptime (default) or order
```

durations take `ms`, `s`, `m` or `h`, or a count of milliseconds

types:
  - oneshot: complete when it exits 0
  - daemon: complete when it writes a newline to its notify fd, or right after spawn if it has none
  - target: no commands, complete when its dependencies are

the rules worth knowing:
  - `start-tries` is the startup budget, `restart` is supervision and only begins once a daemon has reported ready. a daemon that keeps dying before it is ready is broken, not one to respawn forever
  - `deps: uptime` means a dependent may only start while this service is up. `deps: order` means it only needs to have started once. it is set on the service that others depend ON
  - a target goes down if anything behind it dies, and so does everything waiting on it
  - without `onfail` the policy is `stop` when something depends on the service and `warn` when nothing does
  - services get their own cgroup under `/sys/fs/cgroup/ninit.services` and are killed with `cgroup.kill`, so `setsid()` can't escape. without cgroup v2 it falls back to the process group and says so
  - they run with stdin on `/dev/null`, stdout and stderr through ninit, only `PATH` `HOME=/` and `TERM=linux`, in their own session with no controlling terminal. console output is prefixed with the service name and the last KiB is printed on failure

a daemon must run in the foreground: `exec` the real binary with whatever flag
stops it daemonising (`-n`, `--nofork`, `--foreground`). a script that forks and
returns is reported complete and then immediately treated as dead. a readiness
probe belongs in a background subshell of the same script, which writes the
newline while the main shell execs the daemon

a shell on the console has to take the tty itself:

```sh
#%type: daemon
exec setsid -c bash -i < /dev/console > /dev/console 2>&1
```

or use a real getty

examples are in `docs/ninit.d/`. warning: they're written for my machine
my filesystem drivers are `=y` so udev runs after the `/usr/lib/modules` mount

you most likely shouldnt use them like this

### ninitctl

compiling the graph:

```sh
ninitctl init            # compile /etc/ninit.d into /etc/ninit.d/depgraph
ninitctl init -n         # check everything, write nothing
ninitctl show -v         # print the compiled graph and scripts
ninitctl add|del NAME... # move services between /etc/ninit.d and /etc/ninit.d/unused
```

`init` checks each script with `bash -n` before publishing, so a syntax error is
caught at build time rather than at boot (`--no-check` skips it). one exclusive
lock covers reading and publishing, and `add`/`del` take the same lock, so a
build always sees one coherent revision of the directory

talking to the running system, over `/run/ninit/control`, root only:

```sh
ninitctl status [NAME]   # what ninit is running
ninitctl log             # failures ninit recorded
ninitctl start NAME      # start it, or retry it after a failure
ninitctl stop NAME       # stop it and keep it stopped
ninitctl restart NAME    # restart it
ninitctl resume          # retry every failed and skipped service
```

an operation belongs to the service, not to the connection that asked for it, so
killing the client doesn't leave a stop half done. a service counts as stopped
when its whole cgroup is empty, not when its main process has gone

ninit reads its graph once at boot, a rebuilt graph takes effect on the next
boot, and `resume` is how you retry with the graph already loaded. the console
drops output when it can't keep up, so failure lines are also kept in a small
ring that `ninitctl log` reads back — that's where a crash reason lives when the
console lost it, or when built with `--enable-quiet`

the depgraph format is versioned (`NG_VERSION` in `src/ngraph.h`) and checked
exactly. re-run `ninitctl init` after upgrading ninit and before rebooting,
an older graph is refused and the boot lands in the emergency shell

### shutting down

```sh
kill -TERM 1   # reboot (also ctrl-alt-del)
kill -USR2 1   # poweroff
kill -USR1 1   # halt
```

busybox `reboot`, `poweroff` and `halt` send these

services are stopped in dependency order across the whole graph. nothing is
signalled until everything depending on it is gone, independent branches stop in
parallel, and each service gets its own `stop-timeout` before SIGKILL. after that
ninit SIGTERMs whatever is left, waits 5s, SIGKILLs, syncs, and remounts
filesystems read-only.

`tools/shutdown.c` builds one binary that answers to `shutdown`, `poweroff`,
`halt`, `reboot` and `telinit` by looking at `argv[0]` the way sysvinit and
busybox do:

```sh
make tools-install   # save the originals as NAME.old, install ours
make tools-uninstall # put the originals back
```

  - `reboot` is `shutdown -r`, `poweroff` is `shutdown -h`, `halt` is `shutdown -H`. `telinit` takes only 0 and 6
  - TIME is `now`, `+MINUTES` or `HH:MM`. a delayed shutdown waits in the foreground and ctrl-c cancels it
  - `-f` skips ninit and calls `reboot(2)` after a sync, for when ninit is cooked
  - root signals ninit directly. an unprivileged caller asks elogind over D-Bus through `dbus-send` and lets polkit decide, which is what systemd's own `poweroff` does
  - if pid 1 isn't ninit it hands over to the saved `NAME.old` binary, so these are safe to leave installed on a machine that also boots another init

`tools-install` never overwrites an existing `NAME.old`, so running it twice is
safe, and it leaves `/sbin/init` alone. a desktop needs none of this directly

### emergency shell

if the graph is missing or corrupt, or a service with `onfail: shell` runs out of
start-tries, a root shell runs on the console and is respawned when it exits.
`/bin/sh`, or busybox when built with it. ninit doesn't block while it starts and
`ninitctl` still works from it

by default this is an /!\ unauthenticated root shell /!\ the same bargain sysvinit
and busybox make, but if your console is a serial port or a BMC, treat it as a
root credential and configure with `--enable-authshell` to put sulogin in front.

### if you're on gentoo

it's in my overlay:

```sh
emerge --ask app-eselect/eselect-repository
eselect repository add nburch git https://github.com/noahburchell/nburch-overlay.git
emaint sync --repo nburch
emerge --ask sys-apps/ninit
```

the use flags are `o3`, `lto`, `native`, `quiet`, `authshell`, `busybox` and
`debug` and mean the same as the configure options below. ninit picks its own
optimisation flags from those, so `CFLAGS` and `LDFLAGS` in make.conf are ignored
on purpose. then skip to [services](#then)

### if you're on something else

you have to build it, you need a c compiler and make. gcc 14+ or
clang 18+, because the source is c23.

grab the release tarball:

```sh
curl -LO https://github.com/noahburchell/ninit/releases/download/v1.0.0/ninit-1.0.0.tar.xz
tar xf ninit-1.0.0.tar.xz
cd ninit-1.0.0
```

don't use github's own "source code" tarball off the tags page

then the usual lines:

```sh
# 99% of you should use prefix usr, and if you shoudlnt they youd know
./configure --prefix=/usr # whatever options, i use --enable-o3 --enable-lto --enable-native
make -j"$(nproc)"
sudo make install
sudo make tools-install   # optional, see shutting down
```

out of tree builds work and keep the source dir clean:

```sh
mkdir build && cd build && ../configure && make
```

`make V=1` is the plain automake recipe if you'd rather have that. for an
initramfs, link it static with `./configure LDFLAGS=-static`

#### options

```
--enable-o3            -O3 instead of -O2
--enable-lto           link time optimisation
--enable-native        -march=native
--enable-quiet         only WARN and FAIL on the console during boot
--enable-authshell     run sulogin for recovery when root has a usable password
                       hash, plain shell when it doesn't
--with-busybox[=PATH]  try busybox before /bin/sh for the emergency shell
--with-shell=PATH      the interpreter service scripts are written for
--with-shell-name=NAME what to call it
--with-service-dir=DIR where service files live, default /etc/ninit.d
--enable-werror        turn warnings into errors
--enable-debug         -O0 with ASan and UBSan, testing only, never pid 1
```

scripts are compiled as programs for `/bin/bash` and there is no fallback to
another shell: the same text under a different shell is a different program.
`--with-shell=/bin/dash --with-shell-name=dash` to change the interpreter.

### then

write your services into `/etc/ninit.d` (see [above](#service-files)), then:

```sh
sudo ninitctl init -n   # fix anything it reports
sudo ninitctl init      # compile the graph
```

you need to do that again every time you change a service file.

### boot with it

for the first boot, i recomend you do not remove your existing init. so put this in cmdline:

```
init=/sbin/ninit
```

mine:

```
title     Gentoo Linux 7.3 (ninit)
version   7.3-lychee
linux     /vmlinuz-7.3.0-rc2-lychee
options   root=PARTUUID=168ebf8f-0d2d-4ca0-be3a-8216307cb6ba init=/sbin/ninit rootfstype=btrfs rootflags=subvol=@ rw fbcon=nodefer iommu=pt amdgpu.ppfeaturemask=0xfff7ffff
```

### contact

if you have any questions contact me: ninit@nburch.org

### license

GNU General Public Licence v3.0
