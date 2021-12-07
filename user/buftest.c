/*
 * Ring buffer - User Test Program
 *
 * Notes:
 *   - Relevant files for syscall definition: syscall.{c,h}, sysproc.c, user.h
 *
 *
 *
 */
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/*
 * @param name:    char[16], ring buffer name identifier
 * @param attach:  int,      false (0) then detach, true attach.
 * @param loc:     **int,    set to address of a 64-bit location where the ring buffer was mapped to 
 */
int
main(int argc, char *argv[])
{
  char *name = "test_buf";
  int attach = 1;
  uint64 **buffer_loc = malloc(sizeof(uint64));

  printf("sys_ringbuf return: %d\n", ringbuf(name, attach, buffer_loc));

  exit(0);
}
