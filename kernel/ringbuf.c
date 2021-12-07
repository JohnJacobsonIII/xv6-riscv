#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"
#include <stdlib.h>

// Create & Destroy user-mode ring buffer

#define MAX_RINGBUFS 10
#define RINGBUF_SIZE 16
#define FIRST_VA (MAXVA - (5 * PGSIZE)) // trampoline, trapframe, and a couple more for fun

#define MIN(a,b) (((a)<(b))?(a):(b))

struct ringbuf {
  int refcount; // 0 for empty slot
  char name[16];
  void *buf[RINGBUF_SIZE];
  void *book; // manage attached processes
};

struct spinlock ringbuf_lock;
struct ringbuf *ringbufs[MAX_RINGBUFS] = { 0 };

struct book {
  int pids[2];
  uint64 vas[2];
};

/* helper to find index of process in ring buffer
 *
 * @param
 */
int
get_myindex(void){
  return -1;
}

/* helper to find index of a specific buffer within the buffer array
 * 
 * @param name:       identifier for ring buffer
 * @return buf_index: -1 if buffer array is full and no matching names,
 *                    or the index of a ring buffer with matching name if exists,
 *                    else index of an uninitialized ring buffer
 */
int
get_bufindex(char *name){
  int i;
  int buf_index = MAX_RINGBUFS;
 
  printf("start get_bufindex\n"); // REMOVE

  acquire(&ringbuf_lock);
  
  for(i=0; i < MAX_RINGBUFS; i++){
    // printf("i: %d\n", i); // REMOVE
    if(ringbufs[i] == 0){
      // first free index, but keep checking for matching name
      if(buf_index == MAX_RINGBUFS) buf_index = i;
    }
    else if(strncmp(ringbufs[i]->name, name, 16) == 0){
      buf_index = i;
      break; // found existing buffer
    }
  }
  
  release(&ringbuf_lock);

  if(buf_index == MAX_RINGBUFS){
    printf("Unable to identify or allocate ring buffer.\n");
    return -1;
  }
  
  return buf_index;
}

/* init new ringbuf
 *
 * @param name:      ringbuffer identifier
 * @param buf_index: index in ringbuf array
 *
 * @return           0 on success, -1 on failure
 */
int
init_ringbuf(char *name, int buf_index){
  int i;
  char *a;

  printf("initializing index = %d\n", buf_index);
  
  acquire(&ringbuf_lock);
    
  // create a page for the buffer
  a = kalloc();
  if(a == 0){
    panic("kalloc in init_ringbuf: ringbuf\n");
  }
  
  ringbufs[buf_index] = (struct ringbuf *)a;
  ringbufs[buf_index]->refcount = 0;
  
  // another page for the buffers bookkeeping
  a = kalloc();
  if(a == 0){
    panic("kalloc in init_ringbuf: ringbuf\n");
  }
  
  ringbufs[buf_index]->book = a;

  // then the shared ring pages
  for(i=0; i<RINGBUF_SIZE; i++){
    a = kalloc();
    if(a == 0){
      panic("kalloc in init_ringbuf: page\n");
    }
    ringbufs[buf_index]->buf[i] = a;
  }

  release(&ringbuf_lock);

  return 0;
}

/* map ringbuf to user process
 *
 * @param 
 */
int
map_ringbuf(char *name, uint64 buffer_loc, int buf_idx){
  uint64 va;
  uint64 page_offset;
  uint64 buf_offset;
  int i,j;

  //printf("MAXVA: %p, FIRST_VA: %p\n", MAXVA, FIRST_VA); // REMOVE
  //printf("MAXVA diff: %d\n", MAXVA - FIRST_VA); // REMOVE

  int map_attempts = 100; // arbitrary

  struct proc *p = myproc();
  pagetable_t pt = p->pagetable;

  acquire(&ringbuf_lock);

  for(i=0;i<map_attempts;i++){
    va = FIRST_VA + (i * RINGBUF_SIZE * PGSIZE); // ringbuf_size is an arbitrary choice here, just a bigger jump.

    //printf("MAXVA: %p, va: %p, i: %d\n", MAXVA, va, i); // REMOVE

    // Add bookkeeping page
    printf("Map bookkeeping, try: %d\n", i); // REMOVE
    if(mappages(pt, va, PGSIZE, (uint64)ringbufs[buf_idx]->book, PTE_U | PTE_R | PTE_W | PTE_X) < 0) continue;

    // try to map the buffer
    for(j=0;j<RINGBUF_SIZE;j++){
      page_offset = (j+1) * PGSIZE;
      buf_offset = RINGBUF_SIZE * PGSIZE;

      printf("Map page %d first copy, try: %d\n", j, i); // REMOVE
      if(mappages(pt, va + page_offset, PGSIZE, (uint64)ringbufs[buf_idx]->buf[j], PTE_U | PTE_R | PTE_W | PTE_X) < 0) break;
      printf("Map page %d second copy, try: %d\n", j, i); // REMOVE
      if(mappages(pt, va + page_offset + buf_offset, PGSIZE, (uint64)ringbufs[buf_idx]->buf[j], PTE_U | PTE_R | PTE_W | PTE_X) < 0) break;
    }

    // mapping buffer failed, so cleanup before retry
    uvmunmap(pt, va, MIN(j+2,RINGBUF_SIZE+1), 0); // first copy; could have an extra page mapped before second copy fails.
    uvmunmap(pt, va+1+(RINGBUF_SIZE * PGSIZE), j, 0); // second copy
  }
  
  if(i == map_attempts){
    printf("could not find space for buffer.\n");
    release(&ringbuf_lock);
    return -1;
  }

  struct book *mybook = ringbufs[buf_idx]->book;

  // track pid and its mapped va
  mybook->pids[ringbufs[buf_idx]->refcount] = p->pid;
  mybook->vas[ringbufs[buf_idx]->refcount] = va;
  
  ringbufs[buf_idx]->refcount += 1;
  buffer_loc = va;

  release(&ringbuf_lock);
  return 0;
}

