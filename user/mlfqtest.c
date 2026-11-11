// Test the MLFQ scheduler. Run with make CPUS=1 qemu.
// "mlfqtest stress" runs only the stress test, which also
// makes sense with more CPUs.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "kernel/pinfo.h"
#include "user/user.h"

#define NCHILD 20  // children per stress round
#define ROUNDS 4   // stress rounds

struct pinfo buf[NPROC];

void
fail(char *msg)
{
  printf("mlfqtest: FAILED: %s\n", msg);
  exit(1);
}

// fill *out with the entry for pid; return 0, or -1 if not found.
int
lookup(int pid, struct pinfo *out)
{
  int n = getpinfo(buf, NPROC);
  if (n < 0)
    fail("getpinfo returned -1");
  for (int i = 0; i < n; i++) {
    if (buf[i].pid == pid) {
      *out = buf[i];
      return 0;
    }
  }
  return -1;
}

struct pinfo
self(void)
{
  struct pinfo me;
  if (lookup(getpid(), &me) < 0)
    fail("own pid not found");
  return me;
}

// number of processes that are not UNUSED.
int
nprocs(void)
{
  return getpinfo(buf, NPROC);
}

// wait for one child, and exit if it failed (it printed why).
void
wait_ok(void)
{
  int status;
  if (wait(&status) < 0)
    fail("lost a child");
  if (status != 0)
    exit(1);
}

// The level a process should be at after r ticks charged since it
// was created or boosted: slices are 1, 2, 4 and 8 ticks.
int
expected_level(int r)
{
  if (r < 1)
    return 0;
  if (r < 3)
    return 1;
  if (r < 7)
    return 2;
  return 3;
}

// sleep until just after a boost, so a test has about 60 ticks
// before the next one.
void
sync_boost(void)
{
  while (uptime() % BOOSTTICKS < 1 || uptime() % BOOSTTICKS > 4)
    pause(1);
}

// Rules 3-6: a new process starts at level 0, is demoted each time
// it uses up its slice, stays at the lowest level, and keeps its
// used time when it sleeps partway through a slice.
void
test_demote(void)
{
  sync_boost();
  int pid = fork();
  if (pid < 0)
    fail("fork");
  if (pid == 0) {
    int window = uptime() / BOOSTTICKS;
    int target = 2;
    struct pinfo me = self();
    while (me.runticks < 20) {
      if (uptime() / BOOSTTICKS != window)
        fail("demote: a boost happened mid-test; rerun");
      if (me.level != expected_level(me.runticks)) {
        printf("demote: level %d after %d ticks, expected %d\n", me.level,
               me.runticks, expected_level(me.runticks));
        fail("demote: wrong level (time forgiven on sleep?)");
      }
      // sleep partway through slices at 2, 4, 6, ... ticks used.
      if (me.runticks >= target) {
        pause(2);
        target += 2;
      }
      me = self();
    }
    if (me.level != NLEVEL - 1)
      fail("demote: should stay at the lowest level");
    exit(0);
  }
  wait_ok();
  printf("demote: ok\n");
}

// Rules 1 and 7: a level-3 spinner is preempted as soon as a
// higher-level process wakes up, and a new level-0 process runs
// ahead of it.
void
test_priority(void)
{
  struct pinfo a0, a1, b;

  sync_boost();
  int window = uptime() / BOOSTTICKS;
  int a = fork();
  if (a < 0)
    fail("fork");
  if (a == 0)
    for (;;)
      ;

  // let the spinner use up its slices and sink to level 3.
  do {
    pause(1);
    lookup(a, &a0);
  } while (a0.runticks < 8);
  if (a0.level != NLEVEL - 1)
    fail("priority: spinner should be at the lowest level");

  // each pause(1) should return about a tick later, not after
  // the spinner's 8-tick slice.
  int t0 = uptime();
  for (int i = 0; i < 5; i++)
    pause(1);
  int elapsed = uptime() - t0;
  printf("priority: 5 x pause(1) took %d ticks beside a spinner\n", elapsed);
  if (elapsed > 8)
    fail("priority: waking process should preempt a lower level");

  lookup(a, &a0);
  int bpid = fork();
  if (bpid < 0)
    fail("fork");
  if (bpid == 0)
    for (;;)
      ;
  pause(5);
  lookup(a, &a1);
  lookup(bpid, &b);
  kill(a);
  kill(bpid);
  wait(0);
  wait(0);

  printf("priority: new process ran %d ticks, old spinner %d\n", b.runticks,
         a1.runticks - a0.runticks);
  if (b.runticks < 4 || a1.runticks - a0.runticks > 1)
    fail("priority: new level-0 process should run ahead of level 3");
  if (uptime() / BOOSTTICKS != window)
    fail("priority: a boost happened mid-test; rerun");
  printf("priority: ok\n");
}

// Rule 8: a process at the lowest level is moved back up when
// ticks reaches a multiple of BOOSTTICKS.
void
test_boost(void)
{
  int pid = fork();
  if (pid < 0)
    fail("fork");
  if (pid == 0) {
    struct pinfo me;
    do {
      me = self();
    } while (me.level != NLEVEL - 1);

    // the boost resets it to level 0; it then sinks again, but
    // stays above the lowest level for several ticks.
    int next = (uptime() / BOOSTTICKS + 1) * BOOSTTICKS;
    while (uptime() < next + 4) {
      me = self();
      if (uptime() >= next && me.level < NLEVEL - 1)
        exit(0);
    }
    fail("boost: lowest-level process was not boosted");
  }
  wait_ok();
  printf("boost: ok\n");
}

// Many processes forking, sleeping, spinning and exiting:
// every one must be reaped with its own exit status.
void
test_stress(void)
{
  int before = nprocs();

  for (int round = 0; round < ROUNDS; round++) {
    for (int i = 0; i < NCHILD; i++) {
      int pid = fork();
      if (pid < 0)
        fail("stress: fork");
      if (pid == 0) {
        if (i % 3 == 0) {
          int g = fork();
          if (g == 0) {
            pause(1);
            exit(0);
          }
          if (g > 0)
            wait(0);
        }
        if (i % 3 == 1) {
          pause(2);
        } else {
          int t = uptime();
          while (uptime() - t < 2)
            ;
        }
        exit(i);
      }
    }
    int seen = 0;
    for (int i = 0; i < NCHILD; i++) {
      int status;
      if (wait(&status) < 0)
        fail("stress: lost a child");
      seen |= 1 << status;
    }
    if (seen != (1 << NCHILD) - 1)
      fail("stress: wrong exit statuses");
  }
  if (wait(0) != -1)
    fail("stress: extra child");
  if (nprocs() != before)
    fail("stress: process count changed");
  printf("stress: ok\n");
}

int
main(int argc, char *argv[])
{
  if (argc > 1 && strcmp(argv[1], "stress") == 0) {
    test_stress();
  } else {
    test_demote();
    test_priority();
    test_boost();
    test_stress();
  }
  printf("mlfqtest: OK\n");
  exit(0);
}
