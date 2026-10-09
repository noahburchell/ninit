=====
ninit
=====

-------------------------------
small init for any linux distro
-------------------------------

get it: `gentoo <2.1_>`__ | `source <2.2_>`__

ninit is an init system for linux. each service is a shell script in its own file, with its dependencies and policy declared in comment directives. ninitctl init compiles the service directory into one binary dependency graph. at boot ninit loads that graph once, verifies it, and starts each service as soon as its prerequisites are ready, independent services in parallel. every service runs in its own cgroup. a unix socket exposes the running state to ninitctl

.. contents:: contents
   :depth: 1
   :backlinks: none

1 overview
==========

1.1 components
--------------

==============  ================  ========================================
program         source            function
==============  ================  ========================================
ninit           src/              pid 1. mounts the kernel filesystems, loads the graph, starts and supervises services, serves the control socket, shuts the system down
ninitctl        ctl/              compiles and prints the graph, enables and disables services, sends commands to the running ninit
ninit-shutdown  tools/shutdown.c  shutdown, poweroff, halt, reboot and telinit for a system running ninit
==============  ================  ========================================

all three are installed into sbindir. the manual pages ninit(8), ninitctl(8), ninit-shutdown(8) and ninit.d(5) are installed into mandir unless ninit is configured with ``--disable-man``

1.2 terms
---------

===============  ========================================
term             definition
===============  ========================================
service          one file in the service directory. its name is the filename
prerequisite     a service that must be up before another may start
dependent        a service that has this one as a prerequisite, directly or through other services
root             a service with no prerequisites. roots are started at boot
ready            the event that makes a service up. defined per type in `3.4`_
up               ready, and not since exited or stopped
held             stopped by ninitctl stop. a held service is not started again until ninitctl start or ninitctl restart
graph, depgraph  the compiled form of the service directory, /etc/ninit.d/depgraph by default
===============  ========================================

1.3 requirements
----------------

- linux. configure rejects any other host
- a c23 compiler, gcc 14 or later or clang 18 or later, and make
- bash at /bin/bash, or the interpreter chosen with ``--with-shell``, when the first service starts
- /bin/sh for the emergency shell
- python, lua or perl only for a service file whose #! line names it, see `3.12`_
- cgroup v2 for service containment. forking directly into a cgroup uses CLONE_INTO_CGROUP (linux 5.7). SIGKILL is delivered through cgroup.kill (linux 5.14). without cgroup v2 services are contained by process group only
- optional: hwclock at shutdown, sulogin for ``--with-sulogin``, dbus-send and elogind for ninit-shutdown run without root

1.4 scope
---------

ninit mounts only the kernel filesystems listed in `5.2`_. it does not remount the root filesystem, mount fstab entries, enable swap, set the hostname, load modules or keymaps, run udev, or start gettys. each of these is a service. docs/ninit.d/ contains one complete set, see `3.11`_

2 installation
==============

.. _2.1:

2.1 gentoo
----------

ninit is packaged in the nburch overlay:

.. code:: sh

   emerge --ask app-eselect/eselect-repository
   eselect repository add nburch git https://github.com/noahburchell/nburch-overlay.git
   emaint sync --repo nburch
   emerge --ask sys-apps/ninit

========  =====================
use flag  configure option
========  =====================
hardened  ``--enable-hardened``
quiet     ``--enable-quiet``
sulogin   ``--with-sulogin``
busybox   ``--with-busybox``
debug     ``--enable-debug``
========  =====================

CFLAGS and LDFLAGS from make.conf are used as described in `2.2`_. continue at `2.5`_

.. _2.2:

2.2 from source
---------------

download and unpack the release archive:

.. code:: sh

   curl -LO https://github.com/noahburchell/ninit/releases/download/v1.0.4/ninit-1.0.4.tar.xz
   tar xf ninit-1.0.4.tar.xz
   cd ninit-1.0.4

the "source code" archives github generates for each tag contain no configure script. a git checkout needs ``./autogen.sh`` first, which requires autoconf 2.69 and automake 1.16 or later

build and install:

.. code:: sh

   ./configure --prefix=/usr
   make -j"$(nproc)"
   sudo make install
   sudo make tools-install   # optional, see 7.3

builds out of tree are supported:

.. code:: sh

   mkdir build && cd build && ../configure && make

make prints one short line per compile and link. ``make V=1`` prints the full commands. ``./configure LDFLAGS=-static`` links statically

configure starts from an empty CFLAGS. its optimisation flags, ``-O2`` and link time optimisation, come before the CFLAGS and LDFLAGS given to configure or make, so ``-O3`` or ``-fno-lto`` there take effect. the language standard, the warnings and the hardening flags come after them and cannot be overridden

every build is hardened with ``_FORTIFY_SOURCE=2``, ``-fstack-protector-strong``, ``-fstack-clash-protection``, ``-fPIE -pie``, ``-z relro -z now`` and ``-z noexecstack``. ``-pie`` is left out when LDFLAGS contains ``-static``

2.3 configure options
---------------------

==========================  ========================================
option                      effect
==========================  ========================================
``--enable-hardened``       ``_FORTIFY_SOURCE=3``, ``-fcf-protection=full`` on x86 or ``-mbranch-protection=standard`` on arm64, ``-ftrivial-auto-var-init=zero``, ``-fstrict-flex-arrays=3``, ``-fno-delete-null-pointer-checks`` and ``-fno-strict-overflow``
``--enable-quiet``          only WARN and FAIL lines are printed on the console. ninitctl log is unaffected
``--with-sulogin``          run sulogin in front of the emergency shell, see `8`_
``--with-busybox[=PATH]``   try PATH before /bin/sh for the emergency shell. PATH defaults to /bin/busybox
``--with-shell=PATH``       interpreter for service scripts. default /bin/bash
``--with-shell-name=NAME``  argv[0] for that interpreter. default the basename of PATH
``--with-service-dir=DIR``  service directory. default /etc/ninit.d. the graph is DIR/depgraph
``--disable-man``           do not build or install the manual pages
``--enable-werror``         warnings are errors
``--enable-debug``          ``-O0 -g3`` with ASan and UBSan, without link time optimisation or ``_FORTIFY_SOURCE``. not for use as pid 1
==========================  ========================================

there is no fallback to another shell. ``--with-shell`` also selects the interpreter ninitctl init uses for its syntax check. dash, for example, is ``--with-shell=/bin/dash --with-shell-name=dash``

