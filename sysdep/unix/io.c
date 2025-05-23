/*
 *	BIRD Internet Routing Daemon -- Unix I/O
 *
 *	(c) 1998--2004 Martin Mares <mj@ucw.cz>
 *      (c) 2004       Ondrej Filip <feela@network.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

/* Unfortunately, some glibc versions hide parts of RFC 3542 API
   if _GNU_SOURCE is not defined. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef __APPLE__
#define __APPLE_USE_RFC_3542
#endif

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netinet/icmp6.h>
#include <netdb.h>

#include "nest/bird.h"
#include "lib/lists.h"
#include "lib/resource.h"
#include "lib/socket.h"
#include "lib/event.h"
#include "lib/timer.h"
#include "lib/string.h"
#include "lib/mac.h"
#include "nest/iface.h"
#include "nest/cli.h"
#include "conf/conf.h"

#include "sysdep/unix/unix.h"
#include CONFIG_INCLUDE_SYSIO_H

/* Maximum number of calls of tx handler for one socket in one
 * poll iteration. Should be small enough to not monopolize CPU by
 * one protocol instance.
 */
#define MAX_STEPS 4

/* Maximum number of calls of rx handler for all sockets in one poll
   iteration. RX callbacks are often much more costly so we limit
   this to gen small latencies */
#define MAX_RX_STEPS 4


/*
 *	Tracked Files
 */

struct rfile {
  resource r;
  FILE *f;
};

static void
rf_free(resource *r)
{
  struct rfile *a = (struct rfile *) r;

  fclose(a->f);
}

static void
rf_dump(struct dump_request *dreq, resource *r)
{
  struct rfile *a = (struct rfile *) r;

  RDUMP("(FILE *%p)\n", a->f);
}

static struct resclass rf_class = {
  "FILE",
  sizeof(struct rfile),
  rf_free,
  rf_dump,
  NULL,
  NULL
};

struct rfile *
rf_open(pool *p, const char *name, const char *mode)
{
  FILE *f = fopen(name, mode);
  if (!f)
    return NULL;

  struct rfile *r = ralloc(p, &rf_class);
  r->f = f;

  return r;
}

struct rfile *
rf_fdopen(pool *p, int fd, const char *mode)
{
  FILE *f = fdopen(fd, mode);
  if (!f)
    return NULL;

  struct rfile *r = ralloc(p, &rf_class);
  r->f = f;

  return r;
}

void *
rf_file(struct rfile *f)
{
  return f->f;
}

int
rf_fileno(struct rfile *f)
{
  return fileno(f->f);
}

/*
 *	Dumping to files
 */

struct dump_request_file {
  struct dump_request dr;
  uint pos, max; int fd;
  uint last_progress_info;
  char data[0];
};

static void
dump_to_file_flush(struct dump_request_file *req)
{
  if (req->fd < 0)
    return;

  for (uint sent = 0; sent < req->pos; )
  {
    int e = write(req->fd, &req->data[sent], req->pos - sent);
    if (e <= 0)
    {
      req->dr.report(&req->dr, 8009, "Failed to write data: %m");
      close(req->fd);
      req->fd = -1;
      return;
    }
    sent += e;
  }

  req->dr.size += req->pos;
  req->pos = 0;

  for (uint reported = 0; req->dr.size >> req->last_progress_info; req->last_progress_info++)
    if (!reported++)
      req->dr.report(&req->dr, -13, "... dumped %lu bytes in %t s",
	  req->dr.size, current_time_now() - req->dr.begin);
}

static void
dump_to_file_write(struct dump_request *dr, const char *fmt, ...)
{
  struct dump_request_file *req = SKIP_BACK(struct dump_request_file, dr, dr);

  for (uint phase = 0; (req->fd >= 0) && (phase < 2); phase++)
  {
    va_list args;
    va_start(args, fmt);
    int i = bvsnprintf(&req->data[req->pos], req->max - req->pos, fmt, args);
    va_end(args);

    if (i >= 0)
    {
      req->pos += i;
      return;
    }
    else
      dump_to_file_flush(req);
  }

  bug("Too long dump call");
}

struct dump_request *
dump_to_file_init(off_t offset)
{
  ASSERT_DIE(offset + sizeof(struct dump_request_file) + 1024 < (unsigned long) page_size);

  struct dump_request_file *req = alloc_page() + offset;
  *req = (struct dump_request_file) {
    .dr = {
      .write = dump_to_file_write,
      .begin = current_time_now(),
      .offset = offset,
    },
    .max = page_size - offset - OFFSETOF(struct dump_request_file, data[0]),
    .fd = -1,
  };

  return &req->dr;
}

void
dump_to_file_run(struct dump_request *dr, const char *file, const char *what, void (*dump)(struct dump_request *))
{
  struct dump_request_file *req = SKIP_BACK(struct dump_request_file, dr, dr);
  req->fd = open(file, O_CREAT | O_WRONLY | O_EXCL, S_IRUSR);

  if (req->fd < 0)
  {
    dr->report(dr, 8009, "Failed to open file %s: %m", file);
    goto cleanup;
  }

  dr->report(dr, -13, "Dumping %s to %s", what, file);

  dump(dr);

  if (req->fd >= 0)
  {
    dump_to_file_flush(req);
    close(req->fd);
  }

  btime end = current_time_now();
  dr->report(dr, 13, "Dumped %lu bytes in %t s", dr->size, end - dr->begin);

cleanup:
  free_page(((void *) req) - dr->offset);
}

struct dump_request_cli {
  cli *cli;
  struct dump_request dr;
};

static void
cmd_dump_report(struct dump_request *dr, int state, const char *fmt, ...)
{
  struct dump_request_cli *req = SKIP_BACK(struct dump_request_cli, dr, dr);
  va_list args;
  va_start(args, fmt);
  cli_vprintf(req->cli, state, fmt, args);
  va_end(args);
}

void
cmd_dump_file(struct cli *cli, const char *file, const char *what, void (*dump)(struct dump_request *))
{
  if (cli->restricted)
    return cli_printf(cli, 8007, "Access denied");

  struct dump_request_cli *req = SKIP_BACK(struct dump_request_cli, dr,
      dump_to_file_init(OFFSETOF(struct dump_request_cli, dr)));

  req->cli = cli;
  req->dr.report = cmd_dump_report;

  dump_to_file_run(&req->dr, file, what, dump);
}


/*
 *	Time clock
 */

btime boot_time;

void
times_init(struct timeloop *loop)
{
  struct timespec ts;
  int rv;

  rv = clock_gettime(CLOCK_MONOTONIC, &ts);
  if (rv < 0)
    die("Monotonic clock is missing");

  if ((ts.tv_sec < 0) || (((u64) ts.tv_sec) > ((u64) 1 << 40)))
    log(L_WARN "Monotonic clock is crazy");

  loop->last_time = ts.tv_sec S + ts.tv_nsec NS;
  loop->real_time = 0;
}

void
times_update(struct timeloop *loop)
{
  struct timespec ts;
  int rv;

  rv = clock_gettime(CLOCK_MONOTONIC, &ts);
  if (rv < 0)
    die("clock_gettime: %m");

  btime new_time = ts.tv_sec S + ts.tv_nsec NS;

  if (new_time < loop->last_time)
    log(L_ERR "Monotonic clock is broken");

  loop->last_time = new_time;
  loop->real_time = 0;
}

void
times_update_real_time(struct timeloop *loop)
{
  struct timespec ts;
  int rv;

  rv = clock_gettime(CLOCK_REALTIME, &ts);
  if (rv < 0)
    die("clock_gettime: %m");

  loop->real_time = ts.tv_sec S + ts.tv_nsec NS;
}

btime
current_time_now(void)
{
  struct timespec ts;
  int rv;

  rv = clock_gettime(CLOCK_MONOTONIC, &ts);
  if (rv < 0)
    die("clock_gettime: %m");

  return ts.tv_sec S + ts.tv_nsec NS;
}


/**
 * DOC: Sockets
 *
 * Socket resources represent network connections. Their data structure (&socket)
 * contains a lot of fields defining the exact type of the socket, the local and
 * remote addresses and ports, pointers to socket buffers and finally pointers to
 * hook functions to be called when new data have arrived to the receive buffer
 * (@rx_hook), when the contents of the transmit buffer have been transmitted
 * (@tx_hook) and when an error or connection close occurs (@err_hook).
 *
 * Freeing of sockets from inside socket hooks is perfectly safe.
 */

#ifndef SOL_IP
#define SOL_IP IPPROTO_IP
#endif

#ifndef SOL_IPV6
#define SOL_IPV6 IPPROTO_IPV6
#endif

#ifndef SOL_ICMPV6
#define SOL_ICMPV6 IPPROTO_ICMPV6
#endif


/*
 *	Sockaddr helper functions
 */

static inline int UNUSED sockaddr_length(int af)
{ return (af == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6); }

static inline void
sockaddr_fill4(struct sockaddr_in *sa, ip_addr a, uint port)
{
  memset(sa, 0, sizeof(struct sockaddr_in));
#ifdef HAVE_STRUCT_SOCKADDR_SA_LEN
  sa->sin_len = sizeof(struct sockaddr_in);
#endif
  sa->sin_family = AF_INET;
  sa->sin_port = htons(port);
  sa->sin_addr = ipa_to_in4(a);
}

