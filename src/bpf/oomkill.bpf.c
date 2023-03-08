#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "oomkill.h"

// some parts of the kernel can only be accessed via gpl'ed code
char LICENSE[] SEC("license") = "Dual BSD/GPL";

// Dummy instance to get skel builder to give us a definition of our event
struct event _event = {0};

struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 8192);
  __type(key, pid_t);
  __type(value, u64);
} oom_kill SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_RINGBUF);
  __uint(max_entries, 256 * 1024);
} rb SEC(".maps");


// macro to define our bpf program
// see https://www.kernel.org/doc/html/latest/bpf/libbpf/program_types.html
// in this case we're using the tracepoint type
// sudo find /sys/kernel/tracing/events -type d | rg "oom"
// we're gonna trace marking a process to be oom killed
// /sys/kernel/tracing/events/oom/mark_victim
// the trace point is a bit grim isn't it :(
SEC("tp/oom/mark_victim")
int handle_tp(struct  trace_event_raw_mark_victim *ctx) {
  // the task struct has a bunch of things like threads and the stack. it has a lot of info that is helpfully generated by bpftool
  struct task_struct *task;
  struct event *e;
  pid_t pid;
  u64 ts;
  //pid is in the upper 32 bits and the thread id is in the lower 32 bits so we
  // toss those out just for the pid
  pid = bpf_get_current_pid_tgid() >> 32;
  // get timestamp
  ts = bpf_ktime_get_ns();
  // add entry to our bpf map
  bpf_map_update_elem(&oom_kill, &pid, &ts, BPF_ANY);

  // reserve space in our ring buffer
  e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);

  // if we couldn't get space bail!
  if(!e) {
    return 0;
  } 

  // cast the random ass u64 we get as a pointer to a task struct. don't worry kids we're gonna do way more memory crimes.
  task = (struct task_struct *)bpf_get_current_task();

  // ok now we're gonna do a read from this struct using the read methods. this let's us not have to fuss with struct reordering.
  e->pid = pid;
  e->ppid = BPF_CORE_READ(task, real_parent, tgid);
  e->highwater_rss = BPF_CORE_READ(task, mm, hiwater_rss);
  e->exit_code = BPF_CORE_READ(task, exit_code);

  // yeet our event into userspace

  bpf_ringbuf_submit(e, 0);
  return 0;
  
}