/* free ringbuf from user process
 *
 * @param 
 */
int
free_ringbuf(void){
  return -1;
}

/* attach to a ring buffer
 *
 * @param 
 */
int
attach(char *name, uint64 buffer_loc){
  int buf_idx = get_bufindex(name);

  if(ringbufs[buf_idx] == 0){
    init_ringbuf(name, buf_idx);
  }

  if(map_ringbuf(name, buffer_loc, buf_idx) < 0){
    printf("Mapping ring buffer failed\n");  
    return -1;
  }

  printf("done attaching.\n");
  return 0;
  
  /*
  


  */
}

/* detach from a ring buffer
 *
 * @param 
 */
int
detach(char *name){
//  int buf_index = get_bufindex(name);
//
//  acquire(&ringbuf_lock);
//  struct ringbuf *my_buf = ringbufs[buf_index];
//
//  uint64 my_va;
//  // check that this pid is attached, and get its local va.
//  for(i=0;i<2;i++){
//    if(my_buf->book->pids[i] == p->pid){
//      my_va = my_buf->book->vas[i];
//    break;
//    }
//  }
//  if(i == 2){
//    release(&ringbuf_lock);
//    return 0;
//  }
//
//  // refcount-1 makes a used buffer (refcount 1) into 0 (false), 
//  // or unused (refcount 0) into -1 (true) for freeing memory.
//  my_buf->refcount -= 1;
//  uvmunmap(pt, my_va, (2 * RINGBUF_SIZE)+1, my_buf->refcount-1);

  return -1; // incomplete
}

/* syscall to attach / detach a user process from ring buffer
 *
 * @param name:              char[16], ring buffer name identifier
 * @param attach_flag:       int,      0 detach, 1 attach.
 * @param buffer_loc:        **int,    set to address of a 64-bit location where the ring buffer was mapped to
 */
uint64
sys_ringbuf(void)
{
  char name[16];
  int attach_flag;
  uint64 buffer_loc;

  initlock(&ringbuf_lock, "ringbuf_lock");
  
  int name_len = argstr(0, name, 16);

  if(name_len < 0)
    return -1;
  if(argint(1, &attach_flag) < 0)
    return -1;
  if(argaddr(2, &buffer_loc) < 0)
    return -1;

  if(attach_flag == 1){
    printf("start attach\n");
    return attach(name, buffer_loc);
  }
  else if(attach_flag == 0){
    printf("start detach\n");
    return detach(name);
  }
  else{
    printf("invalid attach parameter: %d\n", attach);
    return -1;
  }
}
//
//    
//
//
//  if(attach == 0){ // attach
//    // It will acquire lock and map 16 pages
//    ///finds a free slot in ringbufs 
//    /// acquire lock
//
//    /**************************/
//    /* allocating with kalloc */
//    /**************************/
//    // TODO: Start here
//    
//    /*
//     *  map pages to uvm
//     */
//  }
//  else{ // detach
//    
//    
//
//    //free up memory;
//    //acquire lock
//    if(ringbufs[i].buf[j])
//      kfree((char*)ringbufs[i].buf[j]);
//    if(ringbufs[i].name)
//      kfree((char*)ringbufs[i].name);
//    if(ringbufs[i].book)
//      fileclose(ringbufs[i].book);
//    if(ringbufs[i].refcount)
//      ringbufs[i].refcount = 0;
//      //release lock 
//    release(&ringbuf_lock);
//  }
//
//  return 0;