static inline void
sockaddr_fill6(struct sockaddr_in6 *sa, ip_addr a, struct iface *ifa, uint port)
{
  memset(sa, 0, sizeof(struct sockaddr_in6));
#ifdef SIN6_LEN
  sa->sin6_len = sizeof(struct sockaddr_in6);
#endif
  sa->sin6_family = AF_INET6;
  sa->sin6_port = htons(port);
  sa->sin6_flowinfo = 0;
  sa->sin6_addr = ipa_to_in6(a);

  if (ifa && ipa_is_link_local(a))
    sa->sin6_scope_id = ifa->index;
}

void
sockaddr_fill(sockaddr *sa, int af, ip_addr a, struct iface *ifa, uint port)
{
  if (af == AF_INET)
    sockaddr_fill4((struct sockaddr_in *) sa, a, port);
  else if (af == AF_INET6)
    sockaddr_fill6((struct sockaddr_in6 *) sa, a, ifa, port);
  else
    bug("Unknown AF");
}

static inline void
sockaddr_read4(struct sockaddr_in *sa, ip_addr *a, uint *port)
{
  *port = ntohs(sa->sin_port);
  *a = ipa_from_in4(sa->sin_addr);
}

static inline void
sockaddr_read6(struct sockaddr_in6 *sa, ip_addr *a, struct iface **ifa, uint *port)
{
  *port = ntohs(sa->sin6_port);
  *a = ipa_from_in6(sa->sin6_addr);

  if (ifa && ipa_is_link_local(*a))
    *ifa = if_find_by_index(sa->sin6_scope_id);
}

int
sockaddr_read(sockaddr *sa, int af, ip_addr *a, struct iface **ifa, uint *port)
{
  if (sa->sa.sa_family != af)
    goto fail;

  if (af == AF_INET)
    sockaddr_read4((struct sockaddr_in *) sa, a, port);
  else if (af == AF_INET6)
    sockaddr_read6((struct sockaddr_in6 *) sa, a, ifa, port);
  else
    goto fail;

  return 0;

 fail:
  *a = IPA_NONE;
  *port = 0;
  return -1;
}


/*
 *	IPv6 multicast syscalls
 */

/* Fortunately standardized in RFC 3493 */

#define INIT_MREQ6(maddr,ifa) \
  { .ipv6mr_multiaddr = ipa_to_in6(maddr), .ipv6mr_interface = ifa->index }

static inline int
sk_setup_multicast6(sock *s)
{
  int index = s->iface->index;
  int ttl = s->ttl;
  int n = 0;

  if (setsockopt(s->fd, SOL_IPV6, IPV6_MULTICAST_IF, &index, sizeof(index)) < 0)
    ERR("IPV6_MULTICAST_IF");

  if (setsockopt(s->fd, SOL_IPV6, IPV6_MULTICAST_HOPS, &ttl, sizeof(ttl)) < 0)
    ERR("IPV6_MULTICAST_HOPS");

  if (setsockopt(s->fd, SOL_IPV6, IPV6_MULTICAST_LOOP, &n, sizeof(n)) < 0)
    ERR("IPV6_MULTICAST_LOOP");

  return 0;
}

static inline int
sk_join_group6(sock *s, ip_addr maddr)
{
  struct ipv6_mreq mr = INIT_MREQ6(maddr, s->iface);

  if (setsockopt(s->fd, SOL_IPV6, IPV6_JOIN_GROUP, &mr, sizeof(mr)) < 0)
    ERR("IPV6_JOIN_GROUP");

  return 0;
}

static inline int
sk_leave_group6(sock *s, ip_addr maddr)
{
  struct ipv6_mreq mr = INIT_MREQ6(maddr, s->iface);

  if (setsockopt(s->fd, SOL_IPV6, IPV6_LEAVE_GROUP, &mr, sizeof(mr)) < 0)
    ERR("IPV6_LEAVE_GROUP");

  return 0;
}


/*
 *	IPv6 packet control messages
 */

/* Also standardized, in RFC 3542 */

/*
 * RFC 2292 uses IPV6_PKTINFO for both the socket option and the cmsg
 * type, RFC 3542 changed the socket option to IPV6_RECVPKTINFO. If we
 * don't have IPV6_RECVPKTINFO we suppose the OS implements the older
 * RFC and we use IPV6_PKTINFO.
 */
#ifndef IPV6_RECVPKTINFO
#define IPV6_RECVPKTINFO IPV6_PKTINFO
#endif
/*
 * Same goes for IPV6_HOPLIMIT -> IPV6_RECVHOPLIMIT.
 */
#ifndef IPV6_RECVHOPLIMIT
#define IPV6_RECVHOPLIMIT IPV6_HOPLIMIT
#endif


#define CMSG6_SPACE_PKTINFO CMSG_SPACE(sizeof(struct in6_pktinfo))
#define CMSG6_SPACE_TTL CMSG_SPACE(sizeof(int))

static inline int
sk_request_cmsg6_pktinfo(sock *s)
{
  int y = 1;

  if (setsockopt(s->fd, SOL_IPV6, IPV6_RECVPKTINFO, &y, sizeof(y)) < 0)
    ERR("IPV6_RECVPKTINFO");

  return 0;
}

static inline int
sk_request_cmsg6_ttl(sock *s)
{
  int y = 1;

  if (setsockopt(s->fd, SOL_IPV6, IPV6_RECVHOPLIMIT, &y, sizeof(y)) < 0)
    ERR("IPV6_RECVHOPLIMIT");

  return 0;
}

static inline void
sk_process_cmsg6_pktinfo(sock *s, struct cmsghdr *cm)
{
  if (cm->cmsg_type == IPV6_PKTINFO)
  {
    struct in6_pktinfo *pi = (struct in6_pktinfo *) CMSG_DATA(cm);
    s->laddr = ipa_from_in6(pi->ipi6_addr);
    s->lifindex = pi->ipi6_ifindex;
  }
}

static inline void
sk_process_cmsg6_ttl(sock *s, struct cmsghdr *cm)
{
  if (cm->cmsg_type == IPV6_HOPLIMIT)
    s->rcv_ttl = * (int *) CMSG_DATA(cm);
}

static inline void
sk_prepare_cmsgs6(sock *s, struct msghdr *msg, void *cbuf, size_t cbuflen)
{
  struct cmsghdr *cm;
  struct in6_pktinfo *pi;
  int controllen = 0;

  msg->msg_control = cbuf;
  msg->msg_controllen = cbuflen;

  cm = CMSG_FIRSTHDR(msg);
  cm->cmsg_level = SOL_IPV6;
  cm->cmsg_type = IPV6_PKTINFO;
  cm->cmsg_len = CMSG_LEN(sizeof(*pi));
  controllen += CMSG_SPACE(sizeof(*pi));

  pi = (struct in6_pktinfo *) CMSG_DATA(cm);
  pi->ipi6_ifindex = s->iface ? s->iface->index : 0;
  pi->ipi6_addr = ipa_to_in6(s->saddr);

  msg->msg_controllen = controllen;
}


/*
 *	Miscellaneous socket syscalls
 */

static inline int
sk_set_ttl4(sock *s, int ttl)
{
  if (setsockopt(s->fd, SOL_IP, IP_TTL, &ttl, sizeof(ttl)) < 0)
    ERR("IP_TTL");

  return 0;
}

static inline int
sk_set_ttl6(sock *s, int ttl)
{
  if (setsockopt(s->fd, SOL_IPV6, IPV6_UNICAST_HOPS, &ttl, sizeof(ttl)) < 0)
    ERR("IPV6_UNICAST_HOPS");

  return 0;
}

static inline int
sk_set_tos4(sock *s, int tos)
{
  if (setsockopt(s->fd, SOL_IP, IP_TOS, &tos, sizeof(tos)) < 0)
    ERR("IP_TOS");

  return 0;
}

static inline int
sk_set_tos6(sock *s, int tos)
{
  if (setsockopt(s->fd, SOL_IPV6, IPV6_TCLASS, &tos, sizeof(tos)) < 0)
    ERR("IPV6_TCLASS");

  return 0;
}

static inline int
sk_set_high_port(sock *s UNUSED)
{
  /* Port range setting is optional, ignore it if not supported */

#ifdef IP_PORTRANGE
  if (sk_is_ipv4(s))
  {
    int range = IP_PORTRANGE_HIGH;
    if (setsockopt(s->fd, SOL_IP, IP_PORTRANGE, &range, sizeof(range)) < 0)
      ERR("IP_PORTRANGE");
  }
#endif

#ifdef IPV6_PORTRANGE
  if (sk_is_ipv6(s))
  {
    int range = IPV6_PORTRANGE_HIGH;
    if (setsockopt(s->fd, SOL_IPV6, IPV6_PORTRANGE, &range, sizeof(range)) < 0)
      ERR("IPV6_PORTRANGE");
  }
#endif

  return 0;
}

