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
// 
// [This is probably unnecessary and missing something easy, but the best
// I came up with for enforcing order of commits.]
//
// seq_nbr is an int between 0 and LOGCOPIES (inclusive) attached to each
// logheader. The idea is to treat seq_nbr as a ring of LOGCOPIES+1 entries,
// each being a committed log entry, and viewing the block of committed logs
// as a sliding window within the seq_nbr ring. Since committed entries 
// must be consecutive, we can identify next-to-commit on recovery by
// finding the lowest marked index in this ring
//
//
// For this setup with 4 copies, if two logs (in logs.logs) are in the committed
// state with seq_nbr 2 and 3 respectively, our view is as follows:
//
//  [ 0 | 0 | 1 | 1 | 0 ]
// 
// indicating that the first log to commit is that with seq_nbr 2 (and there are
// 2 logs waiting to be committed / installed.) Likewise
//
//  [ 1 | 0 | 1 | 1 | 1 ]
// 
// indicates the same (to commit a log with seq_nbr 2 first,) although there are 4 logs 
// awaiting commit here.
//
// There is not necessarily a relationship between seq_nbr and logs.logs index, seq_nbr's 
// must be tracked (or scanned on recover) to determine this sequencing.
//
// invariant: only 4 committed logs - should be maintained by logs.copies_committed, since begin_op 
// sleeps when this field is 4.
//
// invariant: committed logs consecutive - should be maintained by above and 
// by avoiding races on logs.active (so it is only ever incremented when a log is ready to commit.)
#define MAX_SEQ_NBR (LOGCOPIES+1)

struct logheader {
  int n;
  int block[LOGSIZE];
  int seq_nbr;
};

struct log {
  struct spinlock lock;
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
  int seq_nbr;               // current seq_nbr
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
    struct log log;
    initlock(&log.lock, "log");
    log.size = sb->nlog / LOGCOPIES;
    log.start = sb->logstart + (i * log.size);
    log.dev = dev;
    log.lh.seq_nbr = -1; // TODO: add checks for this

    logs.logs[i] = log;
  }
  release(&logs.lock); 

  recover_from_log();
}

// Copy committed blocks from log to their home location
static void
install_trans(int recovering, int lognum)
{
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
  acquire(&logs.lock);
  for (int lognum = 0; lognum < LOGCOPIES; lognum++) {
    log = &logs.logs[lognum];
    
    struct buf *buf = bread(log->dev, log->start + (lognum * log->size));
    lh = (struct logheader *) (buf->data);
    
    log->lh.n = lh->n;
    log->lh.seq_nbr = lh->seq_nbr;
    for (i = 0; i < log->lh.n; i++) {
      log->lh.block[i] = lh->block[i];
    }
    brelse(buf);
  }
  release(&logs.lock);
}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
// lognum is index in logs.logs array for log to be written.
static void
write_head(int lognum)
{
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
}

// Searches sequence numbers to find the first log
// index available for commit.
// Only used on recovery, so performance shouldn't be
// critical.
int
find_first_commit() {
  int first_idx = -1;
  int i;

  if(logs.logs[0].lh.n == 0){ // no wrapping, get smallest idx
    for(i=0;i<LOGCOPIES;i++){
      if(logs.logs[i].lh.n > 0){
        first_idx = i;
	break;
      }
    }
    // must have all empty logs, so start fresh.
    if(first_idx < 0){
      first_idx = 0;
    }
  } else { // either wraps, or all logs committed awaiting install
    for(i=LOGCOPIES-1;i>0;i--){
      // invariant: index 0 is committed, so search backwards
      if(logs.logs[i].lh.n == 0){
        first_idx = (i+1) % MAX_SEQ_NBR;
	break;
      }
    }
  }
  // if we haven't set a start yet, all logs are committed.
  // revert to seq_nbr to find first.
  // a gap of 2 should separate last and first entry.
  int next_log;
  if(first_idx < 0){
    for(i=0;i<LOGCOPIES;i++){
      next_log = (i+1) % LOGCOPIES;
      if(logs.logs[i].lh.seq_nbr + 1!= logs.logs[next_log].lh.seq_nbr){
        first_idx = next_log;
	break;
      }
    }
  }

  if(first_idx<0){
    panic("find first commit");
  }

  return first_idx;
}

static void
recover_from_log(void)
{
  read_head(); // reads all logs.

  // Below comment block should be unneeded, just start
  // in the right place and install everything

  // Don't think I need lock since probably no process can
  // start logging until this is done.
  // acquire(&logs.lock);

  //for(lognum = 0; lognum < LOGCOPIES; lognum++){
  //  log = &logs.logs[lognum];
  //  if(log->lh.n > 0){
  //    logs.copies_committed += 1;
  //  }
  //}
  // release(&logs.lock);

  // use find_next_commit to get starting point, then 
  // install everything sequentially.
  int lognum;
  for(int i = find_first_commit();i < LOGCOPIES;i++){
    lognum = i % LOGCOPIES;
    install_trans(1, lognum); // if committed, copy from log to disk
    logs.logs[lognum].lh.n = 0;
    write_head(lognum); // clear the log
  }
}

// called at the start of each FS system call.
void
begin_op(void)
{
  struct log *log; 
  acquire(&logs.lock);
  while(logs.copies_committed == 4){ // All logs committing, sleep
    sleep(&logs, &logs.lock);
  }
  // if this lock is acquired, logs.copies_committed cannot
  // ever be 4 since we also hold logs.lock
  log = &logs.logs[logs.active];
  acquire(&log->lock);
  while(1){
    if(log->committing){ // Move to next log
      logs.active = (logs.active + 1) % LOGCOPIES;
    } else if(log->lh.n + (log->outstanding+1)*MAXOPBLOCKS > LOGSIZE){
      // this op might exhaust log space; just move to next.
      logs.active = (logs.active + 1) % LOGCOPIES;
    } else {
      log->outstanding += 1;
      release(&log->lock);
      release(&logs.lock);
      break;
    }
    log = &logs.logs[logs.active];
  }
}

// called at the end of each FS system call.
// commits if this was the last outstanding operation.
void
end_op(void)
{
  int to_commit = -1;

  acquire(&logs.lock);
  struct log *log = &logs.logs[logs.active];
  
  acquire(&log->lock);
  log->outstanding -= 1;
  if(log->committing)
    panic("log.committing");
  if(log->outstanding == 0){
    to_commit = logs.active;
    log->committing = 1;
    log->lh.seq_nbr = logs.seq_nbr++;
    logs.copies_committed += 1;
  }
  release(&log->lock);
  release(&logs.lock);

  if(to_commit >= 0){
    // first be sure this is the right log to commit next
    acquire(&logs.lock);
    while(log->lh.seq_nbr + logs.copies_committed != logs.seq_nbr){
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
    wakeup(&logs);
    
    release(&log->lock);
    release(&logs.lock);
  }
}

// Copy modified blocks from cache to log.
static void
write_log(int lognum)
{
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
}

static void
commit(int lognum)
{
  // does this need a lock? intuitively no, but need to think...
  struct log *log = &logs.logs[lognum];
  
  if (log->lh.n > 0) {
    write_log(lognum);     // Write modified blocks from cache to log
    write_head(lognum);    // Write header to disk -- the real commit
    install_trans(0, lognum); // Now install writes to home locations
    log->lh.n = 0;
    write_head(lognum);    // Erase the transaction from the log
  }
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
  int i;

  // is a lock needed? we are modifying the log, 
  // but it's protected by its own lock
  // think lock is needed to ensure active log
  // isn't changed mid write.
  acquire(&logs.lock);
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
  release(&logs.lock);
}