the interpreter must run ``-c SCRIPT NAME`` with ``$0`` set to NAME, and check a script read from stdin with ``-n``, as sh does. configure runs both and fails if either fails, which rules out lua, python and perl. it skips the check when cross compiling, when PATH is not executable on the build system, and when ``--with-shell-name`` is not the basename of PATH

2.4 make targets
----------------

===============  ========================================
target           effect
===============  ========================================
install          install ninit, ninitctl and ninit-shutdown into sbindir, the manual pages into mandir and the example services into DOCDIR/ninit.d, and create the service directory
tools-install    install, then link shutdown, poweroff, halt, reboot and telinit to ninit-shutdown, see `7.3`_
tools-uninstall  restore the originals saved by tools-install, remove the links that replaced nothing, and remove ninit-shutdown
graph            run the freshly built ninitctl init on the configured service directory
dist             build ninit-VERSION.tar.xz
===============  ========================================

.. _2.5:

2.5 setup
---------

1. write the service files into /etc/ninit.d, see `3`_, or start from the examples, see `3.11`_
2. run ``ninitctl init -n`` and correct everything it reports
3. run ``ninitctl init`` to write the graph
4. add a boot entry with ``init=`` pointing at ninit and keep the existing entry as the default, see `5.1`_
5. after booting, run ``ninitctl log -e`` to list the warnings and failures of the boot

steps 2 and 3 are repeated after every change to a service file

.. _3:

3 service files
===============

3.1 service directory
---------------------

every regular file in the service directory is one service, named after the file. symlinks are followed. ninitctl init skips:

- names beginning with ``.``
- directories and any other file that is not regular
- the reserved names unused, depgraph, depgraph.old and depgraph.tmp
- any file that is itself a compiled graph

a name must be 1 to 255 bytes and must not contain ``/``, ``,``, whitespace or control characters. names ending in ``~`` or enclosed in ``#`` are rejected. an invalid name fails the build

DIR/unused/ holds disabled services and is not read by ninitctl init. ninitctl del and ninitctl add move files into and out of it, see `4.3`_

.. _3.2:

3.2 file format
---------------

a service file is a shell script. its header is every line before the first line that is neither blank nor a comment. a directive is a header line of the form ``#%key: value``. whitespace around the key and the value is ignored

the #! line is an ordinary comment unless it names python, lua or perl, see `3.12`_. otherwise the interpreter is the configured one, /bin/bash by default. the file does not need to be executable. ninitctl init warns when the first line of a file with commands is a #! line naming any other interpreter, directly or through env. ``#!/bin/sh`` draws no warning when the configured shell lexes like sh

ninitctl init stores the script without its comments, indentation or trailing blanks, and with runs of blanks collapsed to one space. text inside quotes and here-documents is stored as written, and so is the rest of the file after a construct it cannot follow exactly, such as a line continuation inside a word. a removed line stays as an empty line, so line numbers in shell errors match the file