static inline int
sk_set_min_rcvbuf_(sock *s, int bufsize)
{
  int oldsize = 0, oldsize_s = sizeof(oldsize);

  if (getsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &oldsize, &oldsize_s) < 0)
    ERR("SO_RCVBUF");

  if (oldsize >= bufsize)
    return 0;

  bufsize = BIRD_ALIGN(bufsize, 64);
  if (setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize)) < 0)
    ERR("SO_RCVBUF");

  /*
  int newsize = 0, newsize_s = sizeof(newsize);
  if (getsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &newsize, &newsize_s) < 0)
    ERR("SO_RCVBUF");

  log(L_INFO "Setting rcvbuf on %s from %d to %d",
      s->iface ? s->iface->name : "*", oldsize, newsize);
  */

  return 0;
}

static void
sk_set_min_rcvbuf(sock *s, int bufsize)
{
  if (sk_set_min_rcvbuf_(s, bufsize) < 0)
    log(L_WARN "Socket error: %s%#m", s->err);
}

static inline byte *
sk_skip_ip_header(byte *pkt, int *len)
{
  if ((*len < 20) || ((*pkt & 0xf0) != 0x40))
    return NULL;

  int hlen = (*pkt & 0x0f) * 4;
  if ((hlen < 20) || (hlen > *len))
    return NULL;

  *len -= hlen;
  return pkt + hlen;
}

byte *
sk_rx_buffer(sock *s, int *len)
{
  if (sk_is_ipv4(s) && (s->type == SK_IP))
    return sk_skip_ip_header(s->rbuf, len);
  else
    return s->rbuf;
}


/*
 *	Public socket functions
 */

/**
 * sk_setup_multicast - enable multicast for given socket
 * @s: socket
 *
 * Prepare transmission of multicast packets for given datagram socket.
 * The socket must have defined @iface.
 *
 * Result: 0 for success, -1 for an error.
 */

int
sk_setup_multicast(sock *s)
{
  ASSERT(s->iface);

  if (sk_is_ipv4(s))
    return sk_setup_multicast4(s);
  else
    return sk_setup_multicast6(s);
}

/**
 * sk_join_group - join multicast group for given socket
 * @s: socket
 * @maddr: multicast address
 *
 * Join multicast group for given datagram socket and associated interface.
 * The socket must have defined @iface.
 *
 * Result: 0 for success, -1 for an error.
 */

int
sk_join_group(sock *s, ip_addr maddr)
{
  if (sk_is_ipv4(s))
    return sk_join_group4(s, maddr);
  else
    return sk_join_group6(s, maddr);
}

/**
 * sk_leave_group - leave multicast group for given socket
 * @s: socket
 * @maddr: multicast address
 *
 * Leave multicast group for given datagram socket and associated interface.
 * The socket must have defined @iface.
 *
 * Result: 0 for success, -1 for an error.
 */

int
sk_leave_group(sock *s, ip_addr maddr)
{
  if (sk_is_ipv4(s))
    return sk_leave_group4(s, maddr);
  else
    return sk_leave_group6(s, maddr);
}

/**
 * sk_setup_broadcast - enable broadcast for given socket
 * @s: socket
 *
 * Allow reception and transmission of broadcast packets for given datagram
 * socket. The socket must have defined @iface. For transmission, packets should
 * be send to @brd address of @iface.
 *
 * Result: 0 for success, -1 for an error.
 */

int
sk_setup_broadcast(sock *s)
{
  int y = 1;

  if (setsockopt(s->fd, SOL_SOCKET, SO_BROADCAST, &y, sizeof(y)) < 0)
    ERR("SO_BROADCAST");

  return 0;
}

/**
 * sk_set_ttl - set transmit TTL for given socket
 * @s: socket
 * @ttl: TTL value
 *
 * Set TTL for already opened connections when TTL was not set before. Useful
 * for accepted connections when different ones should have different TTL.
 *
 * Result: 0 for success, -1 for an error.
 */

int
sk_set_ttl(sock *s, int ttl)
{
  s->ttl = ttl;

  if (sk_is_ipv4(s))
    return sk_set_ttl4(s, ttl);
  else
    return sk_set_ttl6(s, ttl);
}

/**
 * sk_set_min_ttl - set minimal accepted TTL for given socket
 * @s: socket
 * @ttl: TTL value
 *
 * Set minimal accepted TTL for given socket. Can be used for TTL security.
 * implementations.
 *
 * Result: 0 for success, -1 for an error.
 */

int
sk_set_min_ttl(sock *s, int ttl)
{
  if (sk_is_ipv4(s))
    return sk_set_min_ttl4(s, ttl);
  else
    return sk_set_min_ttl6(s, ttl);
}

#if 0
/**
 * sk_set_md5_auth - add / remove MD5 security association for given socket
 * @s: socket
 * @local: IP address of local side
 * @remote: IP address of remote side
 * @ifa: Interface for link-local IP address
 * @passwd: Password used for MD5 authentication
 * @setkey: Update also system SA/SP database
 *
 * In TCP MD5 handling code in kernel, there is a set of security associations
 * used for choosing password and other authentication parameters according to
 * the local and remote address. This function is useful for listening socket,
 * for active sockets it may be enough to set s->password field.
 *
 * When called with passwd != NULL, the new pair is added,
 * When called with passwd == NULL, the existing pair is removed.
 *
 * Note that while in Linux, the MD5 SAs are specific to socket, in BSD they are
 * stored in global SA/SP database (but the behavior also must be enabled on
 * per-socket basis). In case of multiple sockets to the same neighbor, the
 * socket-specific state must be configured for each socket while global state
 * just once per src-dst pair. The @setkey argument controls whether the global
 * state (SA/SP database) is also updated.
 *
 * Result: 0 for success, -1 for an error.
 */

int
sk_set_md5_auth(sock *s, ip_addr local, ip_addr remote, struct iface *ifa, char *passwd, int setkey)
{ DUMMY; }
#endif

/**
 * sk_set_ipv6_checksum - specify IPv6 checksum offset for given socket
 * @s: socket
 * @offset: offset
 *
 * Specify IPv6 checksum field offset for given raw IPv6 socket. After that, the
 * kernel will automatically fill it for outgoing packets and check it for
 * incoming packets. Should not be used on ICMPv6 sockets, where the position is
 * known to the kernel.
 *
 * Result: 0 for success, -1 for an error.
 */

int
sk_set_ipv6_checksum(sock *s, int offset)
{
  if (setsockopt(s->fd, SOL_IPV6, IPV6_CHECKSUM, &offset, sizeof(offset)) < 0)
    ERR("IPV6_CHECKSUM");

  return 0;
}

int
sk_set_icmp6_filter(sock *s, int p1, int p2)
{
  /* a bit of lame interface, but it is here only for Radv */
  struct icmp6_filter f;

  ICMP6_FILTER_SETBLOCKALL(&f);
  ICMP6_FILTER_SETPASS(p1, &f);
  ICMP6_FILTER_SETPASS(p2, &f);

  if (setsockopt(s->fd, SOL_ICMPV6, ICMP6_FILTER, &f, sizeof(f)) < 0)
    ERR("ICMP6_FILTER");

  return 0;
}

void
sk_log_error(sock *s, const char *p)
{
  log(L_ERR "%s: Socket error: %s%#m", p, s->err);
}


/*
 *	Actual struct birdsock code
 */

static list sock_list;
static struct birdsock *current_sock;
static struct birdsock *stored_sock;

static inline sock *
sk_next(sock *s)
{
  if (!s->n.next->next)
    return NULL;
  else
    return SKIP_BACK(sock, n, s->n.next);
}

static void
sk_alloc_bufs(sock *s)
{
  if (!s->rbuf && s->rbsize)
    s->rbuf = s->rbuf_alloc = xmalloc(s->rbsize);
  s->rpos = s->rbuf;
  if (!s->tbuf && s->tbsize)
    s->tbuf = s->tbuf_alloc = xmalloc(s->tbsize);
  s->tpos = s->ttx = s->tbuf;
}

static void
sk_free_bufs(sock *s)
{
  if (s->rbuf_alloc)
  {
    xfree(s->rbuf_alloc);
    s->rbuf = s->rbuf_alloc = NULL;
  }
  if (s->tbuf_alloc)
  {
    xfree(s->tbuf_alloc);
    s->tbuf = s->tbuf_alloc = NULL;
  }
}

#ifdef HAVE_LIBSSH
static void
sk_ssh_free(sock *s)
{
  struct ssh_sock *ssh = s->ssh;

  if (s->ssh == NULL)
    return;

  s->ssh = NULL;

  if (ssh->channel)
  {
    if (ssh_channel_is_open(ssh->channel))
      ssh_channel_close(ssh->channel);
    ssh_channel_free(ssh->channel);
    ssh->channel = NULL;
  }

  if (ssh->session)
  {
    ssh_disconnect(ssh->session);
    ssh_free(ssh->session);
    ssh->session = NULL;
  }
}
#endif

static void
sk_free(resource *r)
{
  sock *s = (sock *) r;

  sk_free_bufs(s);

#ifdef HAVE_LIBSSH
  if (s->type == SK_SSH || s->type == SK_SSH_ACTIVE)
    sk_ssh_free(s);
#endif

  if (s->fd < 0)
    return;

  /* FIXME: we should call sk_stop() for SKF_THREAD sockets */
  if (!(s->flags & SKF_THREAD))
  {
    if (s == current_sock)
      current_sock = sk_next(s);
    if (s == stored_sock)
      stored_sock = sk_next(s);
    rem_node(&s->n);
  }

  if (s->type != SK_SSH && s->type != SK_SSH_ACTIVE)
    close(s->fd);

  s->fd = -1;
}

