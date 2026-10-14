/* TCP send tail-latency tracing: per-segment rdtsc timing + outlier log.
 * Userspace only. In kernel (__KERNEL__) all macros compile to no-ops. */
#ifndef __TCP_SEND_LAT_H__
#define __TCP_SEND_LAT_H__

#ifdef __KERNEL__

#include <asm/timex.h>

static __inline__ ci_uint64 oo_rdtsc(void) { return get_cycles(); }
static __inline__ ci_uint64 oo_rdtsc_begin(void) { return get_cycles(); }
static __inline__ ci_uint64 oo_rdtsc_end(void) { return get_cycles(); }
static __inline__ ci_uint64 oo_rdtsc_serialized(void) { return get_cycles(); }
static __inline__ int oo_lat_enabled(void) { return 0; }
typedef struct {
  ci_uint64 t0;
  ci_uint64 prologue;
  ci_uint64 trylock;
  ci_uint64 fill_tail;
  ci_uint64 flags;
  ci_uint64 adv_nagle;
  ci_uint64 adv_direct;
  ci_uint64 space;
  ci_uint64 alloc;
  ci_uint64 fill;
  ci_uint64 enqueue;
  ci_uint64 done_flags;
  ci_uint64 poll_main;
  ci_uint64 prequeue;
  ci_uint64 unlock;
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
typedef struct { ci_uint64 hdr; ci_uint64 push; ci_uint64 trailing_poll;
                 ci_uint64 resync; ci_uint64 poll_evq;
                 ci_uint64 poll_tmr; ci_uint64 tx_prequeue;
                 ci_uint64 lock_defer; ci_uint64 br1; ci_uint64 br1_work;
                 ci_uint64 br2; ci_uint64 br2_work; ci_uint64 br3;
                 ci_uint64 br3_work; ci_uint64 br3_spins; ci_uint64 br3_lock;
                 ci_uint64 br3_defer; ci_uint64 entry;
                 ci_uint64 enter_gap; ci_uint64 total; } oo_adv_lat_t;
#define OO_ADV_ACC(_f, _cyc) do {} while(0)

#else /* userspace */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

/* rdtscp + lfence backend. rdtscp waits for prior instructions, trailing
 * lfence blocks later loads from leaking into the reading. begin adds a
 * leading lfence too so the interval is fenced on both sides. */
static __inline__ ci_uint64 oo_rdtsc(void)
{
  unsigned lo, hi, aux;
  __asm__ __volatile__("rdtscp\n\tlfence"
                       : "=a"(lo), "=d"(hi), "=c"(aux) :: "memory");
  return ((ci_uint64)hi << 32) | lo;
}
static __inline__ unsigned oo_rdtscp_cpu(void)
{
  unsigned lo, hi, aux;
  __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux) :: "memory");
  return aux;
}

