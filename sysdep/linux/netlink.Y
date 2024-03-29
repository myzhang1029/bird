/*
 *	BIRD -- Linux Netlink Configuration
 *
 *	(c) 1999--2000 Martin Mares <mj@ucw.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

CF_HDR

CF_DECLS

CF_KEYWORDS(KERNEL, TABLE, METRIC, NETLINK, RX, BUFFER,
	    KRT_PREFSRC, KRT_REALM, KRT_SCOPE, KRT_MTU, KRT_WINDOW,
	    KRT_RTT, KRT_RTTVAR, KRT_SSTHRESH, KRT_CWND, KRT_ADVMSS, KRT_REORDERING,
	    KRT_HOPLIMIT, KRT_INITCWND, KRT_RTO_MIN, KRT_INITRWND, KRT_QUICKACK,
	    KRT_CONGCTL, KRT_FASTOPEN_NO_COOKIE)

CF_KEYWORDS(KRT_LOCK_MTU, KRT_LOCK_WINDOW, KRT_LOCK_RTT, KRT_LOCK_RTTVAR,
	    KRT_LOCK_SSTHRESH, KRT_LOCK_CWND, KRT_LOCK_ADVMSS, KRT_LOCK_REORDERING,
	    KRT_LOCK_HOPLIMIT, KRT_LOCK_INITCWND, KRT_LOCK_RTO_MIN, KRT_LOCK_INITRWND,
	    KRT_LOCK_QUICKACK, KRT_LOCK_CONGCTL, KRT_LOCK_FASTOPEN_NO_COOKIE,
	    KRT_FEATURE_ECN, KRT_FEATURE_ALLFRAG)

/* Deprecated names for backward compatiblity */
CF_KEYWORDS(KRT_SSTRESH, KRT_LOCK_SSTRESH)


CF_GRAMMAR

kern_proto: kern_proto kern_sys_item ';' ;

kern_sys_item:
   KERNEL TABLE expr { THIS_KRT->sys.table_id = $3; }
 | METRIC expr { THIS_KRT->sys.metric = $2; }
 | NETLINK RX BUFFER expr { THIS_KRT->sys.netlink_rx_buffer = $4; }
 ;

dynamic_attr: KRT_PREFSRC	{ $$ = f_new_dynamic_attr(EAF_TYPE_IP_ADDRESS, T_IP, EA_KRT_PREFSRC); } ;
dynamic_attr: KRT_REALM	{ $$ = f_new_dynamic_attr(EAF_TYPE_INT, T_INT, EA_KRT_REALM); } ;
dynamic_attr: KRT_SCOPE	{ $$ = f_new_dynamic_attr(EAF_TYPE_INT, T_INT, EA_KRT_SCOPE); } ;

dynamic_attr: KRT_MTU		{ $$ = f_new_dynamic_attr(EAF_TYPE_INT, T_INT, EA_KRT_MTU); } ;
dynamic_attr: KRT_WINDOW	{ $$ = f_new_dynamic_attr(EAF_TYPE_INT, T_INT, EA_KRT_WINDOW); } ;
dynamic_attr: KRT_RTT		{ $$ = f_new_dynamic_attr(EAF_TYPE_INT, T_INT, EA_KRT_RTT); } ;
dynamic_attr: KRT_RTTVAR	{ $$ = f_new_dynamic_attr(EAF_TYPE_INT, T_INT, EA_KRT_RTTVAR); } ;
dynamic_attr: KRT_SSTHRESH	{ $$ = f_new_dynamic_attr(EAF_TYPE_INT, T_INT, EA_KRT_SSTHRESH); } ;
dynamic_attr: KRT_SSTRESH	{ $$ = f_new_dynamic_attr(EAF_TYPE_INT, T_INT, EA_KRT_SSTHRESH);
				  cf_warn("Attribute krt_sstresh is deprecated (typo), use krt_ssthresh"); } ;