void
sk_set_rbsize(sock *s, uint val)
{
  ASSERT(s->rbuf_alloc == s->rbuf);

  if (s->rbsize == val)
    return;

  s->rbsize = val;
  xfree(s->rbuf_alloc);
  s->rbuf_alloc = xmalloc(val);
  s->rpos = s->rbuf = s->rbuf_alloc;

  if ((s->type == SK_UDP) || (s->type == SK_IP))
    sk_set_min_rcvbuf(s, s->rbsize);
}

void
sk_set_tbsize(sock *s, uint val)
{
  ASSERT(s->tbuf_alloc == s->tbuf);

  if (s->tbsize == val)
    return;

  byte *old_tbuf = s->tbuf;

  s->tbsize = val;
  s->tbuf = s->tbuf_alloc = xrealloc(s->tbuf_alloc, val);
  s->tpos = s->tbuf + (s->tpos - old_tbuf);
  s->ttx  = s->tbuf + (s->ttx  - old_tbuf);
}

void
sk_set_tbuf(sock *s, void *tbuf)
{
  s->tbuf = tbuf ?: s->tbuf_alloc;
  s->ttx = s->tpos = s->tbuf;
}

void
sk_reallocate(sock *s)
{
  sk_free_bufs(s);
  sk_alloc_bufs(s);
}

static void
sk_dump(struct dump_request *dreq, resource *r)
{
  sock *s = (sock *) r;
  static char *sk_type_names[] = { "TCP<", "TCP>", "TCP", "UDP", NULL, "IP", NULL, "MAGIC", "UNIX<", "UNIX", "SSH>", "SSH", "DEL!" };

  RDUMP("(%s, ud=%p, sa=%I, sp=%d, da=%I, dp=%d, tos=%d, ttl=%d, if=%s)\n",
	sk_type_names[s->type],
	s->data,
	s->saddr,
	s->sport,
	s->daddr,
	s->dport,
	s->tos,
	s->ttl,
	s->iface ? s->iface->name : "none");
}

static struct resclass sk_class = {
  "Socket",
  sizeof(sock),
  sk_free,
  sk_dump,
  NULL,
  NULL
};

/**
 * sk_new - create a socket
 * @p: pool
 *
 * This function creates a new socket resource. If you want to use it,
 * you need to fill in all the required fields of the structure and
 * call sk_open() to do the actual opening of the socket.
 *
 * The real function name is sock_new(), sk_new() is a macro wrapper
 * to avoid collision with OpenSSL.
 */
sock *
sock_new(pool *p)
{
  sock *s = ralloc(p, &sk_class);
  s->pool = p;
  // s->saddr = s->daddr = IPA_NONE;
  s->tos = s->priority = s->ttl = -1;
  s->fd = -1;
  return s;
}

