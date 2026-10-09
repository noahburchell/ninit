# tests

`make check` builds and runs every suite below with automake's parallel test harness. each test prints tap, the harness writes `NAME.log` and a summary to `test-suite.log` in the build directory

## suites

| suite | path | requires | covers |
|---|---|---|---|
| unit | `tests/unit/t-*.c` | nothing | graph verifier, crc32c, names, `/etc/locale.conf`, log ring and console, failure reporting, emergency shell state machine, `ninit-shutdown` argument handling, comment stripper, pid table, scheduler simulation, swap and hardware clock steps of shutdown |
| ninitctl | `tests/ctl/*.test` | bash, coreutils | `init`, `show`, `add`, `del`, directives, ordering, limits, locking, `make tools-install`, the examples in `docs/ninit.d` against README 4.2 |
| interpreters | `tests/ctl/interp.test`, `tests/qemu/interp.test` | python3, lua and perl, each part skips without its interpreter | `#!` selection, build time resolution, argv, syntax checks, and python, lua and perl services run by ninit |
| client | `tests/ctl/client.test` | `unshare(1)`, unprivileged user namespaces | the `ninitctl` client against a fake control socket in a private mount namespace |
| qemu | `tests/qemu/*.test` | qemu, a static busybox, a kernel image | ninit as pid 1 of a virtual machine: boot, readiness, failures, restart, dependencies, control socket, output, shutdown, signals, emergency shell, cgroups, console stalls |

a suite whose requirement is missing reports SKIP. no test contacts the control socket or pid 1 of the machine running the tests. the unit tests replace `fork`, `kill`, `reboot`, `mount` and the other system calls with in-process stubs, and abort on any attempt to reach a real process

## qemu tests

### kernel

the kernel is named by `NINIT_TEST_KERNEL`. it needs, built in and not as modules:

- `BLK_DEV_INITRD`, `DEVTMPFS`, `TMPFS`
- `SERIAL_8250` and `SERIAL_8250_CONSOLE`, with at least 2 ports
- `CGROUPS`, `UNIX`, `SIGNALFD`, `UNIX98_PTYS`
- `BINFMT_ELF`, `BINFMT_SCRIPT`
- `ACPI`, for poweroff to end the virtual machine
- `SERIO_I8042` and `KEYBOARD_ATKBD`, for the ctrl-alt-del test

most distribution kernels qualify. the one on an installation image is enough, no modules are loaded:

```sh
bsdtar -xf alpine-standard-3.24.2-x86_64.iso boot/vmlinuz-lts
make check NINIT_TEST_KERNEL="$PWD/boot/vmlinuz-lts"
```

linux before 7.3 kills a process created with `CLONE_INTO_CGROUP` in a cgroup that `cgroup.kill` was written to before. ninit recreates such a cgroup before the next start, so `fail.test` and `restart.test` cover that case on those kernels

### running

| variable | default | meaning |
|---|---|---|
| `NINIT_TEST_KERNEL` | none | kernel image. the qemu tests are skipped without it |
| `NINIT_TEST_QEMU` | `qemu-system-x86_64` | qemu binary |
| `NINIT_TEST_BUSYBOX` | `busybox` in `PATH` | busybox for the guest userland and the initramfs |
| `NINIT_TEST_ACCEL` | `kvm` if `/dev/kvm` is usable, else `tcg` | qemu accelerator |
| `NINIT_TEST_SCALE` | 1 with kvm, 6 with tcg | multiplier for every timeout |
| `T_KEEP` | unset | keep the scratch directory of each test and print its path |

one test:

```sh
make check TESTS=tests/qemu/boot.test NINIT_TEST_KERNEL=/path/to/vmlinuz
```

a `ninit` built with a sanitizer, by `--enable-debug` or `-fsanitize` in `CFLAGS`, cannot run as pid 1 and skips the qemu tests

each boot leaves `console.log` (the serial console without colour), `tap.log` (the guest's results), `initrd` and `init.err` in its scratch directory

### guest layout

the initramfs is `tests/qemu/base.cpio`, built once per build tree from `ninit`, `ninitctl`, `ninit-shutdown`, the configured shell and its libraries, busybox and `tests/qemu/guest.sh`, followed by the files of the scenario. `interp.test` adds python with the modules its startup loads, lua and perl to its scenario, at the paths `ninitctl init` resolves on the build system. ninit is started with `rdinit=/sbin/ninit`. a dynamically linked musl build cannot be copied into the guest, link it statically with `LDFLAGS=-static`

## writing tests

### shell tests

source `tests/lib.sh`. it provides `$T`, a scratch directory removed at exit, and:

| function | effect |
|---|---|
| `t_ok RC WHAT` | pass when RC is 0 |
| `t_is GOT WANT WHAT` | pass when equal |
| `t_like GOT ERE WHAT`, `t_unlike` | pass when GOT matches, or does not match, the extended regular expression |
| `t_has GOT TEXT WHAT` | pass when GOT contains TEXT |
| `t_skip WHAT`, `t_skip_all WHY` | skip one test, or the whole file |
| `run CMD...` | run CMD, leave `$out`, `$err` and `$rc` |
| `t_done` | print the plan, last |

### qemu tests

source `tests/lib.sh` and `tests/qemu/lib.sh`, then call `q_require`. a scenario is:

```sh
q_new NAME
q_svc SERVICE <<'EOF'
...service file...
EOF
q_check <<'EOF'
...guest assertions...
EOF
q_boot
q_import
q_poweroff_ok
```

`q_check` writes the service `check`, a daemon without prerequisites that sources `/t/lib.sh`, runs the assertions and powers the guest off. `T_END` set in the assertions selects `reboot`, `halt`, `none` or a command instead. `q_import` turns the guest's results into host tests. `q_has`, `q_lacks`, `q_like` and `q_order` assert on the console log after the guest has stopped

`Q_ARGS` adds to the kernel command line, `Q_TIMEOUT` sets the time limit in seconds, `Q_HALT` ends a guest that halts, `Q_PIPE` puts the console on a fifo for `q_send`, `q_wait` and `q_mark`, and `Q_POLL` names a function run every 50 ms while the guest runs

the guest library provides `ok`, `is`, `like`, `unlike`, `has`, `lacks` and `check` as above, and:

| function | effect |
|---|---|
| `st NAME` | state of NAME from the control socket |
| `pid_of NAME` | main pid of NAME |
| `st_is NAME STATE [MS]` | pass when NAME reaches STATE within MS, default 10000 |
| `wait_st`, `wait_for MS CMD...`, `wait_log TEXT [MS]` | wait without reporting |
| `log_has TEXT [WHAT] [MS]` | pass when TEXT appears in the log ring |
| `ring` | the log ring |
| `nctl [OPTION]... REQUEST` | raw control socket client, see `tests/helpers/nctl-raw.c` |
| `idx_of NAME`, `cg_of INDEX` | graph index of NAME, cgroup path of an index |
| `now_ms` | wall clock in milliseconds |
