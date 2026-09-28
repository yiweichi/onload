/* TCP send tail-latency tracing: per-segment rdtsc timing + outlier log.
 * Userspace only. In kernel (__KERNEL__) all macros compile to no-ops. */
#ifndef __TCP_SEND_LAT_H__
#define __TCP_SEND_LAT_H__

#ifdef __KERNEL__

#include <asm/timex.h>

static __inline__ ci_uint64 oo_rdtsc(void) { return get_cycles(); }
static __inline__ int oo_lat_enabled(void) { return 0; }
typedef struct {
  ci_uint64 t0;
  ci_uint64 s_prologue;
  ci_uint64 s_trylock;
  ci_uint64 s_fill_tail;
  ci_uint64 s_flags;
  ci_uint64 s_adv_nagle;
  ci_uint64 s_adv;
  ci_uint64 s_send_space;
  ci_uint64 s_alloc;
  ci_uint64 s_fill;
  ci_uint64 s_enqueue;
  ci_uint64 s_done_flags;
  ci_uint64 s_poll;
  ci_uint64 s_prequeue;
  ci_uint64 s_unlock;
  int poll_cnt;
  int alloc_cnt;
  int lock_ok;
  int via_prequeue;
} oo_send_lat_t;
#define OO_LAT_START(_s) \
  int __lat_on = 0; oo_send_lat_t __lat; (void)sizeof(_s); (void)sizeof(__lat)
#define OO_SEG(_f, _stmt) do { _stmt; } while(0)
#define OO_SEG_POLL(_stmt) do { _stmt; } while(0)
#define OO_LAT_OUTLIER(_n,_t,_r,_w) do {} while(0)
typedef struct { ci_uint64 adv_hdr; ci_uint64 adv_push; ci_uint64 adv_poll;
                 ci_uint64 adv_resync; ci_uint64 adv_poll_evq;
                 ci_uint64 adv_poll_tmr; } oo_adv_lat_t;
#define OO_ADV_ACC(_f, _cyc) do {} while(0)

#else /* userspace */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static __inline__ ci_uint64 oo_rdtsc(void)
{
  unsigned lo, hi;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((ci_uint64)hi << 32) | lo;
}

static ci_uint64 oo_lat_thresh_cycles = 0;
static int oo_lat_trace_on = -1;

static __inline__ ci_uint64 oo_tsc_freq(void)
{
  static ci_uint64 freq = 0;
  if( freq ) return freq;
  freq = 3000000000ULL;
  FILE* f = fopen("/proc/cpuinfo", "r");
  if( f ) {
    char line[256];
    while( fgets(line, sizeof(line), f) ) {
      double mhz = 0;
      if( sscanf(line, "cpu MHz : %lf", &mhz) == 1 && mhz > 0 ) {
        freq = (ci_uint64)(mhz * 1e6);
        break;
      }
    }
    fclose(f);
  }
  return freq;
}

static __inline__ int oo_lat_enabled(void)
{
  if( oo_lat_trace_on == -1 ) {
    const char* e = getenv("OO_TCP_SEND_LAT_TRACE");
    oo_lat_trace_on = (e && e[0] != '0') ? 1 : 0;
    if( oo_lat_trace_on ) {
      const char* tns = getenv("OO_TCP_SEND_LAT_THRESH_NS");
      double ns = tns ? atof(tns) : 10000.0;
      if( ns < 0 ) ns = 10000.0;
      oo_lat_thresh_cycles = (ci_uint64)(oo_tsc_freq() * ns / 1e9);
    }
  }
  return oo_lat_trace_on;
}

static __inline__ unsigned long long oo_cyc_to_ns(ci_uint64 cyc)
{
  return (unsigned long long)(cyc * 1e9 / oo_tsc_freq());
}

/* Sub-segments inside adv_nagle path (tcp_tx.c / ip_tx.c write here via
 * thread-local, ci_tcp_sendmsg drains them at OO_LAT_OUTLIER time). */