static int
sk_setup(sock *s)
{
  int y = 1;
  int fd = s->fd;

  if (s->type == SK_SSH_ACTIVE)
    return 0;

  if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
    ERR("O_NONBLOCK");

  if (!s->af)
    return 0;

  if (ipa_nonzero(s->saddr) && !(s->flags & SKF_BIND))
    s->flags |= SKF_PKTINFO;

#ifdef CONFIG_USE_HDRINCL
  if (sk_is_ipv4(s) && (s->type == SK_IP) && (s->flags & SKF_PKTINFO))
  {
    s->flags &= ~SKF_PKTINFO;
    s->flags |= SKF_HDRINCL;
    if (setsockopt(fd, SOL_IP, IP_HDRINCL, &y, sizeof(y)) < 0)
      ERR("IP_HDRINCL");
  }
#endif

  if (s->vrf && !s->iface && (s->type != SK_TCP))
  {
    /* Bind socket to associated VRF interface.
       This is Linux-specific, but so is SO_BINDTODEVICE.
       For accepted TCP sockets it is inherited from the listening one. */
#ifdef SO_BINDTODEVICE
    struct ifreq ifr = {};
    strcpy(ifr.ifr_name, s->vrf->name);
    if (setsockopt(s->fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0)
      ERR("SO_BINDTODEVICE");
#endif
  }

  if (s->iface)
  {
#ifdef SO_BINDTODEVICE
    struct ifreq ifr = {};
    strcpy(ifr.ifr_name, s->iface->name);
    if (setsockopt(s->fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0)
      ERR("SO_BINDTODEVICE");
#endif

#ifdef CONFIG_UNIX_DONTROUTE
    if (setsockopt(s->fd, SOL_SOCKET, SO_DONTROUTE, &y, sizeof(y)) < 0)
      ERR("SO_DONTROUTE");
#endif
  }

  if (sk_is_ipv4(s))
  {
    if (s->flags & SKF_LADDR_RX)
      if (sk_request_cmsg4_pktinfo(s) < 0)
	return -1;

    if (s->flags & SKF_TTL_RX)
      if (sk_request_cmsg4_ttl(s) < 0)
	return -1;

    if ((s->type == SK_UDP) || (s->type == SK_IP))
      if (sk_disable_mtu_disc4(s) < 0)
	return -1;

    if (s->ttl >= 0)
      if (sk_set_ttl4(s, s->ttl) < 0)
	return -1;

    if (s->tos >= 0)
      if (sk_set_tos4(s, s->tos) < 0)
	return -1;
  }

  if (sk_is_ipv6(s))
  {
    if ((s->type == SK_TCP_PASSIVE) || (s->type == SK_TCP_ACTIVE) || (s->type == SK_UDP))
      if (setsockopt(fd, SOL_IPV6, IPV6_V6ONLY, &y, sizeof(y)) < 0)
	ERR("IPV6_V6ONLY");

    if (s->flags & SKF_LADDR_RX)
      if (sk_request_cmsg6_pktinfo(s) < 0)
	return -1;

    if (s->flags & SKF_TTL_RX)
      if (sk_request_cmsg6_ttl(s) < 0)
	return -1;

    if ((s->type == SK_UDP) || (s->type == SK_IP))
      if (sk_disable_mtu_disc6(s) < 0)
	return -1;

    if (s->ttl >= 0)
      if (sk_set_ttl6(s, s->ttl) < 0)
	return -1;

    if (s->tos >= 0)
      if (sk_set_tos6(s, s->tos) < 0)
	return -1;

    if ((s->flags & SKF_UDP6_NO_CSUM_RX) && (s->type == SK_UDP))
      if (sk_set_udp6_no_csum_rx(s) < 0)
	return -1;
  }

  /* Must be after sk_set_tos4() as setting ToS on Linux also mangles priority */
  if (s->priority >= 0)
    if (sk_set_priority(s, s->priority) < 0)
      return -1;

  if ((s->type == SK_UDP) || (s->type == SK_IP))
    sk_set_min_rcvbuf(s, s->rbsize);

  return 0;
}

static void
sk_insert(sock *s)
{
  add_tail(&sock_list, &s->n);
}

static int
sk_connect(sock *s)
{
  sockaddr sa;
  sockaddr_fill(&sa, s->af, s->daddr, s->iface, s->dport);
  return connect(s->fd, &sa.sa, SA_LEN(sa));
}

static void
sk_tcp_connected(sock *s)
{
  sockaddr sa;
  int sa_len = sizeof(sa);

  if ((getsockname(s->fd, &sa.sa, &sa_len) < 0) ||
      (sockaddr_read(&sa, s->af, &s->saddr, &s->iface, &s->sport) < 0))
    log(L_WARN "SOCK: Cannot get local IP address for TCP>");

  s->type = SK_TCP;
  sk_alloc_bufs(s);
  s->tx_hook(s);
}

#ifdef HAVE_LIBSSH
static void
sk_ssh_connected(sock *s)
{
  sk_alloc_bufs(s);
  s->type = SK_SSH;
  s->tx_hook(s);
}
#endif

static int
sk_passive_connected(sock *s, int type)
{
  sockaddr loc_sa, rem_sa;
  int loc_sa_len = sizeof(loc_sa);
  int rem_sa_len = sizeof(rem_sa);

  int fd = accept(s->fd, ((type == SK_TCP) ? &rem_sa.sa : NULL), &rem_sa_len);
  if (fd < 0)
  {
    if ((errno != EINTR) && (errno != EAGAIN))
      s->err_hook(s, errno);
    return 0;
  }

  sock *t = sk_new(s->pool);
  t->type = type;
  t->data = s->data;
  t->af = s->af;
  t->fd = fd;
  t->ttl = s->ttl;
  t->tos = s->tos;
  t->vrf = s->vrf;
  t->rbsize = s->rbsize;
  t->tbsize = s->tbsize;

  if (type == SK_TCP)
  {
    if ((getsockname(fd, &loc_sa.sa, &loc_sa_len) < 0) ||
	(sockaddr_read(&loc_sa, s->af, &t->saddr, &t->iface, &t->sport) < 0))
      log(L_WARN "SOCK: Cannot get local IP address for TCP<");

    if (sockaddr_read(&rem_sa, s->af, &t->daddr, &t->iface, &t->dport) < 0)
      log(L_WARN "SOCK: Cannot get remote IP address for TCP<");
  }

  if (sk_setup(t) < 0)
  {
    /* FIXME: Call err_hook instead ? */
    log(L_ERR "SOCK: Incoming connection: %s%#m", t->err);

    /* FIXME: handle it better in rfree() */
    close(t->fd);
    t->fd = -1;
    rfree(t);
    return 1;
  }

  sk_insert(t);
  sk_alloc_bufs(t);
  s->rx_hook(t, 0);
  return 1;
}

#ifdef HAVE_LIBSSH
/*
 * Return SSH_OK or SSH_AGAIN or SSH_ERROR
 */
static int
sk_ssh_connect(sock *s)
{
  s->fd = ssh_get_fd(s->ssh->session);

  /* Big fall thru automata */
  switch (s->ssh->state)
  {
  case SK_SSH_CONNECT:
  {
    switch (ssh_connect(s->ssh->session))
    {
    case SSH_AGAIN:
      /* A quick look into libSSH shows that ssh_get_fd() should return non-(-1)
       * after SSH_AGAIN is returned by ssh_connect(). This is however nowhere
       * documented but our code relies on that.
       */
      return SSH_AGAIN;

    case SSH_OK:
      break;

    default:
      return SSH_ERROR;
    }
  } /* fallthrough */

  case SK_SSH_SERVER_KNOWN:
  {
    s->ssh->state = SK_SSH_SERVER_KNOWN;

    if (s->ssh->server_hostkey_path)
    {
      int server_identity_is_ok = 1;

      /* Check server identity */
      switch (ssh_is_server_known(s->ssh->session))
      {
#define LOG_WARN_ABOUT_SSH_SERVER_VALIDATION(s,msg,args...) log(L_WARN "SSH Identity %s@%s:%u: " msg, (s)->ssh->username, (s)->host, (s)->dport, ## args);
      case SSH_SERVER_KNOWN_OK:
	/* The server is known and has not changed. */
	break;

      case SSH_SERVER_NOT_KNOWN:
	LOG_WARN_ABOUT_SSH_SERVER_VALIDATION(s, "The server is unknown, its public key was not found in the known host file %s", s->ssh->server_hostkey_path);
	break;

      case SSH_SERVER_KNOWN_CHANGED:
	LOG_WARN_ABOUT_SSH_SERVER_VALIDATION(s, "The server key has changed. Either you are under attack or the administrator changed the key.");
	server_identity_is_ok = 0;
	break;

      case SSH_SERVER_FILE_NOT_FOUND:
	LOG_WARN_ABOUT_SSH_SERVER_VALIDATION(s, "The known host file %s does not exist", s->ssh->server_hostkey_path);
	server_identity_is_ok = 0;
	break;

      case SSH_SERVER_ERROR:
	LOG_WARN_ABOUT_SSH_SERVER_VALIDATION(s, "Some error happened");
	server_identity_is_ok = 0;
	break;

      case SSH_SERVER_FOUND_OTHER:
	LOG_WARN_ABOUT_SSH_SERVER_VALIDATION(s, "The server gave use a key of a type while we had an other type recorded. " \
					     "It is a possible attack.");
	server_identity_is_ok = 0;
	break;
      }

      if (!server_identity_is_ok)
	return SSH_ERROR;
    }
  } /* fallthrough */

  case SK_SSH_USERAUTH:
  {
    s->ssh->state = SK_SSH_USERAUTH;
    switch (ssh_userauth_publickey_auto(s->ssh->session, NULL, NULL))
    {
    case SSH_AUTH_AGAIN:
      return SSH_AGAIN;

    case SSH_AUTH_SUCCESS:
      break;

    default:
      return SSH_ERROR;
    }
  } /* fallthrough */

  case SK_SSH_CHANNEL:
  {
    s->ssh->state = SK_SSH_CHANNEL;
    s->ssh->channel = ssh_channel_new(s->ssh->session);
    if (s->ssh->channel == NULL)
      return SSH_ERROR;
  } /* fallthrough */

  case SK_SSH_SESSION:
  {
    s->ssh->state = SK_SSH_SESSION;
    switch (ssh_channel_open_session(s->ssh->channel))
    {
    case SSH_AGAIN:
      return SSH_AGAIN;

    case SSH_OK:
      break;

    default:
      return SSH_ERROR;
    }
  } /* fallthrough */

  case SK_SSH_SUBSYSTEM:
  {
    s->ssh->state = SK_SSH_SUBSYSTEM;
    if (s->ssh->subsystem)
    {
      switch (ssh_channel_request_subsystem(s->ssh->channel, s->ssh->subsystem))
      {
      case SSH_AGAIN:
	return SSH_AGAIN;

      case SSH_OK:
	break;

      default:
	return SSH_ERROR;
      }
    }
  } /* fallthrough */

  case SK_SSH_ESTABLISHED:
    s->ssh->state = SK_SSH_ESTABLISHED;
  }

  return SSH_OK;
}

/*
 * Return file descriptor number if success
 * Return -1 if failed
 */
static int
sk_open_ssh(sock *s)
{
  if (!s->ssh)
    bug("sk_open() sock->ssh is not allocated");

  ssh_session sess = ssh_new();
  if (sess == NULL)
    ERR2("Cannot create a ssh session");
  s->ssh->session = sess;

  const int verbosity = SSH_LOG_NOLOG;
  ssh_options_set(sess, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
  ssh_options_set(sess, SSH_OPTIONS_HOST, s->host);
  ssh_options_set(sess, SSH_OPTIONS_PORT, &(s->dport));
  /* TODO: Add SSH_OPTIONS_BINDADDR */
  ssh_options_set(sess, SSH_OPTIONS_USER, s->ssh->username);

  if (s->ssh->server_hostkey_path)
    ssh_options_set(sess, SSH_OPTIONS_KNOWNHOSTS, s->ssh->server_hostkey_path);

  if (s->ssh->client_privkey_path)
    ssh_options_set(sess, SSH_OPTIONS_IDENTITY, s->ssh->client_privkey_path);

  ssh_set_blocking(sess, 0);

  switch (sk_ssh_connect(s))
  {
  case SSH_AGAIN:
    break;

  case SSH_OK:
    sk_ssh_connected(s);
    break;

  case SSH_ERROR:
    ERR2(ssh_get_error(sess));
    break;
  }

  return ssh_get_fd(sess);

 err:
  return -1;
}
#endif

/**
 * sk_open - open a socket
 * @s: socket
 *
 * This function takes a socket resource created by sk_new() and
 * initialized by the user and binds a corresponding network connection
 * to it.
 *
 * Result: 0 for success, -1 for an error.
 */
int
sk_open(sock *s)
{
  int af = AF_UNSPEC;
  int fd = -1;
  int do_bind = 0;
  int bind_port = 0;
  ip_addr bind_addr = IPA_NONE;
  sockaddr sa;

  if (s->type <= SK_IP)
  {
    /*
     * For TCP/IP sockets, Address family (IPv4 or IPv6) can be specified either
     * explicitly (SK_IPV4 or SK_IPV6) or implicitly (based on saddr, daddr).
     * But the specifications have to be consistent.
     */

    switch (s->subtype)
    {
    case 0:
      ASSERT(ipa_zero(s->saddr) || ipa_zero(s->daddr) ||
	     (ipa_is_ip4(s->saddr) == ipa_is_ip4(s->daddr)));
      af = (ipa_is_ip4(s->saddr) || ipa_is_ip4(s->daddr)) ? AF_INET : AF_INET6;
      break;

    case SK_IPV4:
      ASSERT(ipa_zero(s->saddr) || ipa_is_ip4(s->saddr));
      ASSERT(ipa_zero(s->daddr) || ipa_is_ip4(s->daddr));
      af = AF_INET;
      break;

    case SK_IPV6:
      ASSERT(ipa_zero(s->saddr) || !ipa_is_ip4(s->saddr));
      ASSERT(ipa_zero(s->daddr) || !ipa_is_ip4(s->daddr));
      af = AF_INET6;
      break;

    default:
      bug("Invalid subtype %d", s->subtype);
    }
  }

  switch (s->type)
  {
  case SK_TCP_ACTIVE:
    s->ttx = "";			/* Force s->ttx != s->tpos */
    /* Fall thru */
  case SK_TCP_PASSIVE:
    fd = socket(af, SOCK_STREAM, IPPROTO_TCP);
    bind_port = s->sport;
    bind_addr = s->saddr;
    do_bind = bind_port || ipa_nonzero(bind_addr);
    break;

#ifdef HAVE_LIBSSH
  case SK_SSH_ACTIVE:
    s->ttx = "";			/* Force s->ttx != s->tpos */
    fd = sk_open_ssh(s);
    break;
#endif

  case SK_UDP:
    fd = socket(af, SOCK_DGRAM, IPPROTO_UDP);
    bind_port = s->sport;
    bind_addr = (s->flags & SKF_BIND) ? s->saddr : IPA_NONE;
    do_bind = 1;
    break;

  case SK_IP:
    fd = socket(af, SOCK_RAW, s->dport);
    bind_port = 0;
    bind_addr = (s->flags & SKF_BIND) ? s->saddr : IPA_NONE;
    do_bind = ipa_nonzero(bind_addr);
    break;

  case SK_MAGIC:
    af = 0;
    fd = s->fd;
    break;

  default:
    bug("sk_open() called for invalid sock type %d", s->type);
  }

  if (fd < 0)
    ERR("socket");

  s->af = af;
  s->fd = fd;

  if (sk_setup(s) < 0)
    goto err;

  if (do_bind)
  {
    if (bind_port)
    {
      int y = 1;

      if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &y, sizeof(y)) < 0)
	ERR2("SO_REUSEADDR");

#ifdef CONFIG_NO_IFACE_BIND
      /* Workaround missing ability to bind to an iface */
      if ((s->type == SK_UDP) && s->iface && ipa_zero(bind_addr))
      {
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &y, sizeof(y)) < 0)
	  ERR2("SO_REUSEPORT");
      }
#endif
    }
    else
      if (s->flags & SKF_HIGH_PORT)
	if (sk_set_high_port(s) < 0)
	  log(L_WARN "Socket error: %s%#m", s->err);

    if (s->flags & SKF_FREEBIND)
      if (sk_set_freebind(s) < 0)
        log(L_WARN "Socket error: %s%#m", s->err);

    sockaddr_fill(&sa, s->af, bind_addr, s->iface, bind_port);
    if (bind(fd, &sa.sa, SA_LEN(sa)) < 0)
      ERR2("bind");
  }

  if (s->ao_keys_init)
  {
    for (int i = 0; i < s->ao_keys_num; i++)
    {
      const struct ao_key *key = s->ao_keys_init[i];
      if (sk_add_ao_key(s, s->daddr, -1, s->iface, key, !i, !i) < 0)
        goto err;
    }
  }
  else if (s->password)
  {
    if (sk_set_md5_auth(s, s->saddr, s->daddr, -1, s->iface, s->password, 0) < 0)
      goto err;
  }

  switch (s->type)
  {
  case SK_TCP_ACTIVE:
    if (sk_connect(s) >= 0)
      sk_tcp_connected(s);
    else if (errno != EINTR && errno != EAGAIN && errno != EINPROGRESS &&
	     errno != ECONNREFUSED && errno != EHOSTUNREACH && errno != ENETUNREACH)
      ERR2("connect");
    break;

  case SK_TCP_PASSIVE:
    if (listen(fd, 8) < 0)
      ERR2("listen");
    break;

  case SK_UDP:
    if (s->flags & SKF_CONNECT)
      if (sk_connect(s) < 0)
	ERR2("connect");

    sk_alloc_bufs(s);
    break;

  case SK_SSH_ACTIVE:
  case SK_MAGIC:
    break;

  default:
    sk_alloc_bufs(s);
  }

  if (!(s->flags & SKF_THREAD))
    sk_insert(s);

  return 0;

