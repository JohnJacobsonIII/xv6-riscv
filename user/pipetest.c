#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int buf_size = 512; // pipe size
int send_size = 10 * (2 << 19); // 10 MB
//int send_size = 4096;
char buf[512];


void
pipetest()
{
  int p[2];

  pipe(p);
  
  if(fork() == 0) { // child
    // change stdout to pipes write side
    close(1);
    dup(p[1]);
    
    // don't need pipe descriptors now
    close(p[0]);
    close(p[1]);

    int total = 0;

    while(total <= send_size) {
      total = total + write(1, buf, buf_size);
    }
  } else { // parent process
    // change stdin to pipes read side
    close(0);
    dup(p[0]);
    
    // don't need pipe descriptors now
    close(p[0]);
    close(p[1]);

    int n;
    int time = uptime();

    // read from pipe
    while((n = read(0, buf, buf_size)) > 0) {
      for(int i=0;i<n;i++) { // validate data
        if(buf[i] != 'a' + (i % 26)) {
          printf("pipe error: invalid data at %d. ", i);
          printf("expected: %c, actual: %c\n", 'a' + (i%26), buf[i]);
          exit(1);  
        }
      }
    }
    
    if(n<0) {
      printf("pipetest: read error\n");
      exit(1);
    }

    time = uptime() - time;
    wait(0);
    
    printf("ticks: %d\n", time);

  }
}

int
main(int argc, char *argv[])
{
  int i;
	
  // init test array
  for(i=0;i<buf_size;i++) {
    // just rotating through english alphabet, why not.
    buf[i] = 'a' + (i%26);
  }

  pipetest();
  
  exit(0);
}
