// SPDX-License-Identifier: BSD-2-Clause-Patent
// pianousb-shell: a login shell for the Mac on PianoDwc3's port 1 (docs/drivers/usb-device-debug.md section 6).
// Runs on the tablet's macOS (a LaunchDaemon, com.unixnu.pianousb-shell.plist): opens /dev/pianousb (the kernel creates it
// with piano-usb-shell=1), starts the shell on a pseudo-terminal and copies bytes both ways. A new shell starts whenever the
// last one exits; /dev/pianousb is reopened if it goes away.
//   pianousb-shell [DEVICE [SHELL]]      defaults: /dev/pianousb /bin/zsh
// On /dev/pianousb it runs only when the kernel was booted with piano-usb-shell=1 (the argument that makes the kernel create the
// node): without it, it exits at once with status 0 and its job (KeepAlive SuccessfulExit false) is not started again, so an
// installed profile without the argument has no shell and no polling for the node. Another DEVICE (a test pty) skips the check.
// Waits with select(), never poll(): XNU's poll() is kqueue underneath, and a character device's knote attaches only when its
// cdevsw is marked kqueue-capable (spec_kqfilter, filt_specattach: EINVAL otherwise) or the caller asks NOTE_LOWAT 1, which poll()
// never does; so poll() on /dev/pianousb as k27 built it returns POLLNVAL at once, every time. The helper of 2026-11-12 polled and
// took only POLLIN/POLLHUP/POLLERR: it spun at full CPU ("pianousb-shell caught burning CPU", desk-dev 17:27) and never read a byte
// the Mac sent, so the shell got no input and sent nothing back. select() goes to the cdevsw's d_select (selscan, fo_select) for
// any device; PianoDwc3 now also marks its cdevsw kqueue-capable. Host test: test_pianousb_shell.py (a device whose poll() answers
// POLLNVAL, as /dev/pianousb's did).
// Build on the Mac (arm64, ad-hoc signed by the linker): clang -O2 -Wall -Wextra -o pianousb-shell pianousb-shell.c
// Installed by configs/userland.toml (tooling/userland/stage.py, docs/userland.md).
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>

#include "../../../package/include/PianoBootArgs.h"

// PianoDwc3 reads piano-usb-shell as a 32-bit number. The merged line can carry it past kern.bootargs' 1023-byte clip.
static int shellArgument(void)
{
  char args[PIANO_BOOT_ARGS_MAX];
  pianoBootArgs(args);
  size_t length;
  const char *value = pianoBootArgValue(args, "piano-usb-shell", &length);
  if (!value || !length || *value < '0' || *value > '9') return 0;
  char *end;
  errno = 0;
  const unsigned long number = strtoul(value, &end, 0);
  return !errno && end == value + length && number > 0 && number <= UINT32_MAX;
}

static volatile sig_atomic_t stopping;
static volatile sig_atomic_t deviceFd = -1, shellFd = -1;
static sigset_t stopSignals;

static void closeFd(volatile sig_atomic_t *slot)
{
  const int fd = *slot;
  *slot = -1;
  if (fd >= 0) close(fd);
}

static void stop(int sig)
{
  (void)sig;
  const int saved = errno;
  stopping = 1;
  // Closing the descriptors also covers a signal arriving just before a blocking syscall.
  closeFd(&deviceFd);
  closeFd(&shellFd);
  errno = saved;
}

static void closeRelayFd(volatile sig_atomic_t *slot)
{
  sigset_t old;
  sigprocmask(SIG_BLOCK, &stopSignals, &old);
  closeFd(slot);
  sigprocmask(SIG_SETMASK, &old, NULL);
}

static int writeAll(int fd, const char *buf, size_t length)
{
  while (length && !stopping) {
    const ssize_t n = write(fd, buf, length);
    if (n > 0) {
      buf += n;
      length -= (size_t)n;
    } else if (n < 0 && errno == EAGAIN) {
      usleep(1000);
    } else if (n == 0 || errno != EINTR) {
      return -1;
    }
  }
  return length ? -1 : 0;
}