err:
  close(fd);
  s->fd = -1;
  return -1;
}

int
sk_open_unix(sock *s, const char *name)
{
  struct sockaddr_un sa;
  int fd;

  /* We are sloppy during error (leak fd and not set s->err), but we die anyway */

  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
    return -1;

  /* Path length checked in test_old_bird() but we may need unix sockets for other reasons in future */
  ASSERT_DIE(strlen(name) < sizeof(sa.sun_path));

  sa.sun_family = AF_UNIX;
  strcpy(sa.sun_path, name);

  if (bind(fd, (struct sockaddr *) &sa, SUN_LEN(&sa)) < 0)
    return -1;

  if (listen(fd, 8) < 0)
    return -1;

  s->fd = fd;
  sk_insert(s);
  return 0;
}


#define CMSG_RX_SPACE MAX(CMSG4_SPACE_PKTINFO+CMSG4_SPACE_TTL, \
			  CMSG6_SPACE_PKTINFO+CMSG6_SPACE_TTL)
#define CMSG_TX_SPACE MAX(CMSG4_SPACE_PKTINFO,CMSG6_SPACE_PKTINFO)

static void
sk_prepare_cmsgs(sock *s, struct msghdr *msg, void *cbuf, size_t cbuflen)
{
  if (sk_is_ipv4(s))
    sk_prepare_cmsgs4(s, msg, cbuf, cbuflen);
  else
    sk_prepare_cmsgs6(s, msg, cbuf, cbuflen);
}

static void
sk_process_cmsgs(sock *s, struct msghdr *msg)
{
  struct cmsghdr *cm;

  s->laddr = IPA_NONE;
  s->lifindex = 0;
  s->rcv_ttl = -1;

  for (cm = CMSG_FIRSTHDR(msg); cm != NULL; cm = CMSG_NXTHDR(msg, cm))
  {
    if ((cm->cmsg_level == SOL_IP) && sk_is_ipv4(s))
    {
      sk_process_cmsg4_pktinfo(s, cm);
      sk_process_cmsg4_ttl(s, cm);
    }

    if ((cm->cmsg_level == SOL_IPV6) && sk_is_ipv6(s))
    {
      sk_process_cmsg6_pktinfo(s, cm);
      sk_process_cmsg6_ttl(s, cm);
    }
  }
}


static inline int
sk_sendmsg(sock *s)
{
  struct iovec iov = {s->tbuf, s->tpos - s->tbuf};
  byte cmsg_buf[CMSG_TX_SPACE];
  sockaddr dst;
  int flags = 0;

  sockaddr_fill(&dst, s->af, s->daddr, s->iface, s->dport);

  struct msghdr msg = {
    .msg_name = &dst.sa,
    .msg_namelen = SA_LEN(dst),
    .msg_iov = &iov,
    .msg_iovlen = 1
  };

#ifdef CONFIG_DONTROUTE_UNICAST
  /* FreeBSD silently changes TTL to 1 when MSG_DONTROUTE is used, therefore we
     cannot use it for other cases (e.g. when TTL security is used). */
  if (ipa_is_ip4(s->daddr) && ip4_is_unicast(ipa_to_ip4(s->daddr)) && (s->ttl == 1))
    flags = MSG_DONTROUTE;
#endif

#ifdef CONFIG_USE_HDRINCL
  byte hdr[20];
  struct iovec iov2[2] = { {hdr, 20}, iov };

  if (s->flags & SKF_HDRINCL)
  {
    sk_prepare_ip_header(s, hdr, iov.iov_len);
    msg.msg_iov = iov2;
    msg.msg_iovlen = 2;
  }
#endif

  if (s->flags & SKF_PKTINFO)
    sk_prepare_cmsgs(s, &msg, cmsg_buf, sizeof(cmsg_buf));

  return sendmsg(s->fd, &msg, flags);
}

static inline int
sk_recvmsg(sock *s)
{
  struct iovec iov = {s->rbuf, s->rbsize};
  byte cmsg_buf[CMSG_RX_SPACE];
  sockaddr src;

  struct msghdr msg = {
    .msg_name = &src.sa,
    .msg_namelen = sizeof(src), // XXXX ??
    .msg_iov = &iov,
    .msg_iovlen = 1,
    .msg_control = cmsg_buf,
    .msg_controllen = sizeof(cmsg_buf),
    .msg_flags = 0
  };

  int rv = recvmsg(s->fd, &msg, 0);
  if (rv < 0)
    return rv;

  //ifdef IPV4
  //  if (cf_type == SK_IP)
  //    rv = ipv4_skip_header(pbuf, rv);
  //endif

  sockaddr_read(&src, s->af, &s->faddr, NULL, &s->fport);
  sk_process_cmsgs(s, &msg);

  if (msg.msg_flags & MSG_TRUNC)
    s->flags |= SKF_TRUNCATED;
  else
    s->flags &= ~SKF_TRUNCATED;

  return rv;
}


static inline void reset_tx_buffer(sock *s) { s->ttx = s->tpos = s->tbuf; }

static int
sk_maybe_write(sock *s)
{
  int e;

  switch (s->type)
  {
  case SK_TCP:
  case SK_MAGIC:
  case SK_UNIX:
    while (s->ttx != s->tpos)
    {
      e = write(s->fd, s->ttx, s->tpos - s->ttx);

      if (e < 0)
      {
	if (errno != EINTR && errno != EAGAIN)
	{
	  reset_tx_buffer(s);
	  /* EPIPE is just a connection close notification during TX */
	  s->err_hook(s, (errno != EPIPE) ? errno : 0);
	  return -1;
	}
	return 0;
      }
      s->ttx += e;
    }
    reset_tx_buffer(s);
    return 1;

#ifdef HAVE_LIBSSH
  case SK_SSH:
    while (s->ttx != s->tpos)
    {
      e = ssh_channel_write(s->ssh->channel, s->ttx, s->tpos - s->ttx);

      if (e < 0)
      {
	s->err = ssh_get_error(s->ssh->session);
	s->err_hook(s, ssh_get_error_code(s->ssh->session));

	reset_tx_buffer(s);
	/* EPIPE is just a connection close notification during TX */
	s->err_hook(s, (errno != EPIPE) ? errno : 0);
	return -1;
      }
      s->ttx += e;
    }
    reset_tx_buffer(s);
    return 1;
#endif

  case SK_UDP:
  case SK_IP:
    {
      if (s->tbuf == s->tpos)
	return 1;

      e = sk_sendmsg(s);

      if (e < 0)
      {
	if (errno != EINTR && errno != EAGAIN)
	{
	  reset_tx_buffer(s);
	  s->err_hook(s, errno);
	  return -1;
	}

	if (!s->tx_hook)
	  reset_tx_buffer(s);
	return 0;
      }
      reset_tx_buffer(s);
      return 1;
    }

  default:
    bug("sk_maybe_write: unknown socket type %d", s->type);
  }
}

