#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

#define PIPESIZE 2048
#define min(a, b) ((a) < (b) ? (a) : (b))

/*
 * Testing: 
 *   Run directly in docker container through 
 *   a weird environment, so really slow numbers..
 *
 *   10MB, avg of 3 runs
 *
 *   - Unmodified : 96 ticks
 *   - New pipe   : 61 ticks
 *   - New memcpy : 107 ticks
 *   - Both       : 23 ticks
 */



struct pipe {
  struct spinlock lock;
  char data[PIPESIZE];
  uint nread;     // number of bytes read
  uint nwrite;    // number of bytes written
  int readopen;   // read fd is still open
  int writeopen;  // write fd is still open
};

int
pipealloc(struct file **f0, struct file **f1)
{
  struct pipe *pi;

  pi = 0;
  *f0 = *f1 = 0;
  if((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  if((pi = (struct pipe*)kalloc()) == 0)
    goto bad;
  pi->readopen = 1;
  pi->writeopen = 1;
  pi->nwrite = 0;
  pi->nread = 0;
  initlock(&pi->lock, "pipe");
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = pi;
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = pi;
  return 0;

 bad:
  if(pi)
    kfree((char*)pi);
  if(*f0)
    fileclose(*f0);
  if(*f1)
    fileclose(*f1);
  return -1;
}

void
pipeclose(struct pipe *pi, int writable)
{
  acquire(&pi->lock);
  if(writable){
    pi->writeopen = 0;
    wakeup(&pi->nread);
  } else {
    pi->readopen = 0;
    wakeup(&pi->nwrite);
  }
  if(pi->readopen == 0 && pi->writeopen == 0){
    release(&pi->lock);
    kfree((char*)pi);
  } else
    release(&pi->lock);
}

int
pipewrite(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  int delta;
  struct proc *pr = myproc();

  acquire(&pi->lock);
  while(i < n){
    if(pi->readopen == 0 || pr->killed){
      release(&pi->lock);
      return -1;
    }
    if(pi->nwrite == pi->nread + PIPESIZE){ //DOC: pipewrite-full
      wakeup(&pi->nread);
      sleep(&pi->nwrite, &pi->lock);
    } else {
      // min of: remaining data to write, length to end of pipe, and total free pipe space
      delta = min(n-i, PIPESIZE - (pi->nwrite - pi->nread));
      delta = min(delta, PIPESIZE - (pi->nwrite % PIPESIZE));
     
      /*
      printf("n: %d\n", n);
      printf("nread: %d\n", pi->nread);
      printf("nwrite: %d\n", pi->nwrite);
      printf("i: %d\n", i);
      printf("delta write: %d\n\n", delta);
      */

      if(copyin(pr->pagetable, &pi->data[pi->nwrite % PIPESIZE], addr + i, delta) == -1) {
        break;
      }

      pi->nwrite = pi->nwrite + delta;
      i = i + delta;
    }
  }
  wakeup(&pi->nread);
  release(&pi->lock);

  return i;
}

int
piperead(struct pipe *pi, uint64 addr, int n)
{
  struct proc *pr = myproc();

  acquire(&pi->lock);
  while(pi->nread == pi->nwrite && pi->writeopen){  //DOC: pipe-empty
    if(pr->killed){
      release(&pi->lock);
      return -1;
    }
    sleep(&pi->nread, &pi->lock); //DOC: piperead-sleep
  }
  
  int i = 0;
  int delta;
  while(i<n) {
    if(pi->nread == pi->nwrite) 
      break;
    // min of everything ready for read, data to read, and length to end of pipe
    delta = min(pi->nwrite - pi->nread, n-i);
    delta = min(delta, PIPESIZE - (pi->nread % PIPESIZE));

    /*
    printf("nread: %d\n", pi->nread);
    printf("nwrite: %d\n", pi->nwrite);
    printf("delta read: %d\n\n", delta);
    */

    if(copyout(pr->pagetable, addr + i, &pi->data[pi->nread % PIPESIZE], delta) == -1) {
      break;
    }

    pi->nread = pi->nread + delta;
    i = i + delta;
  }

  wakeup(&pi->nwrite);  //DOC: piperead-wakeup
  release(&pi->lock);
  return i;
}