/* Paired serialized variants for lock_defer vs total validation. */
static __inline__ ci_uint64 oo_rdtsc_begin(void)
{
  unsigned lo, hi, aux;
  __asm__ __volatile__("lfence\n\trdtscp\n\tlfence"
                       : "=a"(lo), "=d"(hi), "=c"(aux) :: "memory");
  return ((ci_uint64)hi << 32) | lo;
}
static __inline__ ci_uint64 oo_rdtsc_end(void)
{
  unsigned lo, hi, aux;
  __asm__ __volatile__("rdtscp\n\tlfence"
                       : "=a"(lo), "=d"(hi), "=c"(aux) :: "memory");
  return ((ci_uint64)hi << 32) | lo;
}
static __inline__ ci_uint64 oo_rdtsc_serialized(void)
{
  return oo_rdtsc_begin();
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
  ci_uint64 hdr;    /* header prep loop in advance_to */
  ci_uint64 push;   /* ci_ip_send_tcp_list: NIC push/PIO/doorbell */
  ci_uint64 trailing_poll;   /* trailing poll in advance_nagle */
  ci_uint64 resync; /* ci_ip_time_resync */
  ci_uint64 poll_evq; /* evq drain inside trailing poll */
  ci_uint64 poll_tmr; /* timer poll inside trailing poll */
  ci_uint64 tx_prequeue;   /* ci_tcp_tx_prequeue (CAS link) */
  ci_uint64 lock_defer;    /* ci_netif_lock_or_defer_work (wait/defer) */
  ci_uint64 br1;     /* lock_or_defer branch1: defer_work_limit hit, ci_netif_lock */
  ci_uint64 br1_work;/* br1: citp_waitable_deferred_work while holding lock */
  ci_uint64 br2;     /* lock_or_defer branch2: DEFERRED_BIT already set, ci_netif_lock */
  ci_uint64 br2_work;/* br2: citp_waitable_deferred_work while holding lock */
  ci_uint64 br3;     /* lock_or_defer branch3: primary trylock/CAS path */
  ci_uint64 br3_work;/* br3 trylock-hit: deferred_work while holding lock */
  ci_uint64 br3_spins; /* br3 while(1) iterations before success */
  ci_uint64 br3_lock;  /* br3 trylock-hit count (lock free, return 1) */
  ci_uint64 br3_defer; /* br3 CAS-defer count (lock held, return 0) */
  ci_uint64 entry;   /* entry checks: limit load + test_and_set */
  ci_uint64 enter_gap; /* fn entry -> __total (asserts blind spot) */
  ci_uint64 total;   /* whole ci_netif_lock_or_defer_work */
} oo_adv_lat_t;
extern __thread oo_adv_lat_t oo_adv_tls;
#define OO_ADV_ACC(_f, _cyc) do { oo_adv_tls._f += (_cyc); } while(0)
typedef struct {
  ci_uint64 t0;
  ci_uint64 prologue;   /* entry -> fast_path decision (iov scan etc) */
  ci_uint64 trylock;    /* si_trylock attempts */
  ci_uint64 fill_tail;  /* ci_tcp_tx_fill_sendq_tail (fast path) */
  ci_uint64 flags;      /* PSH/MORE flag setup */
  ci_uint64 adv_nagle;  /* ci_tcp_tx_advance_nagle */
  ci_uint64 adv_direct;        /* ci_tcp_tx_advance */
  ci_uint64 space; /* ci_tcp_tx_send_space */
  ci_uint64 alloc;      /* ci_tcp_send_alloc_pkts */
  ci_uint64 fill;       /* ci_tcp_send_fill_pkts (incl memcpy) */
  ci_uint64 enqueue;    /* ci_tcp_sendmsg_enqueue */
  ci_uint64 done_flags; /* final PSH/ACK flag setup */
  ci_uint64 poll_main;       /* ci_netif_poll */
  ci_uint64 prequeue;   /* ci_tcp_send_via_prequeue */
  ci_uint64 unlock;     /* ci_netif_unlock */
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

/* Time one segment: OO_SEG(alloc, m = ci_tcp_send_alloc_pkts(...)); */
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
      __lat.poll_main += oo_rdtsc() - __s0; \
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
        unsigned long long __total = oo_cyc_to_ns(__tot); \
        unsigned long long __prologue = oo_cyc_to_ns(__lat.prologue); \
        unsigned long long __trylock = oo_cyc_to_ns(__lat.trylock); \
        unsigned long long __fill_tail = oo_cyc_to_ns(__lat.fill_tail); \
        unsigned long long __flags = oo_cyc_to_ns(__lat.flags); \
        unsigned long long __adv_nagle = oo_cyc_to_ns(__lat.adv_nagle); \
        unsigned long long __adv_direct = oo_cyc_to_ns(__lat.adv_direct); \
        unsigned long long __space = oo_cyc_to_ns(__lat.space); \
        unsigned long long __alloc = oo_cyc_to_ns(__lat.alloc); \
        unsigned long long __fill = oo_cyc_to_ns(__lat.fill); \
        unsigned long long __enqueue = oo_cyc_to_ns(__lat.enqueue); \
        unsigned long long __done_flags = oo_cyc_to_ns(__lat.done_flags); \
        unsigned long long __poll_main = oo_cyc_to_ns(__lat.poll_main); \
        unsigned long long __prequeue = oo_cyc_to_ns(__lat.prequeue); \
        unsigned long long __tx_prequeue = oo_cyc_to_ns(oo_adv_tls.tx_prequeue); \
        unsigned long long __lock_defer = oo_cyc_to_ns(oo_adv_tls.lock_defer); \
        unsigned long long __br1 = oo_cyc_to_ns(oo_adv_tls.br1); \
        unsigned long long __br1_work = oo_cyc_to_ns(oo_adv_tls.br1_work); \
        unsigned long long __br2 = oo_cyc_to_ns(oo_adv_tls.br2); \
        unsigned long long __br2_work = oo_cyc_to_ns(oo_adv_tls.br2_work); \
        unsigned long long __br3 = oo_cyc_to_ns(oo_adv_tls.br3); \
        unsigned long long __br3_work = oo_cyc_to_ns(oo_adv_tls.br3_work); \
        unsigned long long __br3_spins = (unsigned long long)oo_adv_tls.br3_spins; \
        unsigned long long __br3_lock = (unsigned long long)oo_adv_tls.br3_lock; \
        unsigned long long __br3_defer = (unsigned long long)oo_adv_tls.br3_defer; \
        unsigned long long __entry = oo_cyc_to_ns(oo_adv_tls.entry); \
        unsigned long long __enter_gap = oo_cyc_to_ns(oo_adv_tls.enter_gap); \
        unsigned long long __ld_total = oo_cyc_to_ns(oo_adv_tls.total); \
        unsigned long long __unlock = oo_cyc_to_ns(__lat.unlock); \
        unsigned long long __resync = oo_cyc_to_ns(oo_adv_tls.resync); \
        unsigned long long __hdr = oo_cyc_to_ns(oo_adv_tls.hdr); \
        unsigned long long __push = oo_cyc_to_ns(oo_adv_tls.push); \
        unsigned long long __trailing_poll = oo_cyc_to_ns(oo_adv_tls.trailing_poll); \
        unsigned long long __poll_evq = oo_cyc_to_ns(oo_adv_tls.poll_evq); \
        unsigned long long __poll_tmr = oo_cyc_to_ns(oo_adv_tls.poll_tmr); \
        unsigned long long __sum = __prologue+__trylock+__fill_tail+__flags+__adv_nagle+__adv_direct+__space+__alloc+ \
            __fill+__enqueue+__done_flags+__poll_main+__prequeue+__tx_prequeue+__lock_defer+__unlock; \
        long long __residual = (long long)__total - (long long)__sum; \
        if( __residual < 0 ) __residual = 0; \
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
          "  [tx_prequeue=%lluns lock_defer=%lluns{br1=%lluns+work=%lluns br2=%lluns+work=%lluns br3=%lluns(spins=%llu,lock=%llu,defer=%llu)+work=%lluns entry=%lluns total=%lluns enter_gap=%lluns}] unlock=%lluns\n" \
          "  |-pkt_bufs: alloc=%d free=%d rx=%d tx_inuse=%d\n" \
          "  |-unaccounted(residual)=%lluns\n", \
               (_where), __total, (int)(_rc), \
               (int)((_ts)->send.num), (int)((_ts)->retrans.num), \
               __lat.lock_ok, __lat.via_prequeue, \
               __prologue, __trylock, __space, __alloc, __lat.alloc_cnt, __fill, __enqueue, __done_flags, \
               __fill_tail, __flags, __adv_nagle, __resync, __hdr, __push, __trailing_poll, __poll_evq, __poll_tmr, \
               __adv_direct, __poll_main, __lat.poll_cnt, __prequeue, __tx_prequeue, __lock_defer, __br1, __br1_work, __br2, __br2_work, __br3, __br3_spins, __br3_lock, __br3_defer, __br3_work, __entry, __ld_total, __enter_gap, __unlock, \
               __pb_alloc, __pb_free, __pb_rx, __pb_tx, __residual); } \
      } \
    } \
  } while(0)

#endif /* __KERNEL__ */
#endif
