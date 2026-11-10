// Test the getpinfo system call and the per-process
// scheduling counters. Run with make CPUS=1 qemu.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "kernel/pinfo.h"
#include "user/user.h"

#define SPIN 20    // ticks to spin for
#define SLACK 3    // allowed difference between runticks and ticks
#define RUNNING 4  // enum procstate value from kernel/proc.h

struct pinfo buf[NPROC];

void
fail(char *msg)
{
  printf("pinfotest: FAILED: %s\n", msg);
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

void
test_args(void)
{
  struct pinfo me;

  if (getpinfo(buf, -1) != -1)
    fail("max < 0 should return -1");
  if (getpinfo(buf, 0) != 0)
    fail("max = 0 should return 0");
  if (getpinfo(buf, 1) != 1)
    fail("max = 1 should return 1");
  if (buf[0].pid != 1 || strcmp(buf[0].name, "init") != 0)
    fail("first entry should be init");
  if (getpinfo((struct pinfo *)0xffffffffffffULL, NPROC) != -1)
    fail("bad buffer should return -1");

  if (lookup(getpid(), &me) < 0)
    fail("own pid not found");
  if (me.state != RUNNING)
    fail("own state should be RUNNING");
  if (strcmp(me.name, "pinfotest") != 0)
    fail("own name should be pinfotest");
  printf("args: ok\n");
}

void
test_runticks(void)
{
  struct pinfo before, after;

  lookup(getpid(), &before);
  int t0 = uptime();
  while (uptime() - t0 < SPIN)
    ;
  int elapsed = uptime() - t0;
  lookup(getpid(), &after);

  int charged = after.runticks - before.runticks;
  printf("runticks: %d charged over %d ticks\n", charged, elapsed);
  if (charged < elapsed - SLACK || charged > elapsed + SLACK)
    fail("runticks should grow about once per tick");
}

void
test_nsched(void)
{
  struct pinfo before, after;

  lookup(getpid(), &before);
  for (int i = 0; i < 5; i++)
    pause(1);
  lookup(getpid(), &after);

  printf("nsched: %d -> %d\n", before.nsched, after.nsched);
  // each pause() sleeps, so scheduler() must switch back to us.
  if (after.nsched < before.nsched + 5)
    fail("nsched should grow by at least one per sleep");
}

void
test_child(void)
{
  struct pinfo parent, child;

  lookup(getpid(), &parent);
  int t = uptime();
  int pid = fork();
  if (pid < 0)
    fail("fork");
  if (pid == 0) {
    // the parent has spun for SPIN ticks; none of it may carry over.
    if (lookup(getpid(), &child) < 0)
      fail("child pid not found");
    printf("child: runticks %d nsched %d level %d ctime %d (parent %d %d)\n",
           child.runticks, child.nsched, child.level, child.ctime,
           parent.runticks, parent.nsched);
    if (child.runticks > 1)
      fail("child runticks should start at 0");
    if (child.nsched > 2)
      fail("child nsched should start at 0");
    if (child.level != 0)
      fail("child level should start at 0");
    if (child.ctime < t || child.ctime > t + 1)
      fail("child ctime should be ticks at fork");
    exit(0);
  }
  int status;
  wait(&status);
  if (status != 0)
    exit(1);
}

int
main(void)
{
  test_args();
  test_runticks();
  test_nsched();
  test_child();
  printf("pinfotest: OK\n");
  exit(0);
}