// Return the descriptor that closed or failed, or -1 when the relay can continue.
static int copy(int from, int to)
{
  char buf[4096];
  const ssize_t n = read(from, buf, sizeof buf);
  if (n == 0) return from;
  if (n < 0) return errno == EINTR || errno == EAGAIN ? -1 : from;
  return writeAll(to, buf, (size_t)n) ? to : -1;
}

static int relay(int fd, int master)
{
  static const char banner[] = "\r\npianousb-shell: a new shell on the tablet (exit starts another)\r\n";
  if (writeAll(fd, banner, sizeof banner - 1)) return fd;
  while (!stopping) {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fd, &r);
    FD_SET(master, &r);
    const int n = select((fd > master ? fd : master) + 1, &r, NULL, NULL, NULL);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (!stopping) fprintf(stderr, "pianousb-shell: select: %s\n", strerror(errno));
      return fd;
    }
    int failed = -1;
    if (FD_ISSET(fd, &r)) failed = copy(fd, master);
    if (failed < 0 && FD_ISSET(master, &r)) failed = copy(master, fd);
    if (failed >= 0) return failed;
  }
  return fd;
}

static void reapShell(pid_t pid)
{
  kill(pid, SIGHUP);
  for (int i = 0; i < 25; ++i) {
    const pid_t result = waitpid(pid, NULL, WNOHANG);
    if (result == pid || (result < 0 && errno != EINTR)) return;
    usleep(10000);
  }
  kill(pid, SIGKILL);
  while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

int main(int argc, char **argv)
{
  const char *device = argc > 1 ? argv[1] : "/dev/pianousb", *shell = argc > 2 ? argv[2] : "/bin/zsh";
  if (strcmp(device, "/dev/pianousb") == 0 && !shellArgument()) {
    fprintf(stderr, "pianousb-shell: no piano-usb-shell=1 in the boot arguments: no %s, nothing to do\n", device);
    return 0;
  }
  signal(SIGPIPE, SIG_IGN);
  sigemptyset(&stopSignals);
  sigaddset(&stopSignals, SIGTERM);
  sigaddset(&stopSignals, SIGINT);
  struct sigaction sa = {.sa_handler = stop, .sa_mask = stopSignals};
  if (sigaction(SIGTERM, &sa, NULL) || sigaction(SIGINT, &sa, NULL)) return 1;
  while (!stopping) {
    const int fd = open(device, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
      if (!stopping) sleep(2);
      continue;
    }
    deviceFd = fd;
    fprintf(stderr, "pianousb-shell: %s open\n", device);
    while (!stopping && fd < FD_SETSIZE) {
      sigset_t old;
      sigprocmask(SIG_BLOCK, &stopSignals, &old);
      if (stopping) {
        sigprocmask(SIG_SETMASK, &old, NULL);
        break;
      }
      struct winsize ws = {.ws_row = 50, .ws_col = 160};
      int master = -1;
      const pid_t pid = forkpty(&master, NULL, NULL, &ws);
      if (pid == 0) {
        if (fd > STDERR_FILENO) close(fd);   // forkpty has already installed the shell's standard descriptors
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        sigprocmask(SIG_SETMASK, &old, NULL);
        setenv("TERM", "xterm-256color", 1);
        setenv("HOME", "/var/root", 0);
        chdir("/var/root");
        execl(shell, shell, "-l", (char *)NULL);
        execl("/bin/sh", "sh", "-l", (char *)NULL);
        _exit(127);
      }
      if (pid > 0) shellFd = master;
      sigprocmask(SIG_SETMASK, &old, NULL);
      if (pid < 0) break;
      const int failed = !stopping && master >= 0 && master < FD_SETSIZE ? relay(fd, master) : fd;
      closeRelayFd(&shellFd);
      reapShell(pid);
      if (failed == fd) break;
    }
    closeRelayFd(&deviceFd);
    if (!stopping) sleep(1);
  }
  return 0;
}