int
sk_rx_ready(sock *s)
{
  int rv;
  struct pollfd pfd = { .fd = s->fd };
  pfd.events |= POLLIN;

 redo:
  rv = poll(&pfd, 1, 0);

  if ((rv < 0) && (errno == EINTR || errno == EAGAIN))
    goto redo;

  return rv;
}

/**
 * sk_send - send data to a socket
 * @s: socket
 * @len: number of bytes to send
 *
 * This function sends @len bytes of data prepared in the
 * transmit buffer of the socket @s to the network connection.
 * If the packet can be sent immediately, it does so and returns
 * 1, else it queues the packet for later processing, returns 0
 * and calls the @tx_hook of the socket when the tranmission
 * takes place.
 */
int
sk_send(sock *s, unsigned len)
{
  s->ttx = s->tbuf;
  s->tpos = s->tbuf + len;
  return sk_maybe_write(s);
}

/**
 * sk_send_to - send data to a specific destination
 * @s: socket
 * @len: number of bytes to send
 * @addr: IP address to send the packet to
 * @port: port to send the packet to
 *
 * This is a sk_send() replacement for connection-less packet sockets
 * which allows destination of the packet to be chosen dynamically.
 * Raw IP sockets should use 0 for @port.
 */
int
sk_send_to(sock *s, unsigned len, ip_addr addr, unsigned port)
{
  s->daddr = addr;
  if (port)
    s->dport = port;

  s->ttx = s->tbuf;
  s->tpos = s->tbuf + len;
  return sk_maybe_write(s);
}

/*
int
sk_send_full(sock *s, unsigned len, struct iface *ifa,
	     ip_addr saddr, ip_addr daddr, unsigned dport)
{
  s->iface = ifa;
  s->saddr = saddr;
  s->daddr = daddr;
  s->dport = dport;
  s->ttx = s->tbuf;
  s->tpos = s->tbuf + len;
  return sk_maybe_write(s);
}
*/

static void
call_rx_hook(sock *s, int size)
{
  if (s->rx_hook(s, size))
  {
    /* We need to be careful since the socket could have been deleted by the hook */
    if (current_sock == s)
      s->rpos = s->rbuf;
  }
}

#ifdef HAVE_LIBSSH
static int
sk_read_ssh(sock *s)
{
  ssh_channel rchans[2] = { s->ssh->channel, NULL };
  struct timeval timev = { 1, 0 };

  if (ssh_channel_select(rchans, NULL, NULL, &timev) == SSH_EINTR)
    return 1; /* Try again */

  if (ssh_channel_is_eof(s->ssh->channel) != 0)
  {
    /* The remote side is closing the connection */
    s->err_hook(s, 0);
    return 0;
  }

  if (rchans[0] == NULL)
    return 0; /* No data is available on the socket */

  const uint used_bytes = s->rpos - s->rbuf;
  const int read_bytes = ssh_channel_read_nonblocking(s->ssh->channel, s->rpos, s->rbsize - used_bytes, 0);
  if (read_bytes > 0)
  {
    /* Received data */
    s->rpos += read_bytes;
    call_rx_hook(s, used_bytes + read_bytes);
    return 1;
  }
  else if (read_bytes == 0)
  {
    if (ssh_channel_is_eof(s->ssh->channel) != 0)
    {
	/* The remote side is closing the connection */
	s->err_hook(s, 0);
    }
  }
  else
  {
    s->err = ssh_get_error(s->ssh->session);
    s->err_hook(s, ssh_get_error_code(s->ssh->session));
  }

  return 0; /* No data is available on the socket */
}
#endif

 /* sk_read() and sk_write() are called from BFD's event loop */

static inline int
sk_read_noflush(sock *s, int revents)
{
  switch (s->type)
  {
  case SK_TCP_PASSIVE:
    return sk_passive_connected(s, SK_TCP);

  case SK_UNIX_PASSIVE:
    return sk_passive_connected(s, SK_UNIX);

  case SK_TCP:
  case SK_UNIX:
    {
      int c = read(s->fd, s->rpos, s->rbuf + s->rbsize - s->rpos);

      if (c < 0)
      {
	if (errno != EINTR && errno != EAGAIN)
	  s->err_hook(s, errno);
	else if (errno == EAGAIN && !(revents & POLLIN))
	{
	  log(L_ERR "Got EAGAIN from read when revents=%x (without POLLIN)", revents);
	  s->err_hook(s, 0);
	}
      }
      else if (!c)
	s->err_hook(s, 0);
      else
      {
	s->rpos += c;
	call_rx_hook(s, s->rpos - s->rbuf);
	return 1;
      }
      return 0;
    }

#ifdef HAVE_LIBSSH
  case SK_SSH:
    return sk_read_ssh(s);
#endif

  case SK_MAGIC:
    return s->rx_hook(s, 0);

  default:
    {
      int e = sk_recvmsg(s);

      if (e < 0)
      {
	if (errno != EINTR && errno != EAGAIN)
	  s->err_hook(s, errno);
	return 0;
      }

      s->rpos = s->rbuf + e;
      s->rx_hook(s, e);
      return 1;
    }
  }
}

int
sk_read(sock *s, int revents)
{
  int e = sk_read_noflush(s, revents);
  tmp_flush();
  return e;
}

static inline int
sk_write_noflush(sock *s)
{
  switch (s->type)
  {
  case SK_TCP_ACTIVE:
    {
      if (sk_connect(s) >= 0 || errno == EISCONN)
	sk_tcp_connected(s);
      else if (errno != EINTR && errno != EAGAIN && errno != EINPROGRESS)
	s->err_hook(s, errno);
      return 0;
    }

#ifdef HAVE_LIBSSH
  case SK_SSH_ACTIVE:
    {
      switch (sk_ssh_connect(s))
      {
	case SSH_OK:
	  sk_ssh_connected(s);
	  break;

	case SSH_AGAIN:
	  return 1;

	case SSH_ERROR:
	  s->err = ssh_get_error(s->ssh->session);
	  s->err_hook(s, ssh_get_error_code(s->ssh->session));
	  break;
      }
      return 0;
    }
#endif

  default:
    if (s->ttx != s->tpos && sk_maybe_write(s) > 0)
    {
      if (s->tx_hook)
	s->tx_hook(s);
      return 1;
    }
    return 0;
  }
}

int
sk_write(sock *s)
{
  int e = sk_write_noflush(s);
  tmp_flush();
  return e;
}

int sk_is_ipv4(sock *s)
{ return s->af == AF_INET; }

int sk_is_ipv6(sock *s)
{ return s->af == AF_INET6; }

void
sk_err(sock *s, int revents)
{
  int se = 0, sse = sizeof(se);
  if ((s->type != SK_MAGIC) && (revents & POLLERR))
    if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &se, &sse) < 0)
    {
      log(L_ERR "IO: Socket error: SO_ERROR: %m");
      se = 0;
    }

  s->err_hook(s, se);
  tmp_flush();
}

void
sk_dump_all(struct dump_request *dreq)
{
  node *n;
  sock *s;

  RDUMP("Open sockets:\n");
  WALK_LIST(n, sock_list)
  {
    s = SKIP_BACK(sock, n, n);
    RDUMP("%p ", s);
    sk_dump(dreq, &s->r);
  }
  RDUMP("\n");
}

void
sk_dump_ao_all(struct dump_request *dreq)
{
  RDUMP("TCP-AO sockets:\n");
  WALK_LIST_(node, n, sock_list)
  {
    sock *s = SKIP_BACK(sock, n, n);

    /* Skip non TCP-AO sockets / not supported */
    if (sk_get_ao_info(s, &(struct ao_info){}) < 0)
      continue;

    RDUMP("\n%p", s);
    sk_dump(dreq, &s->r);
    sk_dump_ao_info(s, dreq);
    sk_dump_ao_keys(s, dreq);
  }
}


/*
 *	Internal event log and watchdog
 */

#define EVENT_LOG_LENGTH 32

struct event_log_entry
{
  void *hook;
  void *data;
  btime timestamp;
  btime duration;
};

static struct event_log_entry event_log[EVENT_LOG_LENGTH];
static struct event_log_entry *event_open;
static int event_log_pos, event_log_num, watchdog_active;
static btime last_time;
static btime loop_time;

static void
io_update_time(void)
{
  struct timespec ts;
  int rv;

  /*
   * This is third time-tracking procedure (after update_times() above and
   * times_update() in BFD), dedicated to internal event log and latency
   * tracking. Hopefully, we consolidate these sometimes.
   */

  rv = clock_gettime(CLOCK_MONOTONIC, &ts);
  if (rv < 0)
    die("clock_gettime: %m");

  last_time = ts.tv_sec S + ts.tv_nsec NS;

  if (event_open)
  {
    event_open->duration = last_time - event_open->timestamp;

    if (event_open->duration > config->latency_limit)
      log(L_WARN "Event 0x%p 0x%p took %u.%03u ms",
	  event_open->hook, event_open->data, (uint) (event_open->duration TO_MS), (uint) (event_open->duration % 1000));

    event_open = NULL;
  }
}

