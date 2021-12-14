#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// Simple logging that allows concurrent FS system calls.
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
//
// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   header block, containing block #s for block A, B, C, ...
//   block A
//   block B
//   block C
//   ...
// Log appends are synchronous.


// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
struct logheader {
  int n;
  int block[LOGSIZE];
};

struct log {
  struct spinlock lock;
  int id;
  int start;
  int size;
  int outstanding; // how many FS sys calls are executing.
  int committing;  // this log instance ready for commit
  int dev;
  struct logheader lh;
};
// struct log log;

struct logs {
  struct log logs[LOGCOPIES];
  struct spinlock lock;
  int active;                // active log for writing
  int copies_committed;      // number of logs in committed status
};
struct logs logs;



static void recover_from_log(void);
static void commit();

void
initlog(int dev, struct superblock *sb)
{
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: too big logheader");

  initlock(&logs.lock, "logs");

  acquire(&logs.lock);
  for(int i=0;i<LOGCOPIES;i++){
    initlock(&logs.logs[i].lock, "log");
    logs.logs[i].size = sb->nlog / LOGCOPIES;
    logs.logs[i].start = sb->logstart + (i * logs.logs[i].size);
    logs.logs[i].dev = dev;
    logs.logs[i].id = i;
  }
  release(&logs.lock); 

  recover_from_log();
}

// Copy committed blocks from log to their home location
static void
install_trans(int recovering, int lognum)
{
  //printf("install trans\n");
  int tail;

  struct log *log = &logs.logs[lognum];

  for (tail = 0; tail < log->lh.n; tail++) {
    struct buf *lbuf = bread(log->dev, log->start+tail+1); // read log block
    struct buf *dbuf = bread(log->dev, log->lh.block[tail]); // read dst
    memmove(dbuf->data, lbuf->data, BSIZE);  // copy block to dst
    bwrite(dbuf);  // write dst to disk
    if(recovering == 0)
      bunpin(dbuf);
    brelse(lbuf);
    brelse(dbuf);
  }
  //printf("end install trans\n");
}

// Read the log header from disk into the in-memory log header
static void
read_head(void)
{
  struct logheader *lh;
  struct log *log;
  int i;

  // read all log copies sequentially.
  // will only be read on a recover, so no need for granularity.
  for (int lognum = 0; lognum < LOGCOPIES; lognum++) {
    log = &logs.logs[lognum];
    
    struct buf *buf = bread(log->dev, log->start + (lognum * log->size));
    lh = (struct logheader *) (buf->data);
    
    log->lh.n = lh->n;
    for (i = 0; i < log->lh.n; i++) {
      log->lh.block[i] = lh->block[i];
    }
    brelse(buf);
  }
}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
// lognum is index in logs.logs array for log to be written.
static void
write_head(int lognum)
{
  //printf("write head\n");
  struct log *log;
  log = &logs.logs[lognum];
  
  struct buf *buf = bread(log->dev, log->start + (lognum * log->size));
  struct logheader *hb = (struct logheader *) (buf->data);
  int i;
  
  hb->n = log->lh.n;
  
  for (i = 0; i < log->lh.n; i++) {
    hb->block[i] = log->lh.block[i];
  }
  bwrite(buf);
  brelse(buf);
  //printf("end write head\n");
}

static void
recover_from_log(void)
{
  read_head(); // reads all logs. only one should be on disk at any time.
  for(int lognum = 0;lognum < LOGCOPIES;lognum++){
    install_trans(1, lognum); // if committed, copy from log to disk
    logs.logs[lognum].lh.n = 0;
    write_head(lognum); // clear the log
  }
}

