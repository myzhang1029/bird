/*
 *	BIRD -- Linux Kernel Netlink Route Syncer
 *
 *	(c) 1998--2000 Martin Mares <mj@ucw.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#ifndef _BIRD_KRT_SYS_H_
#define _BIRD_KRT_SYS_H_


/* Kernel interfaces */

struct kif_params {
};

struct kif_state {
};


static inline void kif_sys_init(struct kif_proto *p UNUSED) { }
static inline int kif_sys_reconfigure(struct kif_proto *p UNUSED, struct kif_config *n UNUSED, struct kif_config *o UNUSED) { return 1; }

static inline void kif_sys_preconfig(struct config *c UNUSED) { }
static inline void kif_sys_postconfig(struct kif_config *c UNUSED) { }
static inline void kif_sys_init_config(struct kif_config *c UNUSED) { }
static inline void kif_sys_copy_config(struct kif_config *d UNUSED, struct kif_config *s UNUSED) { }

static inline struct ifa * kif_get_primary_ip(struct iface *i UNUSED) { return NULL; }


/* Kernel routes */

#define KRT_ALLOW_MERGE_PATHS	1

#define EA_KRT_PREFSRC		EA_CODE(PROTOCOL_KERNEL, 0x10)
#define EA_KRT_REALM		EA_CODE(PROTOCOL_KERNEL, 0x11)
#define EA_KRT_SCOPE		EA_CODE(PROTOCOL_KERNEL, 0x12)


#define KRT_METRICS_MAX		0x12	/* RTAX_FASTOPEN_NO_COOKIE+1 */
#define KRT_METRICS_OFFSET	0x20	/* Offset of EA_KRT_* vs RTAX_* */

#define KRT_FEATURES_MAX	4

/*
 * Following attributes are parts of RTA_METRICS kernel route attribute, their
 * ids must be consistent with their RTAX_* constants (+ KRT_METRICS_OFFSET)
 */
#define EA_KRT_METRICS		EA_CODE(PROTOCOL_KERNEL, 0x20)	/* Dummy one */
#define EA_KRT_LOCK		EA_CODE(PROTOCOL_KERNEL, 0x21)
#define EA_KRT_MTU		EA_CODE(PROTOCOL_KERNEL, 0x22)
#define EA_KRT_WINDOW		EA_CODE(PROTOCOL_KERNEL, 0x23)
#define EA_KRT_RTT		EA_CODE(PROTOCOL_KERNEL, 0x24)
#define EA_KRT_RTTVAR		EA_CODE(PROTOCOL_KERNEL, 0x25)
#define EA_KRT_SSTHRESH		EA_CODE(PROTOCOL_KERNEL, 0x26)
#define EA_KRT_CWND		EA_CODE(PROTOCOL_KERNEL, 0x27)
#define EA_KRT_ADVMSS		EA_CODE(PROTOCOL_KERNEL, 0x28)
#define EA_KRT_REORDERING 	EA_CODE(PROTOCOL_KERNEL, 0x29)
#define EA_KRT_HOPLIMIT		EA_CODE(PROTOCOL_KERNEL, 0x2a)
#define EA_KRT_INITCWND		EA_CODE(PROTOCOL_KERNEL, 0x2b)
#define EA_KRT_FEATURES		EA_CODE(PROTOCOL_KERNEL, 0x2c)
#define EA_KRT_RTO_MIN		EA_CODE(PROTOCOL_KERNEL, 0x2d)
#define EA_KRT_INITRWND		EA_CODE(PROTOCOL_KERNEL, 0x2e)
#define EA_KRT_QUICKACK		EA_CODE(PROTOCOL_KERNEL, 0x2f)
#define EA_KRT_CONGCTL		EA_CODE(PROTOCOL_KERNEL, 0x30)
#define EA_KRT_FASTOPEN_NO_COOKIE EA_CODE(PROTOCOL_KERNEL, 0x31)


struct krt_params {
  u32 table_id;				/* Kernel table ID we sync with */
  u32 metric;				/* Kernel metric used for all routes */
  uint netlink_rx_buffer;		/* Rx buffer size for the netlink socket */
};

struct krt_state {
  struct krt_proto *hash_next;
};


static inline void krt_sys_init(struct krt_proto *p UNUSED) { }
static inline void krt_sys_preconfig(struct config *c UNUSED) { }
static inline void krt_sys_postconfig(struct krt_config *x UNUSED) { }


#endif