/**
 * io_log_event - mark approaching event into event log
 * @hook: event hook address
 * @data: event data address
 *
 * Store info (hook, data, timestamp) about the following internal event into
 * a circular event log (@event_log). When latency tracking is enabled, the log
 * entry is kept open (in @event_open) so the duration can be filled later.
 */
void
io_log_event(void *hook, void *data)
{
  if (config->latency_debug)
    io_update_time();

  struct event_log_entry *en = event_log + event_log_pos;

  en->hook = hook;
  en->data = data;
  en->timestamp = last_time;
  en->duration = 0;

  event_log_num++;
  event_log_pos++;
  event_log_pos %= EVENT_LOG_LENGTH;

  event_open = config->latency_debug ? en : NULL;
}

static inline void
io_close_event(void)
{
  if (event_open)
    io_update_time();
}

void
io_log_dump(struct dump_request *dreq)
{
  int i;

  RDUMP("Event log:");
  for (i = 0; i < EVENT_LOG_LENGTH; i++)
  {
    struct event_log_entry *en = event_log + (event_log_pos + i) % EVENT_LOG_LENGTH;
    if (en->hook)
      RDUMP("  Event 0x%p 0x%p at %8d for %d ms", en->hook, en->data,
	  (int) ((last_time - en->timestamp) TO_MS), (int) (en->duration TO_MS));
  }
}

void
watchdog_sigalrm(int sig UNUSED)
{
  /* Update last_time and duration, but skip latency check */
  config->latency_limit = 0xffffffff;
  io_update_time();

  debug_safe("Watchdog timer timed out\n");

  /* We want core dump */
  abort();
}

static inline void
watchdog_start1(void)
{
  io_update_time();

  loop_time = last_time;
}

static inline void
watchdog_start(void)
{
  io_update_time();

  loop_time = last_time;
  event_log_num = 0;

  if (config->watchdog_timeout)
  {
    alarm(config->watchdog_timeout);
    watchdog_active = 1;
  }
}

static inline void
watchdog_stop(void)
{
  io_update_time();

  if (watchdog_active)
  {
    alarm(0);
    watchdog_active = 0;
  }

  btime duration = last_time - loop_time;
  if (duration > config->watchdog_warning)
    log(L_WARN "I/O loop cycle took %u.%03u ms for %d events",
	(uint) (duration TO_MS), (uint) (duration % 1000), event_log_num);
}


/*
 *	Main I/O Loop
 */

void
io_init(void)
{
  init_list(&sock_list);
  init_list(&global_event_list);
  init_list(&global_work_list);
  krt_io_init();
  // XXX init_times();
  // XXX update_times();
  boot_time = current_time();

  u64 now = (u64) current_real_time();
  srandom((uint) (now ^ (now >> 32)));
}

static int short_loops = 0;
#define SHORT_LOOP_MAX 10
#define WORK_EVENTS_MAX 10

void
io_loop(void)
{
  int poll_tout, timeout;
  int nfds, events, pout;
  timer *t;
  sock *s;
  node *n;
  int fdmax = 256;
  struct pollfd *pfd = xmalloc(fdmax * sizeof(struct pollfd));

  watchdog_start1();
  for(;;)
    {
      times_update(&main_timeloop);
      ev_run_list(&global_event_list);
      ev_run_list_limited(&global_work_list, WORK_EVENTS_MAX);
      timers_fire(&main_timeloop);
      io_close_event();

      events = !EMPTY_LIST(global_event_list) || !EMPTY_LIST(global_work_list);
      poll_tout = (events ? 0 : 3000); /* Time in milliseconds */
      if (t = timers_first(&main_timeloop))
      {
	times_update(&main_timeloop);
	timeout = (tm_remains(t) TO_MS) + 1;
	poll_tout = MIN(poll_tout, timeout);
      }

      nfds = 0;
      WALK_LIST(n, sock_list)
	{
	  pfd[nfds] = (struct pollfd) { .fd = -1 }; /* everything other set to 0 by this */
	  s = SKIP_BACK(sock, n, n);
	  if (s->rx_hook)
	    {
	      pfd[nfds].fd = s->fd;
	      pfd[nfds].events |= POLLIN;
	    }
	  if (s->tx_hook && s->ttx != s->tpos)
	    {
	      pfd[nfds].fd = s->fd;
	      pfd[nfds].events |= POLLOUT;
	    }
	  if (pfd[nfds].fd != -1)
	    {
	      s->index = nfds;
	      nfds++;
	    }
	  else
	    s->index = -1;

	  if (nfds >= fdmax)
	    {
	      fdmax *= 2;
	      pfd = xrealloc(pfd, fdmax * sizeof(struct pollfd));
	    }
	}

      /*
       * Yes, this is racy. But even if the signal comes before this test
       * and entering poll(), it gets caught on the next timer tick.
       */

      if (async_config_flag)
	{
	  io_log_event(async_config, NULL);
	  async_config();
	  async_config_flag = 0;
	  continue;
	}
      if (async_dump_flag)
	{
	  io_log_event(async_dump, NULL);
	  async_dump();
	  async_dump_flag = 0;
	  continue;
	}
      if (async_shutdown_flag)
	{
	  io_log_event(async_shutdown, NULL);
	  async_shutdown();
	  async_shutdown_flag = 0;
	  continue;
	}

      /* And finally enter poll() to find active sockets */
      watchdog_stop();
      pout = poll(pfd, nfds, poll_tout);
      watchdog_start();

      if (pout < 0)
	{
	  if (errno == EINTR || errno == EAGAIN)
	    continue;
	  die("poll: %m");
	}
      if (pout)
	{
	  times_update(&main_timeloop);

	  /* guaranteed to be non-empty */
	  current_sock = SKIP_BACK(sock, n, HEAD(sock_list));

	  while (current_sock)
	    {
	      sock *s = current_sock;
	      if (s->index == -1)
		{
		  current_sock = sk_next(s);
		  goto next;
		}

	      int e;
	      int steps;

	      steps = MAX_STEPS;
	      if (s->fast_rx && (pfd[s->index].revents & POLLIN) && s->rx_hook)
		do
		  {
		    steps--;
		    io_log_event(s->rx_hook, s->data);
		    e = sk_read(s, pfd[s->index].revents);
		    if (s != current_sock)
		      goto next;
		  }
		while (e && s->rx_hook && steps);

	      steps = MAX_STEPS;
	      if (pfd[s->index].revents & POLLOUT)
		do
		  {
		    steps--;
		    io_log_event(s->tx_hook, s->data);
		    e = sk_write(s);
		    if (s != current_sock)
		      goto next;
		  }
		while (e && steps);

	      current_sock = sk_next(s);
	    next: ;
	    }

	  short_loops++;
	  if (events && (short_loops < SHORT_LOOP_MAX))
	    continue;
	  short_loops = 0;

	  int count = 0;
	  current_sock = stored_sock;
	  if (current_sock == NULL)
	    current_sock = SKIP_BACK(sock, n, HEAD(sock_list));

	  while (current_sock && count < MAX_RX_STEPS)
	    {
	      sock *s = current_sock;
	      if (s->index == -1)
		{
		  current_sock = sk_next(s);
		  goto next2;
		}

	      if (!s->fast_rx && (pfd[s->index].revents & POLLIN) && s->rx_hook)
		{
		  count++;
		  io_log_event(s->rx_hook, s->data);
		  sk_read(s, pfd[s->index].revents);
		  if (s != current_sock)
		    goto next2;
		}

	      if (pfd[s->index].revents & (POLLHUP | POLLERR))
		{
		  sk_err(s, pfd[s->index].revents);
		  if (s != current_sock)
		    goto next2;
		}

	      current_sock = sk_next(s);
	    next2: ;
	    }


	  stored_sock = current_sock;
	}
    }
}

void
test_old_bird(const char *path)
{
  int fd;
  struct sockaddr_un sa;

  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    die("Cannot create socket: %m");
  if (strlen(path) >= sizeof(sa.sun_path))
    die("Socket path too long");
  bzero(&sa, sizeof(sa));
  sa.sun_family = AF_UNIX;
  strcpy(sa.sun_path, path);
  if (connect(fd, (struct sockaddr *) &sa, SUN_LEN(&sa)) == 0)
    die("I found another BIRD running.");
  close(fd);
}


/*
 *	DNS resolver
 */

ip_addr
resolve_hostname(const char *host, int type, const char **err_msg)
{
  struct addrinfo *res;
  struct addrinfo hints = {
    .ai_family = AF_UNSPEC,
    .ai_socktype = (type == SK_UDP) ? SOCK_DGRAM : SOCK_STREAM,
    .ai_flags = AI_ADDRCONFIG,
  };

  *err_msg = NULL;

  int err_code = getaddrinfo(host, NULL, &hints, &res);
  if (err_code != 0)
  {
    *err_msg = gai_strerror(err_code);
    return IPA_NONE;
  }

  ip_addr addr = IPA_NONE;
  uint unused;

  sockaddr_read((sockaddr *) res->ai_addr, res->ai_family, &addr, NULL, &unused);
  freeaddrinfo(res);

  return addr;
}