the stripper follows sh lexing. it runs when ``--with-shell-name`` is sh, bash, dash, ash, ksh, mksh, oksh, loksh, yash or posh, the shells that lex like sh. for any other shell the file is stored as written. zsh, for one, reads ``(#i)`` as a glob flag where the stripper sees a comment

a #% line after the header is not a directive. ninitctl init warns about it and treats it as a comment. lines inside a quoted string or a here-document are exempt from the warning, and so is a file stored as written

a file containing a NUL byte is rejected

a daemon with a readiness probe:

.. code:: sh

   #!/bin/bash
   #%depon: dbus, udev
   #%type: daemon
   #%notify: 3
   #%restart: always
   #%start-timeout: 10s
   { gdbus wait --system org.freedesktop.login1 && echo >&3; } &
   exec 3>&- /lib/elogind/elogind

3.3 directives
--------------

=============  ==========================  ===============================================  ========================================
directive      value                       default                                          meaning
=============  ==========================  ===============================================  ========================================
type           oneshot, daemon, target     oneshot if the file has commands, target if not  how the service becomes ready, see `3.4`_
depon          service names               none                                             prerequisites of this service
depof          service names               none                                             services this one is a prerequisite of
deps           uptime, order               uptime                                           what the dependents of this service require of it, see `3.5`_
onfail         warn, stop, shell           stop if the service has dependents, warn if not  action once start-tries are exhausted, see `3.6`_
start-tries    1 to 250                    1 for a oneshot, 2 for a daemon                  start attempts before onfail applies
start-delay    duration, at most 65535 ms  0, applied as 10 ms                              delay between start attempts
start-timeout  duration                    90 s                                             time allowed from spawn to ready
stop-timeout   duration                    5 s                                              time allowed from SIGTERM to SIGKILL
restart        always, no                  no                                               daemons only. respawn after an exit once it has been ready, see `3.7`_. ``never`` is accepted for ``no``
notify         3 to 255                    none                                             daemons only. the fd on which the daemon reports readiness
name           the filename                none                                             optional. the build fails if it differs from the filename
=============  ==========================  ===============================================  ========================================

- keys are lowercase and case sensitive
- names in depon and depof are separated by commas, whitespace or both
- depon and depof may be repeated and accumulate. any other repeated directive takes its last value
- a duration is a decimal integer with an optional unit, ms, s, m or h. no unit means milliseconds. zero is rejected. the maximum is 24 h
- an unknown key, a directive without ``:``, or an invalid value fails the build

.. _3.4:

3.4 types and readiness
-----------------------

a service is ready when its dependents may start. readiness makes it up and releases its dependents

=====================  =========  ========================================
type                   commands   ready when
=====================  =========  ========================================
oneshot                required   the script exits with status 0
daemon with notify: N  required   data containing a newline is written to fd N
daemon without notify  required   the interpreter has been executed
target                 forbidden  all of its prerequisites are up
=====================  =========  ========================================

a oneshot stays up after it exits. a daemon is up until its main process exits. a target has no process, and goes down when a prerequisite with deps: uptime goes down

a daemon with notify: N is started with the write end of a pipe on fd N. bytes other than the newline are ignored. if every copy of the write end is closed before a newline arrives, the start fails with ``closed its notify fd before reporting ready``

ninitctl init lists every daemon that has dependents and no notify

a start attempt fails when:

- a oneshot exits nonzero or is killed by a signal
- a daemon exits before it is ready
- a daemon closes its notify fd before it is ready
- the service is not ready within start-timeout of being spawned, in which case it is killed. this applies to oneshots as well

.. _3.5:

3.5 dependencies
----------------

``depon: a`` in service x makes a a prerequisite of x. ``depof: y`` in service x makes x a prerequisite of y. both produce the same edge

a name that matches no service, a service that depends on itself, and a dependency cycle each fail the build. a cycle is printed as ``a -> b -> a``

a service is started as soon as all of its prerequisites are up. roots are started at boot

deps is set on the prerequisite and defines what its dependents require of it:

======  ========================================
deps    meaning
======  ========================================
uptime  dependents may start only while this service is up. while it is down they cannot be started or restarted, and targets that depend on it are down
order   dependents may start once this service has been ready once. later exits do not affect them
======  ========================================

a dependent that is already running is never stopped because a prerequisite went down

.. _3.6:

3.6 start failures
------------------

a failed start attempt kills the service's cgroup and process group. if attempts remain, the service is started again after start-delay, at least 10 ms later. the console shows ``NAME: failed (REASON), retrying (A of T)``

when start-tries attempts have failed, the service is failed. the console shows ``NAME: failed T times (REASON)`` followed by the last 1 KiB of its output, and onfail applies:

======  ========================================
onfail  effect
======  ========================================
warn    nothing beyond the report. valid only for a service with no dependents
stop    every dependent that has not started is marked skipped and will not start. services outside that set continue
shell   as stop, and the emergency shell is started, see `8`_
======  ========================================

the default is stop for a service with dependents and warn for one without. onfail: warn on a service with dependents fails the build

onfail applies only to starting. an exit after the service has been ready is handled as described in `3.7`_

REASON is one of ``exit N``, ``killed by SIGNAME``, ``killed by SIGNAME (core dumped)``, ``timed out and was killed`` or ``closed its notify fd before reporting ready``

ninitctl resume returns every failed and skipped service to pending and starts those that can start, see `6`_

.. _3.7:

3.7 restart
-----------

restart: always supervises a daemon after it has been ready once. before that only start-tries applies, and a daemon that never becomes ready fails like any other service

when a daemon with restart: always exits, it goes down, its cgroup and process group are killed, and it is started again after a delay of 100, 250, 500, 1000 and 2000 ms, then 5000 ms for every further exit. the delay returns to 100 ms once the daemon has stayed up for 10 s. the console shows ``NAME: exited (REASON) after N ms up, restarting in D ms``, followed by the output tail on the first exit of a sequence

a respawned daemon that fails to become ready is killed and started again on the same schedule, without limit. a restart is postponed while a prerequisite with deps: uptime is down, and made when that prerequisite is up again. a held service is not restarted

when a daemon without restart exits after being ready, the console shows ``NAME: exited (REASON) after N ms up`` and the output tail, its cgroup is killed, and if it has deps: uptime, every dependent that has not started is marked skipped

.. _3.8:

3.8 execution environment
-------------------------

each start runs ``/bin/bash -c SCRIPT NAME``, where SCRIPT is the file as stored by ninitctl init, see `3.2`_, so ``$0`` is the service name. a python, lua or perl service runs as in `3.12`_ instead, with the same process attributes. the process has:

- a new session and no controlling terminal
- the cgroup /sys/fs/cgroup/ninit.services/svc-N, where N is the service's index in the graph, the # column of ninitctl show
- stdin on /dev/null, stdout and stderr on a pipe read by ninit
- the notify pipe on fd N if notify: N is set. every other descriptor is closed
- the environment ``PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin``, ``HOME=/``, ``TERM=linux``, and the locale variables from /etc/locale.conf. nothing else is inherited
- an empty signal mask and the default disposition for every signal
- working directory ``/`` and umask 022
- RLIMIT_NOFILE of 1024 soft and 524288 hard, or ninit's hard limit if that is lower
- oom_score_adj 0. ninit itself runs at -1000
- uid 0, gid 0 and every capability

/etc/locale.conf is read once at boot. it holds ``NAME=value`` lines for LANG and the ``LC_*`` variables, with optionally quoted values made of letters, digits, ``_``, ``.``, ``-`` and ``@``, and ``#`` comments. a later assignment overrides an earlier one. an invalid line is skipped with a warning. without the file, or with no locale variable in it, services get ``LANG=C.UTF-8`` and ninitctl init warns

on linux 5.7 and later the process is created inside its cgroup by clone3() with CLONE_INTO_CGROUP. on older kernels it is forked and joins the cgroup before exec. if it cannot join, it prints ``ninit: could not join its cgroup`` on its own output and is contained by its process group only. if /sys/fs/cgroup is not cgroup2, or ninit.services cannot be created in it, every service is contained by process group only and ninit logs this at boot

a signal sent to a service reaches every process in its cgroup and its process group. SIGKILL is delivered through cgroup.kill. on kernels without cgroup.kill, before 5.14, SIGKILL reaches the process group only

a service is stopped when its main process has been reaped and its cgroup is empty. without cgroups it is stopped when its main process has been reaped

processes a oneshot leaves running after it exits remain in its cgroup. they are signalled by ninitctl stop and at shutdown

3.9 output
----------

each line a service writes to stdout or stderr is logged as ``NAME: line`` at NOTE level, see `5.4`_. lines longer than 256 bytes are split

the last 1024 bytes of output are kept per service and printed with the failure messages described in `3.6`_ and `3.7`_

3.10 writing daemons
--------------------

the main process of a daemon must stay in the foreground. exec the daemon binary with its option not to fork, such as ``-n``, ``-D``, ``--nofork`` or ``--foreground``. when the main process exits the service has exited: a script that starts a forking daemon and returns is treated as a daemon that died, and its cgroup, the forked daemon included, is killed

a daemon with no readiness mechanism of its own is given one by a probe in a background subshell of the same script, as in the example in `3.2`_. the subshell writes the newline once the probe succeeds. the main shell closes its own copy of the fd with ``3>&-`` and execs the daemon

a daemon that can report readiness on a descriptor writes to it directly, such as ``dbus-daemon --print-address=3`` with notify: 3

services have no controlling terminal. an interactive shell on the console acquires it explicitly:

.. code:: sh

   #%type: daemon
   exec setsid -c bash -i < /dev/console > /dev/console 2>&1

a getty acquires its terminal itself, as in ``exec agetty --noclear tty1 linux``

.. _3.11:

3.11 examples
-------------

docs/ninit.d/ is a complete service set for a system with udev, dbus and elogind, and one daemon for each common task. make install installs it into DOCDIR/ninit.d/

============================  ========================================
service                       function
============================  ========================================
fs                            if ``/`` is mounted read-only, check it with fsck and remount it read-write
mounts                        check the local filesystems in /etc/fstab with fsck, then mount them. ``_netdev`` entries and network filesystems are skipped
swap                          ``swapon -a``
tmpfiles-dev, tmpfiles        kmod static nodes and the tmpfiles.d entries under /dev, then every other tmpfiles.d entry
udev, udev-trigger            the udev daemon, then an ``add`` event for every existing device
modules-load, sysctl, binfmt  modules-load.d, sysctl.d and binfmt.d
console                       KEYMAP and FONT from /etc/vconsole.conf
hostname                      the name in /etc/hostname
hwclock                       the kernel timezone, from /etc/adjtime
loopback                      brings ``lo`` up
basic                         target. the local system is set up
getty1 to getty6              agetty on tty1 to tty6
dbus, elogind                 the system bus and the login manager
sysklogd                      system log
cronie                        cron
sshd                          ssh server. missing host keys are generated first
chronyd                       ntp client
NetworkManager                network configuration
bluetooth                     bluetoothd
cupsd                         printing
sddm                          display manager
============================  ========================================

fs and mounts fail, and start the emergency shell, when fsck reports errors it could not correct. for ``/`` a correction that requires a reboot counts as a failure

unused/ holds three disabled services:

===========  ========================================
service      function
===========  ========================================
dhcpcd       dhcp client, an alternative to NetworkManager
avahi        mdns and dns-sd
udev-settle  waits until udev has processed every queued event. needed when /etc/fstab names devices that udev rules create, such as lvm or raid volumes. it places itself before mounts with depof, so enabling it requires no change to mounts
===========  ========================================

to use the set, copy it, move the services whose programs are not installed to unused/, and build the graph:

.. code:: sh

   cp -a /usr/share/doc/ninit/ninit.d/. /etc/ninit.d/
   ninitctl del bluetooth cupsd sddm
   ninitctl add udev-settle
   ninitctl init -n

.. _3.12:

3.12 python, lua and perl
-------------------------

the configured shell is the language of service files. python, lua and perl are an extra for the few services better written in one of them. a file whose #! line names one of these interpreters runs under it instead of the shell:

========  ========================================
language  #! interpreter
========  ========================================
python    python, python3, python3.N
lua       lua, luaN.N, luajit and its versioned names
perl      perl, perlN.N
========  ========================================

the interpreter is named directly, as in ``#!/usr/bin/python3``, or through env, as in ``#!/usr/bin/env lua``. ninitctl init resolves it once, when the graph is built, on the running system or below the ``--root`` directory of `4.1`_, and the graph holds its absolute path. ninit never runs env, so the options and NAME=VALUE words before the program, read as gnu and busybox env read them, have no effect. a path in the #! line is kept as written. env is resolved through the PATH services run with, see `3.8`_, with the directory made canonical and the program keeping its own name, so ``env python3`` is /usr/bin/python3 where /usr/sbin links to /usr/bin. an interpreter that does not exist or is not executable fails the build. the #! line of a file without commands, which runs nothing, is not read. the words after the interpreter are separate arguments, as ``env -S`` splits them, and come before the script

the build fails on an argument that keeps the stored script from running. a letter counts inside a cluster of options, as in ``-Bc``:

========  ========================================
language  refused #! arguments
========  ========================================
all       a word that is neither an option nor an option argument, ``-``, ``--``
python    ``-c``, ``-m``, ``-h``, ``-V``, ``--help``, ``--version``, and ``-W``, ``-X`` or ``--check-hash-based-pycs`` without their argument
lua       ``-e``, luajit ``-b``, and ``-l`` or luajit ``-j`` without their argument
perl      ``-e``, ``-E``, ``-x``, ``-c``, ``-h``, ``-u``, ``-v``, ``-V``, ``-d`` without a module, ``-n``, ``-p``, ``-a``, ``-F``, and ``-I`` without its argument
========  ========================================

the header, #! line and directives included, is stored as empty lines and the rest of the file as written. nothing is stripped and a #% line after the header draws no warning. line numbers in interpreter messages match the file. in lua a ``#`` line is valid only in the header. crlf line ends are stored as written, and the three interpreters read them as lf, in strings and here-documents too

each start runs:

========  ===============================================================================  ============================
language  command                                                                          the service name
========  ===============================================================================  ============================
python    ``INTERP -I -B -u ARGS -c SCRIPT NAME``                                          sys.argv is ``['-c', NAME]``
lua       ``INTERP ARGS -e 'io.stdout:setvbuf("line") arg = { [0] = "NAME" }' -e SCRIPT``  arg[0]
perl      ``INTERP ARGS -e '$| = 1; $0 = '\''NAME'\'';' -e '#line 1' -e SCRIPT``           ``$0``
========  ===============================================================================  ============================

``-I`` keeps ``/``, the working directory, off sys.path and ignores the ``PYTHON*`` variables. ``-B`` keeps python from writing ``__pycache__`` directories as root. ``-u``, the lua line buffering and perl's ``$|`` hand output to ninit as it is written, so the log keeps pace with the service and a killed service leaves its last lines in the output tail. perl's ``#line 1`` restores the line numbers the first ``-e`` shifted. stdin is /dev/null and @ARGV is empty, so perl's ``<>`` reads nothing

everything else in `3.8`_ applies: the environment, the descriptors, the notify fd and the cgroup. a daemon without notify is ready once its interpreter has been executed

the syntax check of `4.1`_ compiles python through compile() and lua through load(), neither runs the script. perl has no check that runs nothing, ``perl -c`` runs BEGIN blocks and ``use`` statements as it compiles. one that blocks is killed after 30 s and fails the build. ``--no-check`` skips every check

the interpreter must exist at its path when the service starts. with /usr on its own filesystem, a service whose interpreter is under /usr needs to depend on the service that mounts it. when the exec fails the service prints ``ninit: exec PATH: errno N`` and fails

4 compiling the graph
=====================

ninitctl has configuration commands, described here, and runtime commands, described in `6`_. ``ninitctl help`` prints a summary of both

.. _4.1:

4.1 init
--------

::

   ninitctl init [-d DIR] [-o FILE] [-n] [--no-check] [--root ROOT]

======================  ========================================
option                  effect
======================  ========================================
``-d``, ``--dir DIR``   service directory. default /etc/ninit.d
``-o``, ``--out FILE``  output file. default DIR/depgraph
``-n``, ``--dry-run``   perform every step except writing the output
``--no-check``          skip the syntax check
``--root ROOT``         build for the system whose root directory is ROOT. default DIR ROOT/etc/ninit.d
======================  ========================================

``init`` performs, in order:

1. take an exclusive flock on DIR. ``add`` and ``del`` take the same lock
2. read and parse every service file, see `3`_, resolve the interpreter the #! line of a file with commands names, see `3.12`_, and strip the comments from a shell script. the first error stops the build
3. check every script, up to 32 in parallel, a shell script with ``bash -n`` and the others as in `3.12`_. bash runs the check with extglob on, since ``-n`` never runs the shopt that would turn it on. every syntax error is reported with its service name and the failures print in service order. a check still running after 30 s is killed and reported as not finished. any error stops the build
4. resolve depon and depof, merge duplicate edges, reject cycles
5. order the services topologically. among the services ready to be placed, roots come first, then the service with the longest chain of dependents, then the first by name
6. resolve the default onfail of each service and count its dependents
7. build the image and verify it with the checks ninit applies at boot
8. write it to a temporary file in the output directory and fsync it, hard link the current graph to FILE.old, rename the new file over FILE, fsync the directory

with ``--root``, the interpreters of `3.12`_ and /etc/locale.conf are looked up below ROOT as the system booted from it sees them, absolute links and ``..`` included, and every syntax check runs chrooted into ROOT. the chroot requires CAP_SYS_CHROOT, without it no script can be checked and the build requires ``--no-check``. DIR and FILE are paths on the running system. the lookup uses openat2, linux 5.6 or newer

a failed build writes nothing and leaves the existing graph in place. if FILE or FILE.old exists and is not a graph, the build fails instead of replacing it. the output mode is 0644 less the umask, and less group or other read permission if any service file lacks it

on success ``init`` prints ``FILE: N services, E edges, R roots, B bytes``. ``-n`` prints ``FILE: would write`` followed by the same figures and the mode

``init`` warns about:

- daemons that have dependents and no notify, see `3.4`_
- a missing /etc/locale.conf, or one that sets no locale variable
- a #% line after the header
- a file with a reserved name that is not a graph

the running ninit does not read the new graph. it takes effect at the next boot

4.2 show
--------

::

   ninitctl show [-f FILE | FILE] [-v]

``show`` verifies FILE, default /etc/ninit.d/depgraph, and prints it::

   $ ninitctl show
   depgraph  /etc/ninit.d/depgraph
   31 services, 33 edges, 8 roots, 5 levels, 4651 bytes, crc ea286463

     #  service         type     lvl  kills  onfail  rdy  rst  depends on
     ─────────────────────────────────────────────────────────────────────────
     0  fs              oneshot    1     20  shell     —    —  —
     1  tmpfiles-dev    oneshot    1     14  stop      —    —  —
     2  modules-load    oneshot    1     10  stop      —    —  —
     3  console         oneshot    1      9  stop      —    —  —
     4  hostname        oneshot    1      9  stop      —    —  —
     5  loopback        oneshot    1      1  stop      —    —  —
     6  binfmt          oneshot    1      0  warn      —    —  —
     7  hwclock         oneshot    1      0  warn      —    —  —
     8  mounts          oneshot    2     19  shell     —    —  fs
     9  udev            daemon     2     13  stop      3  yes  tmpfiles-dev
    10  dbus            daemon     3      4  stop      3  yes  mounts
    11  sysctl          oneshot    2      9  stop      —    —  modules-load
    12  tmpfiles        oneshot    3      9  stop      —    —  mounts
    13  udev-trigger    oneshot    3      9  stop      —    —  udev
    14  basic           target     4      8  stop      —    —  console hostname mounts sysctl tmpfiles udev-trigger
    15  elogind         daemon     4      1  stop      3  yes  udev dbus
    16  NetworkManager  daemon     4      0  warn      3  yes  loopback udev dbus
    17  bluetooth       daemon     4      0  warn      —  yes  udev dbus
    18  chronyd         daemon     3      0  warn      —  yes  mounts
    19  cronie          daemon     3      0  warn      —  yes  mounts
    20  cupsd           daemon     3      0  warn      —  yes  mounts
    21  getty1          daemon     5      0  warn      —  yes  basic
    22  getty2          daemon     5      0  warn      —  yes  basic
    23  getty3          daemon     5      0  warn      —  yes  basic
    24  getty4          daemon     5      0  warn      —  yes  basic
    25  getty5          daemon     5      0  warn      —  yes  basic
    26  getty6          daemon     5      0  warn      —  yes  basic
    27  sddm            daemon     5      0  warn      —  yes  basic elogind
    28  sshd            daemon     5      0  warn      —  yes  basic
    29  swap            oneshot    3      0  warn      —    —  mounts
    30  sysklogd        daemon     3      0  warn      —  yes  mounts

   deepest dependency chain (5 services)
     fs → mounts → tmpfiles → basic → getty1

==========  ========================================
column      meaning
==========  ========================================
``#``       index in the graph. the service's cgroup is svc-#
service     name. roots are shown in green on a terminal
type        oneshot, daemon or target
lvl         number of services in the longest prerequisite chain ending at this one, itself included
kills       number of services that depend on it, directly or not. these are skipped if it fails
onfail      effective failure policy
rdy         notify fd
rst         yes for restart: always
depends on  direct prerequisites
==========  ========================================

the last line is one of the longest chains. ``-v`` adds under each service its effective start-timeout, stop-timeout, start-tries, start-delay and deps, marked ``(set)`` if any policy directive was given and ``(default)`` if not, followed by its script as stored, without comments

.. _4.3:

4.3 add, del
------------

::

   ninitctl del [-d DIR] [--] NAME...
   ninitctl add [-d DIR] [--] NAME...

``del`` moves DIR/NAME to DIR/unused/NAME, creating unused/ if needed. ``add`` moves it back. neither replaces an existing file. a relative symlink is rewritten so that it resolves to the same target from its new location. both take the lock described in `4.1`_. neither rebuilds the graph

4.4 exit status
---------------

======  ========================================
status  meaning
======  ========================================
0       success
1       failure, including a failure reported by ninit
2       usage error
======  ========================================

this applies to every ninitctl command

5 boot
======

.. _5.1:

5.1 kernel command line
-----------------------

::

   init=/usr/sbin/ninit

the path is wherever ninit was installed. pid 1 must be started through a path whose last component is ninit, not through a link named init. ninit-shutdown identifies ninit by /proc/1/comm

a systemd-boot entry::

   title    linux (ninit)
   linux    /vmlinuz-linux
   options  root=PARTUUID=... rw init=/usr/sbin/ninit

``ninit_graph=PATH`` selects a different graph. an argument that ninit receives beginning with ``/`` takes precedence over it. the kernel passes arguments after ``--`` to init unchanged, and elsewhere drops bare arguments that contain a ``.``, so a path is normally given as ``ninit_graph=``. the previous graph, /etc/ninit.d/depgraph.old, can be booted this way

.. _5.2:

5.2 initialisation
------------------

ninit performs, in order:

1. exit with status 1 if it is not pid 1
2. open /dev/null on any of fd 0 to 2 that is closed. set umask 022 and working directory ``/``
3. block every signal except SIGSEGV, SIGBUS, SIGILL and SIGFPE, receive SIGCHLD, SIGTERM, SIGINT, SIGUSR1 and SIGUSR2 through a signalfd, and have ctrl-alt-del send SIGINT
4. mount each filesystem below that is not already mounted, creating the mount point. a failure is logged as a warning
5. create the links /dev/fd, /dev/stdin, /dev/stdout and /dev/stderr, and /dev/core if /proc/kcore exists
6. create /sys/fs/cgroup/ninit.services, or fall back to process groups
7. create /run/ninit with mode 0700 and listen on /run/ninit/control with mode 0600
8. set its own oom_score_adj to -1000
9. write its log to /dev/console
10. raise its own RLIMIT_NOFILE
11. read /etc/locale.conf
12. print ``Welcome to NAME!``, where NAME is PRETTY_NAME from /etc/os-release or /usr/lib/os-release
13. load and verify the graph. on failure, log the reason and load the graph at the same path with ``.old`` appended, the one the last ninitctl init replaced. if that does not exist or fails too, start the emergency shell
14. start every root

==============  ========  ========================================
mount point     type      options
==============  ========  ========================================
/proc           proc      nosuid, noexec, nodev
/sys            sysfs     nosuid, noexec, nodev
/dev            devtmpfs  nosuid, mode=0755
/run            tmpfs     nosuid, nodev, mode=0755
/dev/pts        devpts    nosuid, noexec, mode=0620, gid=5, ptmxmode=0666
/dev/shm        tmpfs     nosuid, nodev, mode=1777
/sys/fs/cgroup  cgroup2   nosuid, noexec, nodev, nsdelegate
==============  ========  ========================================

the control socket exists before the graph is loaded, so ninitctl works from the emergency shell

5.3 startup
-----------

startup is event driven. when a service becomes ready, each dependent whose prerequisites are now all up is started at once. the roots are started together, in index order

while any service is starting, each 10 s in which nothing else happens produces ``WAIT NAME: still running after N s`` for every service still starting

startup ends when no service is starting and no restart is pending. ninit then prints a summary: each failed and each skipped service, the number never started, and ``boot: N of M services up in T ms``

processes reparented to pid 1 are reaped

.. _5.4:

5.4 log
-------

each line is ``[SSS.mmm] TAG > message``, where the time is seconds since ninit started

====  ========================================
tag   meaning
====  ========================================
DONE  a service became ready. for a oneshot or daemon, with the time since it was spawned
NOTE  progress and service output
WAIT  a service is still starting
WARN  a problem ninit recovered from
FAIL  a failure
====  ========================================

every line is printed on the console and appended to a 128 KiB ring in memory, which overwrites its oldest lines. ninitctl log reads the ring

on a terminal the tags are coloured. with ``--enable-quiet`` only WARN and FAIL lines are printed on the console. the ring receives every line

ninit never blocks on the console. a line the console does not accept within 100 ms is dropped whole and counted, and the count is printed as ``console: dropped N messages`` once output resumes. a line is never cut short. the ring retains dropped lines

a line equal to the line before it, at the same tag, is counted instead of logged. a block of up to 16 lines logged twice in a row is counted the same way from its third occurrence. the count is logged as ``last message repeated N times`` or ``last N messages repeated M times`` before the next different line. a count still open 30 s after its first repeat is logged then, the next after 120 s, then every 600 s. a single repeat is logged as it was. a block that stops partway is logged as it arrived, at most 1 s after its first held line. the count carries the most severe tag of its block, and reaches the console if any line of the block did

.. _6:

6 runtime control
=================

.. _6.1:

6.1 commands
------------

runtime commands go to ninit over /run/ninit/control. the socket has mode 0600 in a directory with mode 0700, so only root can use it

===============================  ========================================
command                          effect
===============================  ========================================
``ninitctl status [NAME]``       state of every service, or of NAME
``ninitctl log [-e] [-w] [-t]``  the log ring, oldest first. ``-e`` prints only WARN and FAIL lines. ``-w`` then prints each line as it is logged, until interrupted. ``-t`` prints only the boot time, ``T ms`` from the boot summary, and fails until startup has ended
``ninitctl start NAME``          clear the hold, reset the attempt count and the restart delay, start NAME. returns when it is ready or has failed
``ninitctl stop NAME``           hold NAME, send it SIGTERM, and SIGKILL after stop-timeout. returns when it has stopped
``ninitctl restart NAME``        ``stop``, then ``start``. if NAME is already stopped, ``start``
``ninitctl resume``              return every failed and skipped service to pending, reset their attempt counts, start those whose prerequisites are up
``ninitctl reload``              refused. the graph is read once, at boot
===============================  ========================================

``status`` prints a table of index, name, state and main pid, with a held column when any service is held. its last line is ``N services, M up``

6.2 service states
------------------

========  ========================================
state     meaning
========  ========================================
pending   not started. waiting for prerequisites, held, or stopped before it became ready
starting  spawned and not yet ready
up        ready and still up
down      was ready, has since exited or been stopped
failed    exhausted start-tries
skipped   will not start because a prerequisite failed or went down
========  ========================================

6.3 semantics
-------------

- an operation belongs to the service, not to the connection. closing the client does not cancel it
- a service accepts one operation at a time. a second is refused with ``NAME is busy``
- ``start`` is refused if the service is running or its previous instance has not exited
- ``start`` is refused with ``waiting on a prerequisite`` if a prerequisite is not up. the service is left pending and starts when its prerequisites are up. a service that was up goes down
- ``start`` on a oneshot that has completed runs it again
- ``start`` reports failure if the service is neither ready nor failed within start-timeout plus 2 s
- ``stop`` reports failure if the service has not stopped 2 s after SIGKILL
- ``stop`` does not stop dependents. what they may do while the service is down is set by its deps, see `3.5`_
- a held service is not restarted, not started when its prerequisites come up, and not started by ``resume``
- targets have no process and cannot be started, stopped or restarted
- once shutdown has begun the socket is no longer served

6.4 protocol
------------

a client connects, sends one request line, and reads the reply until ninit closes the connection

- request: ``VERB [NAME]`` and a newline, at most 1023 bytes. a carriage return before the newline is ignored. VERB is a command from `6.1`_
- reply: zero or more data lines, then one final line. each line begins with a marker and a space, ``.`` for data, ``+`` for success and ``-`` for failure
- ``log time`` is answered with one data line, ``T ms`` from ``boot: N of M services up in T ms``, or with ``- boot has not finished``
- ``log watch`` is answered with the ring, then with each line as it is logged, and has no final line. a watcher the ring overtakes receives one WARN line, ``log: skipped N bytes``, in place of the lines it lost. at most 4 connections watch at once
- a client that closes its end before the final line is dropped. its operation continues
- at most 8 connections are served at once. a further connection receives ``- too many control connections``

a ``status`` data line has the form ``NAME STATE HOLD pid PID``, where HOLD is ``wanted`` or ``stopped`` and PID is 0 when there is no process::

   > status udev
   < . udev up wanted pid 412
   < + udev

7 shutdown
==========

.. _7.1:

7.1 signals
-----------

===============  ========================================
signal to pid 1  action
===============  ========================================
SIGTERM          reboot
SIGINT           reboot. the kernel sends it on ctrl-alt-del
SIGUSR1          halt
SIGUSR2          power off
===============  ========================================

7.2 sequence
------------

1. stop the services in reverse dependency order across the whole graph. a service is sent SIGTERM once every service that depends on it has stopped, and SIGKILL after its stop-timeout. independent branches stop in parallel. this step ends after 30 s regardless
2. send SIGTERM to every process and wait up to 5 s for all of them to exit
3. send SIGKILL to what remains and wait up to 2 s
4. run ``hwclock --systohc --utc``, or ``hwclock --systohc --localtime`` if the third line of /etc/adjtime is LOCAL, killing it after 5 s
5. swapoff every active swap area
6. remove the control socket
7. sync, then remount every filesystem read-only, most recently mounted first, in up to 3 passes. a filesystem that stays writable is unmounted. if that fails, it is synced and remounted or unmounted once more, and otherwise detached. ``/`` is remounted read-only last
8. sync and call reboot(2) with the requested action

step 1 follows every dependency, including those with deps: order. during shutdown services are not restarted, the emergency shell is not respawned, and further shutdown signals are ignored

**note** on a machine whose hardware clock keeps local time, set the third line of /etc/adjtime to LOCAL

.. _7.3:

7.3 ninit-shutdown
------------------

ninit-shutdown is one binary installed under several names. the name it is invoked by selects the default action:

========================  ==============  ========================================
name                      default action  operand
========================  ==============  ========================================
shutdown, ninit-shutdown  power off       TIME, required
poweroff                  power off       TIME, optional
reboot                    reboot          TIME, optional
halt                      halt            TIME, optional
telinit                   none            runlevel, required. ``0`` powers off, ``6`` reboots, ``q`` does nothing
========================  ==============  ========================================

==========================================  ========================================
option                                      effect
==========================================  ========================================
``-r``                                      reboot
``-h``, ``-P``, ``-p``                      power off
``-H``                                      halt
``-f``                                      sync and call reboot(2) directly. services are not stopped and filesystems are not remounted
``-c``                                      refused. a pending shutdown runs in the foreground and is cancelled by interrupting it
``-t SEC``, ``-k``, ``-n``, ``-w``, ``-d``  accepted and ignored
``--help``                                  print usage
==========================================  ========================================

TIME is ``now``, ``+MINUTES``, or ``HH:MM`` in local time, meaning its next occurrence. operands after TIME, such as a message, are ignored

ninit-shutdown performs, in order:

1. if /proc/1/comm is readable and is not ninit, execute NAME.old from its own directory with the same arguments
2. with ``-f``, sync and call reboot(2)
3. if TIME is in the future, print a notice on /dev/console and wait in the foreground. ctrl-c cancels
4. as root, send the signal from `7.1`_ to pid 1
5. otherwise, call org.freedesktop.login1.Manager.PowerOff, Reboot or Halt through ``dbus-send --system``, leaving the decision to elogind and polkit. if that fails, exit with ``permission denied``

.. code:: sh

   make tools-install     # save the originals as NAME.old, link the names to ninit-shutdown
   make tools-uninstall   # put the originals back

tools-install operates in sbindir. each existing shutdown, poweroff, halt, reboot and telinit is renamed to NAME.old. an existing symlink is saved as a new symlink to the same target, or to TARGET.old if the target is one of these names. an existing NAME.old is never replaced. /sbin/init is not touched. tools-uninstall renames every NAME.old back, removes each link to ninit-shutdown that has no NAME.old, and removes ninit-shutdown

.. _8:

8 emergency shell
=================

the emergency shell starts when:

- the graph is missing, unreadable, empty or fails verification, which includes a version mismatch, and the graph with ``.old`` appended to its path does not exist or fails the same way, see `5.2`_
- a service with onfail: shell exhausts its start-tries
- memory runs out while ninit allocates its tables at boot

ninit keeps running while the shell runs. services unaffected by the failure continue to start, and ninitctl works from the shell. once the shell has started ninit prints ``shell: started on the console, exit with reboot or poweroff``

before starting the shell ninit resets the console. a virtual terminal left in graphics mode is returned to text mode, a raw keyboard to unicode, and the terminal settings to sane defaults

the shell runs on /dev/console in its own session with the console as its controlling terminal, with PATH as for services, ``HOME=/``, and ``TERM=linux`` on a virtual terminal or ``TERM=vt220`` otherwise. ninit tries, in order:

1. /sbin/sulogin, then /usr/sbin/sulogin, only with ``--with-sulogin`` and only if root's hash in /etc/shadow is usable, meaning not empty and not beginning with ``!`` or ``*``
2. the ``--with-busybox`` path, as the login shell ``-sh``
3. /bin/sh, as the login shell ``-sh``

when the shell exits it is started again. ninit stops restarting it after 5 consecutive exits within 1 s of starting, after 2 consecutive failures to execute any shell, or after 5 consecutive failures to start it, which are retried every 2 s

**note** without ``--with-sulogin`` the emergency shell is an unauthenticated root shell on the console. where the console is reachable remotely, such as over a serial line or a BMC, access to it is equivalent to a root credential. with ``--with-sulogin``, a root account without a usable hash still receives an unauthenticated shell, as does a system on which sulogin cannot be executed

9 depgraph format
=================

a graph is loaded only if its version equals the NG_VERSION ninit was built with, defined in src/ngraph.h, currently 8. after upgrading ninit, run ``ninitctl init`` before rebooting. an older graph is refused. ninit then loads depgraph.old, which after an upgrade is as old unless ninitctl init ran twice, and the boot ends in the emergency shell when that is refused too

ninit reads the whole graph into private read-only memory and verifies it before use. every offset and length is bounds checked, sections must not overlap, the checksum must match, every edge must point to a higher index, stored counts must agree with the edge list, names must be valid and unique, and every field must be in range. a graph that fails any check is not used. ninitctl show applies the same checks

all integers are in host byte order. ninitctl writes the sections in the order below. ninit locates them through the offsets in the header

=================  =========  ===============
section            alignment  size in bytes
=================  =========  ===============
header             8          64
service table      8          16 × n_svc
dependent offsets  4          4 × (n_svc + 1)
dependent indices  4          4 × n_edges
policy table       4          16 × n_svc
string table       1          blob_len
=================  =========  ===============

header:

======  ====  ============  ========================================
offset  size  field         description
======  ====  ============  ========================================
0x00    4     magic         0x4744494e, the bytes NIDG
0x04    4     version       NG_VERSION
0x08    4     crc32         crc32c of the whole file with this field set to 0
0x0c    4     total_len     file size
0x10    4     n_svc         number of services
0x14    4     n_roots       number of roots
0x18    4     n_edges       number of edges
0x1c    4     blob_len      size of the string table
0x20    4     off_svc       offset of the service table
0x24    4     off_rdep_off  offset of the dependent offsets
0x28    4     off_rdep_idx  offset of the dependent indices
0x2c    4     off_blob      offset of the string table
0x30    4     off_pol       offset of the policy table
0x34    4     reserved      0
0x38    8     srcs_hash     64-bit fnv-1a of the source files' names and contents in name order. not checked
======  ====  ============  ========================================

service record:

======  ====  ==========  ========================================
offset  size  field       description
======  ====  ==========  ========================================
0x0     2     unmet       number of prerequisites. 0 for roots and only for roots
0x2     1     type        0 oneshot, 1 daemon, 2 target
0x3     1     flags       bits 1:0 onfail, 0 warn, 1 stop, 2 shell. bit 2 restart. bit 3 interpreter, the script runs with the argv at exec_off instead of the configured shell. other bits 0
0x4     2     n_desc      number of dependents, direct or not
0x6     2     notify_fd   0 for none, otherwise 3 to 255. daemons only
0x8     4     script_off  string table offset of the script. 0xffffffff for a target
0xc     4     name_off    string table offset of the name
======  ====  ==========  ========================================

policy record:

======  ====  ===========  ========================================
offset  size  field        description
======  ====  ===========  ========================================
0x0     4     start_ms     start-timeout. 0 means 90000
0x4     4     stop_ms      stop-timeout. 0 means 5000
0x8     2     retry_ms     start-delay
0xa     1     start_tries  start-tries. 0 means 1 for a oneshot and 2 for a daemon
0xb     1     pflags       bit 0 deps: order. other bits 0
0xc     4     exec_off     string table offset of the interpreter argv when flags bit 3 is set, otherwise 0xffffffff
======  ====  ===========  ========================================

services are stored in topological order, roots first at indices 0 to n_roots - 1. the dependents of service i are entries ``rdep_off[i]`` up to but not including ``rdep_off[i + 1]`` of the dependent indices, sorted ascending, each greater than i. the string table holds the NUL-terminated names and scripts and ends in a NUL

an interpreter argv is two lists of NUL-terminated strings, each ended by an empty string. ninit runs the first string of the first list with the first list, the script and the second list as its arguments. the first string is an absolute path of at most 255 bytes, the lists hold at most 16 strings together and each string is at most 4096 bytes

10 limits
=========

================================================  ========================================
quantity                                          limit
================================================  ========================================
services in a graph                               8192
service name                                      255 bytes
script of a oneshot or daemon, without comments   131071 bytes
service file                                      135167 bytes
start-timeout, stop-timeout                       1 ms to 24 h
start-delay                                       1 ms to 65535 ms
start-tries                                       1 to 250
notify                                            fd 3 to 255
interpreter path from a #! line                   255 bytes
interpreter arguments, those ninit adds included  16, of 4096 bytes each
output kept per service                           1024 bytes
syntax check messages kept per script             4095 bytes
service output line                               256 bytes, longer lines are split
log ring                                          128 KiB
repeating block of log lines                      16 lines
control request                                   1023 bytes
concurrent control connections                    8
concurrent log watchers                           4
/etc/locale.conf                                  8191 bytes, 14 variables, 127 bytes per assignment
================================================  ========================================

=========================================  =======================================
interval                                   value
=========================================  =======================================
syntax check of one script                 30 s
minimum delay between start attempts       10 ms
restart delays                             100, 250, 500, 1000, 2000, then 5000 ms
uptime that resets the restart delay       10 s
stall report                               10 s
ninitctl start deadline                    start-timeout + 2 s
ninitctl stop wait after SIGKILL           2 s
ordered stop at shutdown                   30 s
wait after SIGTERM to every process        5 s
wait after SIGKILL to every process        2 s
hwclock at shutdown                        5 s
console write                              100 ms
log repeat count                           30 s, 120 s, then every 600 s
log line held as a possible repeat         1 s
emergency shell exit counted as immediate  1 s
=========================================  =======================================

11 files
========

====================================  ========================================
path                                  description
====================================  ========================================
/etc/ninit.d/                         service directory
/etc/ninit.d/unused/                  disabled services
/etc/ninit.d/depgraph                 compiled graph
/etc/ninit.d/depgraph.old             graph replaced by the last ninitctl init, loaded when depgraph fails
DOCDIR/ninit.d/                       example service set, see `3.11`_
/etc/locale.conf                      locale for services
/etc/adjtime                          hardware clock mode for hwclock at shutdown, UTC or LOCAL on its third line
/etc/os-release, /usr/lib/os-release  name in the welcome line
/etc/shadow                           read before the emergency shell with ``--with-sulogin``
/run/ninit/control                    control socket
/sys/fs/cgroup/ninit.services/svc-N/  cgroup of the service with index N
SBINDIR/NAME.old                      originals saved by ``make tools-install``
====================================  ========================================

contact
=======

ninit@nburch.org

license
=======

GNU General Public Licence v3.0
