/*
 *	BIRD -- I/O and event loop
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#ifndef _BIRD_SYSDEP_UNIX_IO_LOOP_H_
#define _BIRD_SYSDEP_UNIX_IO_LOOP_H_

#include "lib/rcu.h"

#include <pthread.h>

struct pipe
{
  int fd[2];
};

struct pfd {
  BUFFER(struct pollfd) pfd;
  BUFFER(struct birdloop *) loop;
};

void sockets_prepare(struct birdloop *, struct pfd *);
void socket_changed(struct birdsock *);

void pipe_new(struct pipe *);
void pipe_pollin(struct pipe *, struct pfd *);
void pipe_drain(struct pipe *);
void pipe_kick(struct pipe *);

#define TIME_BY_SEC_SIZE	16

struct spent_time {
  u64 total_ns;
  u64 last_written_ns;
  u64 by_sec_ns[TIME_BY_SEC_SIZE];
};

struct birdloop
{
  node n;

  event event;
  timer timer;

  pool *pool;

  struct timeloop time;
  event_list event_list;
  list sock_list;
  struct birdsock *sock_active;
  int sock_num;
  uint sock_changed:1;

  uint ping_pending;

  _Atomic u32 thread_transition;
#define LTT_PING  1
#define LTT_MOVE  2
  _Atomic u32 flags;
  struct birdloop_flag_handler *flag_handler;

  void (*stopped)(void *data);
  void *stop_data;

  struct birdloop *prev_loop;

  struct bird_thread *thread;

#define TIME_BY_SEC_SIZE	16
  struct spent_time working, locking;
};

struct bird_thread
{
  node n;

  struct pipe wakeup;
  event_list priority_events;

  struct birdloop *meta;

  pthread_t thread_id;
  pthread_attr_t thread_attr;

  struct rcu_thread rcu;

  list loops;
  struct birdloop_pickup_group *group;
  pool *pool;
  struct pfd *pfd;

  event cleanup_event;

  u8 sock_changed;
  u8 busy_active;
  u16 busy_counter;
  uint loop_count;

  u64 max_latency_ns;
  u64 max_loop_time_ns;

  struct spent_time overhead, idle;
};

#endif