dynamic_attr: KRT_CWND		{ $$ = f_new_dynamic_attr(EAF_TYPE_INT, T_INT, EA_KRT_CWND); } ;
dynamic_attr: KRT_ADVMSS	{ $$ = f_new_dynamic_attr(EAF_TYPE_INT, T_INT, EA_KRT_ADVMSS); } ;
dynamic_attr: KRT_REORDERING	{ $$ = f_new_dynamic_attr(EAF_TYPE_INT, T_INT, EA_KRT_REORDERING); } ;
dynamic_attr: KRT_HOPLIMIT	{ $$ = f_new_dynamic_attr(EAF_TYPE_INT, T_INT, EA_KRT_HOPLIMIT); } ;
dynamic_attr: KRT_INITCWND	{ $$ = f_new_dynamic_attr(EAF_TYPE_INT, T_INT, EA_KRT_INITCWND); } ;
dynamic_attr: KRT_RTO_MIN	{ $$ = f_new_dynamic_attr(EAF_TYPE_INT, T_INT, EA_KRT_RTO_MIN); } ;
dynamic_attr: KRT_INITRWND	{ $$ = f_new_dynamic_attr(EAF_TYPE_INT, T_INT, EA_KRT_INITRWND); } ;
dynamic_attr: KRT_QUICKACK	{ $$ = f_new_dynamic_attr(EAF_TYPE_INT, T_INT, EA_KRT_QUICKACK); } ;
dynamic_attr: KRT_CONGCTL	{ $$ = f_new_dynamic_attr(EAF_TYPE_STRING, T_STRING, EA_KRT_CONGCTL); } ;
dynamic_attr: KRT_FASTOPEN_NO_COOKIE { $$ = f_new_dynamic_attr(EAF_TYPE_INT, T_INT, EA_KRT_FASTOPEN_NO_COOKIE); } ;

/* Bits of EA_KRT_LOCK, based on RTAX_* constants */

dynamic_attr: KRT_LOCK_MTU	{ $$ = f_new_dynamic_attr_bit(2, T_BOOL, EA_KRT_LOCK); } ;
dynamic_attr: KRT_LOCK_WINDOW	{ $$ = f_new_dynamic_attr_bit(3, T_BOOL, EA_KRT_LOCK); } ;
dynamic_attr: KRT_LOCK_RTT	{ $$ = f_new_dynamic_attr_bit(4, T_BOOL, EA_KRT_LOCK); } ;
dynamic_attr: KRT_LOCK_RTTVAR	{ $$ = f_new_dynamic_attr_bit(5, T_BOOL, EA_KRT_LOCK); } ;
dynamic_attr: KRT_LOCK_SSTHRESH	{ $$ = f_new_dynamic_attr_bit(6, T_BOOL, EA_KRT_LOCK); } ;
dynamic_attr: KRT_LOCK_SSTRESH	{ $$ = f_new_dynamic_attr_bit(6, T_BOOL, EA_KRT_LOCK);
				  cf_warn("Attribute krt_lock_sstresh is deprecated (typo), use krt_lock_ssthresh"); } ;
dynamic_attr: KRT_LOCK_CWND	{ $$ = f_new_dynamic_attr_bit(7, T_BOOL, EA_KRT_LOCK); } ;
dynamic_attr: KRT_LOCK_ADVMSS	{ $$ = f_new_dynamic_attr_bit(8, T_BOOL, EA_KRT_LOCK); } ;
dynamic_attr: KRT_LOCK_REORDERING { $$ = f_new_dynamic_attr_bit(9, T_BOOL, EA_KRT_LOCK); } ;
dynamic_attr: KRT_LOCK_HOPLIMIT	{ $$ = f_new_dynamic_attr_bit(10, T_BOOL, EA_KRT_LOCK); } ;
dynamic_attr: KRT_LOCK_INITCWND	{ $$ = f_new_dynamic_attr_bit(11, T_BOOL, EA_KRT_LOCK); } ;
/* No lock for FEATURES bitfield */
dynamic_attr: KRT_LOCK_RTO_MIN	{ $$ = f_new_dynamic_attr_bit(13, T_BOOL, EA_KRT_LOCK); } ;
dynamic_attr: KRT_LOCK_INITRWND	{ $$ = f_new_dynamic_attr_bit(14, T_BOOL, EA_KRT_LOCK); } ;
dynamic_attr: KRT_LOCK_QUICKACK	{ $$ = f_new_dynamic_attr_bit(15, T_BOOL, EA_KRT_LOCK); } ;
dynamic_attr: KRT_LOCK_CONGCTL	{ $$ = f_new_dynamic_attr_bit(16, T_BOOL, EA_KRT_LOCK); } ;
dynamic_attr: KRT_LOCK_FASTOPEN_NO_COOKIE { $$ = f_new_dynamic_attr_bit(17, T_BOOL, EA_KRT_LOCK); } ;

dynamic_attr: KRT_FEATURE_ECN	{ $$ = f_new_dynamic_attr_bit(0, T_BOOL, EA_KRT_FEATURES); } ;
dynamic_attr: KRT_FEATURE_ALLFRAG { $$ = f_new_dynamic_attr_bit(3, T_BOOL, EA_KRT_FEATURES); } ;


CF_CODE

CF_END