typedef struct {
  ci_uint64 adv_hdr;    /* header prep loop in advance_to */
  ci_uint64 adv_push;   /* ci_ip_send_tcp_list: NIC push/PIO/doorbell */
  ci_uint64 adv_poll;   /* trailing poll in advance_nagle */
  ci_uint64 adv_resync; /* ci_ip_time_resync */
  ci_uint64 adv_poll_evq; /* evq drain inside trailing poll */
  ci_uint64 adv_poll_tmr; /* timer poll inside trailing poll */
} oo_adv_lat_t;
extern __thread oo_adv_lat_t oo_adv_tls;
#define OO_ADV_ACC(_f, _cyc) do { oo_adv_tls._f += (_cyc); } while(0)
typedef struct {
  ci_uint64 t0;
  ci_uint64 s_prologue;   /* entry -> fast_path decision (iov scan etc) */
  ci_uint64 s_trylock;    /* si_trylock attempts */
  ci_uint64 s_fill_tail;  /* ci_tcp_tx_fill_sendq_tail (fast path) */
  ci_uint64 s_flags;      /* PSH/MORE flag setup */
  ci_uint64 s_adv_nagle;  /* ci_tcp_tx_advance_nagle */
  ci_uint64 s_adv;        /* ci_tcp_tx_advance */
  ci_uint64 s_send_space; /* ci_tcp_tx_send_space */
  ci_uint64 s_alloc;      /* ci_tcp_send_alloc_pkts */
  ci_uint64 s_fill;       /* ci_tcp_send_fill_pkts (incl memcpy) */
  ci_uint64 s_enqueue;    /* ci_tcp_sendmsg_enqueue */
  ci_uint64 s_done_flags; /* final PSH/ACK flag setup */
  ci_uint64 s_poll;       /* ci_netif_poll */
  ci_uint64 s_prequeue;   /* ci_tcp_send_via_prequeue */
  ci_uint64 s_unlock;     /* ci_netif_unlock */
  int poll_cnt;
  int alloc_cnt;
  int lock_ok;
  int via_prequeue;
} oo_send_lat_t;

#define OO_LAT_START(_s) \
  int __lat_on = oo_lat_enabled(); \
  oo_send_lat_t __lat; \
  if( __lat_on ) { \
    memset(&__lat, 0, sizeof(__lat)); \
    memset(&oo_adv_tls, 0, sizeof(oo_adv_tls)); \
    __lat.t0 = oo_rdtsc(); \
  }

/* Time one segment: OO_SEG(s_alloc, m = ci_tcp_send_alloc_pkts(...)); */
#define OO_SEG(_f, _stmt) \
  do { \
    if( __lat_on ) { \
      ci_uint64 __s0 = oo_rdtsc(); \
      _stmt; \
      __lat._f += oo_rdtsc() - __s0; \
    } else { \
      _stmt; \
    } \
  } while(0)

#define OO_SEG_POLL(_stmt) \
  do { \
    if( __lat_on ) { \
      ci_uint64 __s0 = oo_rdtsc(); \
      _stmt; \
      __lat.s_poll += oo_rdtsc() - __s0; \
      __lat.poll_cnt++; \
    } else { \
      _stmt; \
    } \
  } while(0)