// called at the start of each FS system call.
void
begin_op(void)
{
  //printf("begin_op\n");
  struct log *log; 
  
  while(1){
    log = &logs.logs[logs.active];
    acquire(&log->lock);
    if(log->committing){ // Move to next log
      sleep(&log,&log->lock);
    } else if(log->lh.n + (log->outstanding+1)*MAXOPBLOCKS > LOGSIZE){
      // this op might exhaust log space; just move to next.
      sleep(&log,&log->lock);
    } else if(logs.copies_committed == LOGCOPIES) {
      sleep(&log,&log->lock);
    } else {
      //printf("incr outstanding: id %d outstanding %d\n", log->id, log->outstanding);
      log->outstanding += 1;
      release(&log->lock);
      break;
    }
    release(&log->lock);
  }
  //printf("end begin_op\n");
}

// called at the end of each FS system call.
// commits if this was the last outstanding operation.
void
end_op(void)
{
  //printf("end_op\n");
  int to_commit = -1;

  acquire(&logs.lock);
  struct log *log = &logs.logs[logs.active];
  
  acquire(&log->lock);
  //printf("decr outstanding: id %d outstanding %d\n", log->id, log->outstanding);
  log->outstanding -= 1;
  if(log->committing)
    panic("log.committing");
  if(log->outstanding == 0){
    to_commit = logs.active;
    logs.active = (logs.active + 1) % LOGCOPIES;
    //printf("active log %d\n", logs.active);
    log->committing = 1;
    logs.copies_committed += 1;

    //printf("wake %d\n", log->id);
    wakeup(&log);
  }
  release(&log->lock);
  release(&logs.lock);

  if(to_commit >= 0){
    // first be sure this is the right log to commit next
    acquire(&logs.lock);
    while((log->id + logs.copies_committed) % LOGCOPIES != logs.active){
      sleep(&logs,&logs.lock); 
    }
    release(&logs.lock);

    // call commit w/o holding locks, since not allowed
    // to sleep with locks.
    commit(to_commit);
    acquire(&logs.lock);
    acquire(&log->lock);

    log->committing = 0;
    logs.copies_committed -= 1;
    //printf("wake %d\n", log->id);
    wakeup(&log);
    wakeup(&logs);
    
    release(&log->lock);
    release(&logs.lock);
  }
  //printf("end end_op\n");
}

// Copy modified blocks from cache to log.
static void
write_log(int lognum)
{
  //printf("write log\n");
  // does this need a lock? intuitively no, but need to think...
  struct log *log = &logs.logs[lognum];
  int tail;
  
  for (tail = 0; tail < log->lh.n; tail++) {
    struct buf *to = bread(log->dev, log->start+tail+1); // log block
    struct buf *from = bread(log->dev, log->lh.block[tail]); // cache block
    memmove(to->data, from->data, BSIZE);
    bwrite(to);  // write the log
    brelse(from);
    brelse(to);
  }
  //printf("end write log\n");
}

static void
commit(int lognum)
{
  //printf("commit\n");
  // does this need a lock? intuitively no, but need to think...
  struct log *log = &logs.logs[lognum];

  if (log->lh.n > 0) {
    write_log(lognum);     // Write modified blocks from cache to log
    write_head(lognum);    // Write header to disk -- the real commit
    install_trans(0, lognum); // Now install writes to home locations
    log->lh.n = 0;
    write_head(lognum);    // Erase the transaction from the log
  }
  //printf("end commit\n");
}

// Caller has modified b->data and is done with the buffer.
// Record the block number and pin in the cache by increasing refcnt.
// commit()/write_log() will do the disk write.
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
void
log_write(struct buf *b)
{
  //printf("log_write\n");
  int i;
  struct log *log = &logs.logs[logs.active];
  
  acquire(&log->lock);
  if (log->lh.n >= LOGSIZE || log->lh.n >= log->size - 1)
    panic("too big a transaction");
  if (log->outstanding < 1)
    panic("log_write outside of trans");

  for (i = 0; i < log->lh.n; i++) {
    if (log->lh.block[i] == b->blockno)   // log absorption
      break;
  }
  log->lh.block[i] = b->blockno;
  if (i == log->lh.n) {  // Add new block to log?
    bpin(b);
    log->lh.n++;
  }
  release(&log->lock);
  //printf("end log_write\n");
}