#define OO_LAT_OUTLIER(_ni, _ts, _rc, _where) \
  do { \
    if( __lat_on ) { \
      ci_uint64 __end = oo_rdtsc(); \
      ci_uint64 __tot = __end - __lat.t0; \
      if( __tot >= oo_lat_thresh_cycles ) { \
        unsigned long long __tot_ns = oo_cyc_to_ns(__tot); \
        unsigned long long __pro = oo_cyc_to_ns(__lat.s_prologue); \
        unsigned long long __ft = oo_cyc_to_ns(__lat.s_fill_tail); \
        unsigned long long __fl = oo_cyc_to_ns(__lat.s_flags); \
        unsigned long long __an = oo_cyc_to_ns(__lat.s_adv_nagle); \
        unsigned long long __ad = oo_cyc_to_ns(__lat.s_adv); \
        unsigned long long __ss = oo_cyc_to_ns(__lat.s_send_space); \
        unsigned long long __al = oo_cyc_to_ns(__lat.s_alloc); \
        unsigned long long __fi = oo_cyc_to_ns(__lat.s_fill); \
        unsigned long long __en = oo_cyc_to_ns(__lat.s_enqueue); \
        unsigned long long __df = oo_cyc_to_ns(__lat.s_done_flags); \
        unsigned long long __po = oo_cyc_to_ns(__lat.s_poll); \
        unsigned long long __pq = oo_cyc_to_ns(__lat.s_prequeue); \
        unsigned long long __un = oo_cyc_to_ns(__lat.s_unlock); \
        unsigned long long __rs = oo_cyc_to_ns(oo_adv_tls.adv_resync); \
        unsigned long long __hd = oo_cyc_to_ns(oo_adv_tls.adv_hdr); \
        unsigned long long __pu = oo_cyc_to_ns(oo_adv_tls.adv_push); \
        unsigned long long __ap = oo_cyc_to_ns(oo_adv_tls.adv_poll); \
        unsigned long long __ev = oo_cyc_to_ns(oo_adv_tls.adv_poll_evq); \
        unsigned long long __tm = oo_cyc_to_ns(oo_adv_tls.adv_poll_tmr); \
        unsigned long long __sum = __pro+oo_cyc_to_ns(__lat.s_trylock)+__ft+__fl+__an+__ad+__ss+__al+ \
            __fi+__en+__df+__po+__pq+__un; \
        long long __resid = (long long)__tot_ns - (long long)__sum; \
        if( __resid < 0 ) __resid = 0; \
        { int __pb_alloc = (_ni)->packets->n_pkts_allocated; \
          int __pb_free = (_ni)->packets->n_free; \
          int __pb_rx = (_ni)->state->n_rx_pkts; \
          int __pb_used = __pb_alloc - __pb_free - \
                          (_ni)->state->n_async_pkts; \
          int __pb_tx = __pb_used - __pb_rx - (_ni)->state->n_looppkts; \
        fprintf(stderr, \
          "TCP_SEND_LAT [%s] total=%lluns rc=%d sendq=%d retrans=%d " \
          "lock=%d preq_taken=%d\n" \
          "  |-prologue=%lluns trylock=%lluns\n" \
          "  |-send_path: space=%lluns alloc=%lluns(%d) fill=%lluns " \
          "enqueue=%lluns done_flags=%lluns fill_tail=%lluns flags=%lluns\n" \
          "  |-adv_nagle=%lluns\n" \
          "  |  |-resync=%lluns hdr=%lluns push=%lluns trailing_poll=%lluns\n" \
          "  |     |-poll_evq=%lluns poll_tmr=%lluns\n" \
          "  |-adv_direct=%lluns poll_main=%lluns(%d) prequeue=%lluns " \
          "unlock=%lluns\n" \
          "  |-pkt_bufs: alloc=%d free=%d rx=%d tx_inuse=%d\n" \
          "  |-unaccounted(residual)=%lluns\n", \
               (_where), __tot_ns, (int)(_rc), \
               (int)((_ts)->send.num), (int)((_ts)->retrans.num), \
               __lat.lock_ok, __lat.via_prequeue, \
               __pro, oo_cyc_to_ns(__lat.s_trylock), __ss, __al, __lat.alloc_cnt, __fi, __en, __df, \
               __ft, __fl, __an, __rs, __hd, __pu, __ap, __ev, __tm, \
               __ad, __po, __lat.poll_cnt, __pq, __un, \
               __pb_alloc, __pb_free, __pb_rx, __pb_tx, __resid); } \
      } \
    } \
  } while(0)

#endif /* __KERNEL__ */
#endif
