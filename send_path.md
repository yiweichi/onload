下面按你这个仓库当前代码，沿着一次普通的：

```c
send(fd, buf, len, 0);
```

从应用层一直跟到 NIC 的路径过一遍。

先说一个重要概念：

> `send()` 不是先把 packet 交给一个独立的“Onload 程序”。Onload 主要以用户态共享库的形式注入应用进程，通过拦截 libc/socket API，在应用进程内部直接完成 TCP/IP 处理和网卡发送。

整体路径大致是：

```text
应用程序
  │
  └─ send()
       │
       ▼
Onload 拦截函数：onload_send()
       │
       ▼
fd table 查找 fd
       │
       ▼
TCP fd 操作函数：citp_tcp_send()
       │
       ▼
TCP 协议栈：ci_tcp_sendmsg()
       │
       ├─ 分配 Onload packet buffer
       ├─ 从应用 buffer 拷贝 payload
       ├─ 填 TCP/IP/Ethernet header
       ├─ 放入 TCP send queue
       ├─ 处理 congestion/window/Nagle
       ▼
TCP transmit advance
       │
       ▼
__ci_netif_send()
       │
       ├─ CTPIO
       ├─ PIO
       ├─ DMA direct transmit
       └─ DMA overflow queue
       ▼
ef_vi transmit API
       │
       ▼
网卡 TX ring / doorbell
       │
       ▼
NIC DMA 读取 packet buffer
       │
       ▼
网卡发出 Ethernet frame
```

---

# 1. 应用调用 `send()`

假设应用代码是：

```c
char buf[] = "hello";
send(fd, buf, sizeof(buf), 0);
```

应用看到的是标准 BSD socket API，并不知道底层是否使用 Onload。

Onload 通过动态库拦截 `send()`。对应代码在：

```937:976:/home/yiweichi/onload/src/lib/transport/unix/sockcall_intercept.c
OO_INTERCEPT(ssize_t, send,
             (int fd, const void* msg, size_t len, int flags))
{
  citp_fdinfo* fdi=0;
  struct msghdr m;
  int rc;
  citp_lib_context_t lib_context;

  if( CI_UNLIKELY(citp.init_level < CITP_INIT_ALL) ) {
    citp_do_init(CITP_INIT_SYSCALLS);
    return ci_sys_send(fd, msg, len, flags);
  }

  Log_CALL(log("%s(%d, %p, %u, %x)", __FUNCTION__, fd, msg, (unsigned)len, flags));

  if( (fdi = citp_fdtable_lookup_fast(&lib_context, fd)) ) {
    m.msg_iov = (struct iovec*) &msg;
    m.msg_iovlen = 1;
    CI_DEBUG(m.msg_control = CI_NOT_NULL);
    m.msg_controllen = 0;
    /* msg_flags is output only */
    rc = citp_fdinfo_get_ops(fdi)->send(fdi, &m, flags);
    citp_fdinfo_release_ref_fast(fdi);
    citp_exit_lib(&lib_context, rc >= 0);
  }
  else {
    Log_PT(log("PT: sys_send(%d, msg, %d, 0x%x)", fd, (int) len,
               (unsigned) flags));
    citp_exit_lib_if(&lib_context, TRUE);
    rc = ci_sys_send(fd, msg, len, flags);
  }
  Log_CALL_RESULT(rc);
  return rc;
}
```

这里做了几件事。

## 1.1 初始化 Onload syscall 拦截

如果 Onload 还没有完成初始化：

```c
citp_do_init(CITP_INIT_SYSCALLS);
```

然后才继续走 Onload 路径。

## 1.2 根据 fd 查找 Onload fd

```c
citp_fdtable_lookup_fast(&lib_context, fd)
```

Onload 内部维护了一张 fd table，用来判断这个 fd 是：

- Onload 加速的 TCP socket；
- Onload 加速的 UDP socket；
- Linux 原生 socket；
- pipe、epoll 或其他特殊 fd。

## 1.3 如果不是 Onload socket，就回退到 Linux

如果查找不到 Onload fd：

```c
rc = ci_sys_send(fd, msg, len, flags);
```

也就是说，Onload 并不会强行接管所有 fd。

## 1.4 如果是 Onload socket，转发给该 fd 的操作表

```c
rc = citp_fdinfo_get_ops(fdi)->send(fdi, &m, flags);
```

对于 TCP socket，这个函数指针最终对应：

```c
citp_tcp_send()
```

---

# 2. 进入 TCP fd 层：`citp_tcp_send()`

TCP fd 的 send 实现在：

```1745:1782:/home/yiweichi/onload/src/lib/transport/unix/tcp_fd.c
static int citp_tcp_send(citp_fdinfo* fdinfo, const struct msghdr* msg,
                         int flags)
{
  citp_sock_fdi* epi = fdi_to_sock_fdi(fdinfo);
  int rc;

  ci_assert(msg != NULL);

  if( epi->sock.s->b.sb_aflags & (CI_SB_AFLAG_O_NONBLOCK |
                                  CI_SB_AFLAG_O_NDELAY) ) {
    flags |= MSG_DONTWAIT;
  }

  if(CI_LIKELY( msg->msg_iov != NULL && msg->msg_iovlen > 0 )) {
    ci_uint32 state;

    state = OO_ACCESS_ONCE(epi->sock.s->b.state);

    if( CI_UNLIKELY(state == CI_TCP_CLOSED || state == CI_TCP_LISTEN ||
                    state == CI_TCP_INVALID) ) {
      if( (rc = ci_get_so_error(epi->sock.s)) != 0 )
        CI_SET_ERROR(rc, rc);
      else
        CI_SET_ERROR(rc, EPIPE);
    }
    else {
      rc = ci_tcp_sendmsg(epi->sock.netif, SOCK_TO_TCP(epi->sock.s),
                          msg->msg_iov, msg->msg_iovlen, flags);
    }
  }
```

`citp_tcp_send()` 主要负责：

1. 把 socket fdinfo 转成 Onload TCP socket；
2. 检查 `O_NONBLOCK`；
3. 检查 TCP 状态；
4. 调用真正的 TCP 发送函数。

真正进入 TCP 协议栈的调用是：

```c
ci_tcp_sendmsg(epi->sock.netif,
               SOCK_TO_TCP(epi->sock.s),
               msg->msg_iov,
               msg->msg_iovlen,
               flags);
```

这里的两个核心对象是：

```text
epi->sock.netif
    当前 Onload stack，也就是 ci_netif

SOCK_TO_TCP(epi->sock.s)
    当前 TCP socket，也就是 ci_tcp_state
```

因此从这一刻开始，发送动作已经在某一个 Onload stack 的共享状态中执行了。

---

# 3. 进入 `ci_tcp_sendmsg()`

核心函数位于：

```1873:1908:/home/yiweichi/onload/src/lib/transport/ip/tcp_send.c
int ci_tcp_sendmsg(ci_netif* ni, ci_tcp_state* ts,
                   const ci_iovec* iov, unsigned long iovlen,
                   int flags
                   CI_KERNEL_ARG(ci_addr_spc_t addr_spc))
{
  ci_ip_pkt_queue* sendq = &ts->send;
  ci_ip_pkt_fmt* pkt;
  ci_iovec_ptr piov;
  int m;
  struct tcp_send_info sinf;
  int af = ipcache_af(&ts->s.pkt);

  ci_assert(iov != NULL);
  ci_assert_gt(iovlen, 0);
  ci_assert(ts);
  ci_assert(ts->s.b.state != CI_TCP_LISTEN);

  if( ts->snd_delegated ) {
    int rc;
    CI_SET_ERROR(rc, EBUSY);
    return rc;
  }

  sinf.rc = 0;
  sinf.stack_locked = 0;
  sinf.total_unsent = 0;
  sinf.total_sent = 0;
  sinf.pf.alloc_pkt = NULL;
  sinf.timeout = ts->s.so.sndtimeo_msec;
```

其中：

```text
ni
  当前 Onload stack

ts
  TCP socket 的完整协议状态

ts->send
  TCP 发送队列

iov
  应用提供的 buffer 描述

sinf
  本次 send 操作的临时状态
```

注意，`ci_tcp_sendmsg()` 并不是直接把应用的 `buf` 指针交给 NIC。NIC 不能直接读取普通用户态应用 buffer。Onload 必须把数据放入适合 DMA 的 packet buffer。

---

# 4. 计算本次发送的数据量

函数首先遍历应用传入的 iovec：

```1909:1937:/home/yiweichi/onload/src/lib/transport/ip/tcp_send.c
  if(CI_UNLIKELY( (~ts->s.b.state & CI_TCP_STATE_SYNCHRONISED) ))
    goto not_synchronised;

 is_sync:

#define MAX_SEND_CHUNK 0x3fffffff
  for( m = 0; m < (int)iovlen; ++m ) {
    sinf.total_unsent += CI_IOVEC_LEN(&iov[m]);
    if(CI_UNLIKELY( CI_IOVEC_BASE(&iov[m]) == NULL &&
                    CI_IOVEC_LEN(&iov[m]) > 0 )) {
      sinf.rc = -EFAULT;
      ci_tcp_sendmsg_handle_rc_or_tx_errno(ni, ts, flags, &sinf);
      if( sinf.set_errno ) CI_SET_ERROR(sinf.rc, sinf.rc);
      return sinf.rc;
    }
    if( CI_IOVEC_LEN(&iov[m]) > MAX_SEND_CHUNK ||
        sinf.total_unsent > MAX_SEND_CHUNK ) {
      sinf.total_unsent = MAX_SEND_CHUNK;
      break;
    }
  }
#undef MAX_SEND_CHUNK
```

这里主要做：

- 统计应用要求发送多少字节；
- 检查 iovec 指针；
- 防止长度整数溢出；
- 确认连接是否已经同步建立。

例如：

```text
应用要求发送 5000 字节
TCP 有效 MSS = 1460
```

那么后续大概率需要构造多个 TCP segment：

```text
segment 1: 1460 bytes
segment 2: 1460 bytes
segment 3: 1460 bytes
segment 4: 620 bytes
```

实际 MSS 还会受到 TCP options、IPv4/IPv6、路径 MTU 等因素影响。

---

# 5. 分配 Onload packet buffer

在 fast path 中，代码会调用：

```2050:2059:/home/yiweichi/onload/src/lib/transport/ip/tcp_send.c
  while( 1 ) {
    /* Grab packet buffers and fill them with data. */
    m = ci_tcp_send_alloc_pkts(ni, ts, &sinf, 0);
    if( sinf.n_needed > 0 )
      goto no_pkt_buf;

  got_pkt_buf:
    ci_tcp_send_fill_pkts(ni, ts, &sinf, &piov, m CI_KERNEL_ARG(addr_spc));
```

这里：

```c
ci_tcp_send_alloc_pkts()
```

负责从当前 Onload stack 的 packet buffer pool 中分配 packet 对象。

这些 packet buffer 通常由 Onload 预先准备好，并且具备：

- 适合 DMA 的内存；
- 对应的 DMA 地址；
- Onload 内部 packet metadata；
- Ethernet/IP/TCP header 空间；
- 发送队列相关状态。

可以把一个 `ci_ip_pkt_fmt` 理解成：

```text
packet object
├── Ethernet header
├── IP header
├── TCP header
├── TCP payload
├── DMA address
├── buffer length
├── interface index
├── TCP sequence metadata
└── TX queue metadata
```

分配的是 Onload 管理的 packet，不是直接使用应用的 `buf`。

---

# 6. 把应用 payload 拷贝进 packet buffer

随后调用：

```c
ci_tcp_send_fill_pkts()
```

最终会使用 `ci_tcp_sendmsg_fill_pkt()`，它的核心逻辑是：

```164:210:/home/yiweichi/onload/src/lib/transport/ip/tcp_send.c
static
int ci_tcp_sendmsg_fill_pkt(ci_netif* ni, ci_tcp_state* ts,
                            struct tcp_send_info* sinf,
                            ci_iovec_ptr* piov, int hdrlen,
                            int maxlen
                            CI_KERNEL_ARG(ci_addr_spc_t addr_spc))
{
  /* Initialise and fill a packet buffer from an iovec. */
  int n;
  ci_ip_pkt_fmt* pkt = oo_pkt_filler_next_pkt(ni, &sinf->pf, sinf->stack_locked);

  ci_assert(pkt);
  ci_assert(! ci_iovec_ptr_is_empty_proper(piov));
  ci_tcp_tx_pkt_init(pkt, hdrlen, maxlen);
  oo_pkt_filler_init(&sinf->pf, pkt,
                     (uint8_t*) oo_tx_l3_hdr(pkt) + hdrlen);

  n = sinf->total_unsent - sinf->fill_list_bytes;
  n = CI_MIN(maxlen, n);
  sinf->rc = oo_pkt_fill(ni, &ts->s, NULL/*p_netif_locked*/,
                         CI_FALSE/*can_block*/, &sinf->pf, piov,
                         n CI_KERNEL_ARG(addr_spc));

  ci_assert_ge(oo_offbuf_left(&pkt->buf), n);
  oo_offbuf_advance(&pkt->buf, n);

  pkt->pf.tcp_tx.end_seq = n;

  ci_assert_equal(TX_PKT_LEN(pkt),
                  oo_offbuf_ptr(&pkt->buf) - PKT_START(pkt));
  return n;
}
```

这里的关键步骤是：

## 6.1 获取一个 packet

```c
oo_pkt_filler_next_pkt()
```

从 packet pool 取一个 packet。

## 6.2 初始化发送 buffer

```c
ci_tcp_tx_pkt_init(pkt, hdrlen, maxlen);
```

初始化 packet 布局，为 Ethernet/IP/TCP header 和 payload 保留空间。

## 6.3 设置 payload 起始位置

```c
oo_pkt_filler_init(&sinf->pf, pkt,
                   (uint8_t*) oo_tx_l3_hdr(pkt) + hdrlen);
```

payload 会从 TCP header 后面开始写入。

## 6.4 拷贝应用数据

```c
oo_pkt_fill(..., piov, n, ...);
```

这一步从应用传入的 iovec 中读取数据，并写入 Onload packet buffer。

从内存角度看，大概是：

```text
应用内存：
+-----------------------+
| "hello"               |
+-----------------------+
          │
          │ oo_pkt_fill()
          ▼
Onload packet buffer：
+ Ethernet + IP + TCP + "hello" +
```

## 6.5 记录 TCP sequence space

```c
pkt->pf.tcp_tx.end_seq = n;
```

这时暂时只记录本 packet 消耗多少 TCP sequence space，后续放入 send queue 时会把真实 sequence number 修正进去。

---

# 7. 设置 TCP/IP header 和 sequence number

packet 被填充后，会经过：

```c
ci_tcp_sendmsg_prep_pkt()
```

代码中核心部分是：

```332:365:/home/yiweichi/onload/src/lib/transport/ip/tcp_send.c
ci_inline void ci_tcp_sendmsg_prep_pkt(ci_netif* ni, ci_tcp_state* ts,
                                       ci_ip_pkt_fmt* pkt, unsigned seq)
{
  int orig_hdrlen, extra_opts;

  ci_ipcache_update_flowlabel(ni, &ts->s);

  /* Copy in the headers */
  ci_pkt_init_from_ipcache(pkt, &ts->s.pkt);

  orig_hdrlen = (int)pkt->pf.tcp_tx.start_seq;

  pkt->pf.tcp_tx.start_seq = seq;
  pkt->pf.tcp_tx.end_seq += seq;

  pkt->pf.tcp_tx.block_end = OO_PP_NULL;
```

这个阶段主要完成：

- 根据 socket 的连接信息生成 Ethernet/IP header；
- 设置源 IP、目的 IP；
- 设置源端口、目的端口；
- 设置 Ethernet source/destination MAC；
- 设置 TCP sequence number；
- 设置 TCP ACK number；
- 设置 TCP options；
- 设置 IPv4/IPv6 字段；
- 记录 packet 的 sequence range。

例如，如果当前发送序列号是：

```text
seq = 10000
payload length = 100
```

那么这个 segment 会表示：

```text
TCP sequence = 10000
TCP sequence end = 10100
```

之后还会根据具体发送时机设置 flags：

```text
ACK
PSH | ACK
```

如果应用使用了 `MSG_MORE` 或 TCP cork，最后一个 packet 可能暂时不设置 PSH，也可能暂时不立即发送。

---

# 8. 放入 TCP send queue

packet 准备好后，代码会把它放入 socket 的 TCP send queue：

```2067:2086:/home/yiweichi/onload/src/lib/transport/ip/tcp_send.c
      /* eff_mss may now be != ts->eff_mss */
      ts->send_in += ci_tcp_sendmsg_enqueue(ni, ts,
                                            sinf.fill_list,
                                            sinf.fill_list_bytes,
                                            &ts->send);
      sinf.total_sent += sinf.fill_list_bytes;
      sinf.total_unsent -= sinf.fill_list_bytes;

      /* Now we've sent all the packets we grabbed, but not necessarily all
       * of the data -- so check to see if we're done yet.  The last
       * segment gets the PSH flag.
       */
      if( sinf.total_unsent == 0 ) {
        if( (sinf.fill_list->flags & CI_PKT_FLAG_TX_MORE) )
          TX_PKT_IPX_TCP(af, sinf.fill_list)->tcp_flags = CI_TCP_FLAG_ACK;
        else
          TX_PKT_IPX_TCP(af, sinf.fill_list)->tcp_flags =
              CI_TCP_FLAG_PSH | CI_TCP_FLAG_ACK;
```

这里的 `ts->send` 是 TCP 发送队列。

此时要区分几个状态：

```text
应用 buffer
  └─ 已经被复制到 Onload packet buffer

packet buffer
  └─ 已经放入 TCP send queue

TCP send queue
  └─ 还不一定已经提交给 NIC

NIC TX ring
  └─ 才表示已经交给硬件发送队列
```

所以，`send()` 返回成功，并不一定表示 packet 已经到达 NIC，更不表示对端已经收到。通常只表示数据已经被 Onload 接收并成功放入发送处理流程。

---

# 9. 获得 Onload stack lock

Onload stack 内部有共享状态，因此发送时需要获取 stack lock：

```c
if( si_trylock(ni, &sinf) )
```

如果成功：

```text
当前线程直接处理 send queue
```

如果失败：

```text
当前线程暂时无法持有 stack lock
```

代码会把 packet 放到 prequeue：

```2065:2116:/home/yiweichi/onload/src/lib/transport/ip/tcp_send.c
  filled_some_pkts:
    /* If we can grab the lock now, setup the meta-data and get sending.
     * Otherwise queue the packets for the netif lock holder.
     */
    if( si_trylock(ni, &sinf) ) {
      ...
      ts->send_in += ci_tcp_sendmsg_enqueue(ni, ts,
                                            sinf.fill_list,
                                            sinf.fill_list_bytes,
                                            &ts->send);
      ...
      if( CI_LIKELY( ! ci_ip_queue_is_empty(sendq) ))
        ci_tcp_tx_advance(ts, ni);
    }
    else {
      ...
      /* Couldn't get the netif lock, so enqueue packets on the prequeue. */
      if( ! ci_tcp_send_via_prequeue(ni, ts, &sinf) ) {
        ...
      }
```

因此发送路径有两种：

```text
拿到 stack lock：
    packet -> TCP send queue -> tx advance

没拿到 stack lock：
    packet -> TCP prequeue
                    │
                    └─ 之后由持锁线程合并/处理
```

这也是为什么多个线程共享同一个 Onload stack 时可能出现锁竞争。

---

# 10. TCP transmit advance

当 packet 已经进入 `ts->send` 后，调用：

```c
ci_tcp_tx_advance(ts, ni);
```

你在 `tcp_send.c` 中可以看到它被调用的位置，例如：

```88:113:/home/yiweichi/onload/src/lib/transport/ip/tcp_send.c
static void ci_tcp_tx_advance_nagle(ci_netif* ni, ci_tcp_state* ts)
{
  /* Nagle's algorithm ... */

  ci_ip_pkt_queue* sendq = &ts->send;
  ci_ip_pkt_fmt* pkt = NULL;

  if( (sendq->num != 1) | (!ci_tcp_is_inflight(ts)) |
      OO_SP_NOT_NULL(ts->local_peer) ) {
  advance_now:
    ci_ip_time_resync(IPTIMER_STATE(ni));
    ci_tcp_tx_advance(ts, ni);
    if(CI_UNLIKELY( ts->tcpflags & CI_TCPT_FLAG_MSG_WARM))
      return;
    goto poll_and_out;
  }
```

以及：

```2083:2087:/home/yiweichi/onload/src/lib/transport/ip/tcp_send.c
      if(CI_LIKELY( ! ci_ip_queue_is_empty(sendq) ))
        ci_tcp_tx_advance(ts, ni);
```

`ci_tcp_tx_advance()` 是从 TCP send queue 向下继续推进的核心函数。虽然它的具体实现不在当前这个 `tcp_send.c` 文件中，但从调用语义看，它负责：

- 检查发送窗口；
- 检查 congestion window；
- 检查 retransmit 状态；
- 应用 Nagle 规则；
- 选择当前可以发送的 packet；
- 处理 TCP sequence；
- 将 packet 标记为待发送；
- 进入 netif TX 路径。

这里有三个重要限制。

## 10.1 TCP send buffer

如果发送队列或 socket send buffer 没有空间，`send()` 可能：

- 阻塞；
- 返回部分发送长度；
- 非阻塞模式返回 `EAGAIN`。

## 10.2 TCP receive window

对端通告的接收窗口可能限制当前能发送多少数据。

## 10.3 congestion window

TCP 拥塞控制也会限制当前可以放入网络中的数据量。

所以即使 Onload 已经从应用 buffer 复制了数据，也可能暂时留在 TCP send queue，等待 ACK、窗口或拥塞窗口推进。

---

# 11. 从 TCP 层进入 netif TX 层

最终，待发送的 packet 会进入：

```c
__ci_netif_send(ni, pkt)
```

实现位于：

```222:249:/home/yiweichi/onload/src/lib/transport/ip/netif_tx.c
void __ci_netif_send(ci_netif* netif, ci_ip_pkt_fmt* pkt)
{
  int intf_i, rc;
  oo_pktq* dmaq;
  ef_vi* vi;
  ef_iovec iov[CI_IP_PKT_SEGMENTS_MAX];
  int iov_len;

  ci_assert(netif);
  ci_assert(pkt);
  ci_assert(pkt->intf_i >= 0);
  ci_assert(pkt->intf_i < CI_CFG_MAX_INTERFACES);
  ci_assert_flags(pkt->flags, CI_PKT_FLAG_TX_PENDING);

  ___ci_netif_dmaq_insert_prep_pkt(netif, pkt);

  intf_i = pkt->intf_i;

  dmaq = ci_netif_dmaq(netif, intf_i);
  vi = ci_netif_vi(netif, intf_i);
```

这一步已经从 TCP 协议层进入网卡发送层。

这里的对象：

```text
netif
  当前 Onload stack

pkt
  一个已经构造好的完整 Ethernet frame

intf_i
  选择的网络接口

vi
  该接口对应的 EF_VI virtual interface

dmaq
  DMA overflow queue
```

packet 必须有：

```c
CI_PKT_FLAG_TX_PENDING
```

表示它已经准备好发送。

---

# 12. 选择 PIO、CTPIO 或 DMA

Onload 不只有一种硬件发送方式。这里有三条主要路径：

```text
1. CTPIO
2. PIO
3. DMA
```

具体选择受到：

- NIC 类型；
- packet 长度；
- TX ring 空间；
- PIO 是否开启；
- CTPIO 配置；
- packet 是否连续；
- 当前发送队列状态；

等因素影响。

---

## 12.1 PIO 路径

如果 packet 较小，且 PIO 可用：

```245:292:/home/yiweichi/onload/src/lib/transport/ip/netif_tx.c
  if( oo_pktq_is_empty(dmaq) ) {
#if CI_CFG_PIO
    order = ci_log2_ge(pkt->pay_len, CI_CFG_MIN_PIO_BLOCK_ORDER);
    buddy = &netif->state->nic[intf_i].pio_buddy;
    if( ! ci_netif_may_ctpio(netif, intf_i, pkt->pay_len) &&
        netif->state->nic[intf_i].oo_vi_flags & OO_VI_FLAGS_PIO_EN ) {
      if( pkt->pay_len <= NI_OPTS(netif).pio_thresh && pkt->n_buffers == 1 ) {
        if( (offset = ci_pio_buddy_alloc(netif, buddy, order)) >= 0 ) {
          rc = ef_vi_transmit_copy_pio(vi,
                                       offset, PKT_START(pkt), pkt->buf_len,
                                       OO_PKT_ID(pkt));
          if( rc == 0 ) {
            CITP_STATS_NETIF_INC(netif, pio_pkts);
            ci_assert(pkt->pio_addr == -1);
            pkt->pio_addr = offset;
            pkt->pio_order = order;
            return;
          }
```

PIO 的含义是：

```text
CPU/用户态代码
    │
    └─ 把 packet 内容复制到 NIC 的 PIO memory
                              │
                              └─ NIC 读取并发送
```

调用：

```c
ef_vi_transmit_copy_pio()
```

这里 packet 的内容被复制到网卡映射的 PIO 区域，而不是让网卡通过 DMA 读取原 packet buffer。

特点：

- 对小 packet 延迟较低；
- 需要 packet 足够小；
- 需要单个 buffer；
- 需要 PIO 资源可用；
- 不适合大 packet。

---

## 12.2 CTPIO 路径

CTPIO 是更低延迟的发送方式。代码中相关函数是：

```31:60:/home/yiweichi/onload/src/lib/transport/ip/netif_tx.c
#if CI_CFG_CTPIO
static inline int tx_ctpio(ci_netif* ni, int intf_i, ef_vi* vi,
                           ci_ip_pkt_fmt* pkt, const ef_iovec *iov,
                           int iov_len)
{
  ci_netif_state_nic_t* nsn = &ni->state->nic[intf_i];
  struct iovec host_iov[CI_IP_PKT_SEGMENTS_MAX];
  int total_length;
  int rc;

  total_length = ci_netif_pkt_to_host_iovec(ni, pkt, host_iov,
                                      sizeof(host_iov) / sizeof(host_iov[0]));

  oo_pkt_calc_checksums(ni, pkt, host_iov);
  ef_vi_transmitv_ctpio(vi, total_length, host_iov,
                        iov_len, nsn->ctpio_ct_threshold);
  CITP_STATS_NETIF_INC(ni, ctpio_pkts);
  rc = ef_vi_transmitv_ctpio_fallback(vi, iov, iov_len,
                                      OO_PKT_ID(pkt));
  return rc;
}
#endif
```

CTPIO 典型逻辑是：

```text
CPU 直接把 packet 写入 NIC 的 transmit datapath
          │
          ├─ NIC 立即发送
          │
          └─ 如果 CTPIO 失败，fallback 到 DMA
```

代码中：

```c
ef_vi_transmitv_ctpio()
```

执行 CTPIO 发送；

```c
ef_vi_transmitv_ctpio_fallback()
```

在必要时准备 fallback 的 DMA 发送。

CTPIO 通常用于对延迟特别敏感、长度较小且满足条件的 packet。

---

## 12.3 普通 DMA 路径

如果不走 PIO/CTPIO，代码会构造 EF_VI 的 iovec：

```228:240:/home/yiweichi/onload/src/lib/transport/ip/netif_tx.h
ci_inline int ci_netif_pkt_to_iovec(ci_netif* ni, ci_ip_pkt_fmt* pkt,
                                    ef_iovec* iov, unsigned iovlen)
{
  int i, intf_i = pkt->intf_i;
  unsigned n = pkt->n_buffers;

  iov[0].iov_base = pkt_dma_addr(ni, pkt, intf_i) + pkt->pkt_start_off;
  iov[0].iov_len = pkt->buf_len;

  if(CI_UNLIKELY( n > 1 )) {
    for( i = 1; i < n; ++i ) {
      pkt = PKT_CHK(ni, pkt->frag_next);
      iov[i].iov_base = pkt_dma_addr(ni, pkt, intf_i) + pkt->pkt_start_off;
      iov[i].iov_len = pkt->buf_len;
    }
  }
  return n;
}
```

这个函数不再使用普通虚拟地址，而是转换成：

```c
pkt_dma_addr(ni, pkt, intf_i)
```

也就是 NIC 能够访问的 DMA 地址。

例如 packet 在内存中可能是：

```text
packet buffer 0:
  DMA address = 0x12340000
  length      = 900

packet buffer 1:
  DMA address = 0x12350000
  length      = 600
```

最终形成：

```text
ef_iovec[0] = { .iov_base = 0x12340000, .iov_len = 900 }
ef_iovec[1] = { .iov_base = 0x12350000, .iov_len = 600 }
```

然后调用：

```302:318:/home/yiweichi/onload/src/lib/transport/ip/netif_tx.c
    calc_csum_if_needed(netif, vi, pkt);
    iov_len = ci_netif_pkt_to_iovec(netif, pkt, iov,
                                    sizeof(iov) / sizeof(iov[0]));

#if CI_CFG_CTPIO
    if( (iov_len > 0) && (iov_len <= CI_IP_PKT_SEGMENTS_MAX) &&
        ci_netif_may_ctpio(netif, intf_i, pkt->pay_len) ) {
      rc = tx_ctpio(netif, intf_i, vi, pkt, iov, iov_len);
    }
    else
#endif
    if( (rc = ef_vi_transmitv(vi, iov, iov_len, OO_PKT_ID(pkt))) == 0 ) {
      ci_netif_ctpio_desist(netif, intf_i);
      CITP_STATS_NETIF_INC(netif, tx_dma_doorbells);
    }
    if( rc == 0 ) {
      return;
    }
```

普通 DMA 调用是：

```c
ef_vi_transmitv(vi, iov, iov_len, OO_PKT_ID(pkt))
```

这一步的含义是：

> 把一组 packet buffer 的 DMA 地址和长度，提交给 EF_VI/NIC 的 TX 发送队列。

---

# 13. `ef_vi_transmitv()` 做了什么

`ef_vi` 是 EF_VI 层提供的用户态网卡接口。

Onload 使用它来操作 NIC 的 Virtual Interface。

可以把：

```c
ef_vi* vi
```

理解为：

```text
当前 Onload stack 对某块 NIC 的用户态发送/接收接口
```

`ef_vi_transmitv()` 大致会：

1. 检查 TX ring 是否有空间；
2. 创建硬件 TX descriptor；
3. 把 packet 的 DMA 地址写入 descriptor；
4. 把 packet 的长度写入 descriptor；
5. 保存 packet ID，便于发送完成后回收；
6. 更新 TX producer index；
7. 触发或等待 doorbell。

逻辑示意：

```text
Onload packet buffer
        │
        │ DMA address + length
        ▼
EF_VI TX descriptor
        │
        ▼
NIC TX ring
```

此时 NIC 尚未必立刻看到新 descriptor，通常还需要 doorbell。

---

# 14. TX doorbell：通知 NIC

当发送 descriptor 写入 TX ring 后，Onload 需要通知 NIC：

```text
“TX ring 中有新的 packet，可以开始处理了。”
```

在批量 DMA 路径中，代码调用：

```181:214:/home/yiweichi/onload/src/lib/transport/ip/netif_tx.c
  ef_vi_transmit_push(vi);
  CITP_STATS_NETIF_INC(ni, tx_dma_doorbells);
```

在 direct transmit 路径中，`ef_vi_transmitv()` 可能已经完成了必要的 push/doorbell 处理；代码随后统计：

```c
CITP_STATS_NETIF_INC(netif, tx_dma_doorbells);
```

这里的 doorbell 本质上是对网卡寄存器或映射区域的一次写操作，告诉 NIC：

```text
新的 TX descriptors 已经准备好
```

然后 NIC 硬件会：

1. 读取 TX descriptor；
2. 通过 PCIe DMA 读取 packet buffer；
3. 读取 Ethernet/IP/TCP frame；
4. 根据 descriptor 和 NIC 配置处理 checksum/offload；
5. 将 frame 放入 MAC transmit pipeline；
6. 从物理端口发出。

---

# 15. TX ring 满时怎么办

如果 NIC 的 TX ring 没有足够空间，`ef_vi_transmitv()` 不会成功。

这时 packet 会进入 Onload 的 DMA overflow queue：

```315:329:/home/yiweichi/onload/src/lib/transport/ip/netif_tx.c
  /* drop to here if any of the above methods to send directly failed
   * - put it on the DMA queue instead
   */
  LOG_NT(log("%s: ENQ id=%d", __FUNCTION__, OO_PKT_FMT(pkt)));
  __ci_netif_dmaq_put(netif, dmaq, pkt);
}
```

路径变成：

```text
TCP send queue
    │
    ▼
__ci_netif_send()
    │
    ├─ TX ring 有空间：直接提交给 NIC
    │
    └─ TX ring 满：进入 dmaq
```

之后通过：

```c
ci_netif_dmaq_shove1()
ci_netif_dmaq_shove2()
```

把 overflow queue 中的 packet 再搬到硬件 TX ring。

`__ci_netif_dmaq_shove()` 的核心代码是：

```126:181:/home/yiweichi/onload/src/lib/transport/ip/netif_tx.c
static void __ci_netif_dmaq_shove(ci_netif* ni, int intf_i, int is_fresh)
{
  oo_pktq* dmaq = &ni->state->nic[intf_i].dmaq;
  ef_vi* vi = ci_netif_vi(ni, intf_i);
  ci_ip_pkt_fmt* pkt = PKT_CHK(ni, dmaq->head);
  int rc;

  do {
    pkt = PKT_CHK(ni, dmaq->head);
    ci_assert(pkt->flags & CI_PKT_FLAG_TX_PENDING);
    ci_assert_equal(intf_i, pkt->intf_i);
    {
      ef_iovec iov[CI_IP_PKT_SEGMENTS_MAX];
      int iov_len;

      calc_csum_if_needed(ni, vi, pkt);
      iov_len = ci_netif_pkt_to_iovec(ni, pkt, iov,
                                      sizeof(iov) / sizeof(iov[0]));

      rc = ef_vi_transmitv_init(vi, iov, iov_len, OO_PKT_ID(pkt));

      if( rc >= 0 ) {
        __oo_pktq_next(ni, dmaq, pkt, netif.tx.dmaq_next);
      }
      else {
        break;
      }
    }
  }
  while( oo_pktq_not_empty(dmaq) );

  ef_vi_transmit_push(vi);
}
```

这里的流程是：

```text
dmaq head
   │
   ▼
转换为 DMA iovec
   │
   ▼
ef_vi_transmitv_init()
   │
   ▼
成功则从 dmaq 移除
   │
   ▼
ef_vi_transmit_push()
```

`ef_vi_transmitv_init()` 只初始化 descriptor，最后统一 `transmit_push()`，这样可以批量提交多个 packet，减少 doorbell 次数。

---

# 16. checksum 在哪里处理

在发送到 NIC 前，Onload 需要保证 checksum 正确。

在 `netif_tx.c` 中：

```20:28:/home/yiweichi/onload/src/lib/transport/ip/netif_tx.c
static inline void calc_csum_if_needed(ci_netif* ni, ef_vi* vi,
                                       ci_ip_pkt_fmt* pkt)
{
  /* Calculate packet checksum in case of AF_XDP */
  if( CI_UNLIKELY(vi->nic_type.arch == EF_VI_ARCH_AF_XDP) ) {
    ...
    if( protocol == IPPROTO_TCP || protocol == IPPROTO_UDP )
      oo_pkt_calc_checksums(ni, pkt, my_iov);
  }
}
```

对于 AF_XDP，会显式计算 TCP/UDP checksum。

对于支持 NIC checksum offload 的路径，部分 checksum 可能由网卡完成。具体是否由软件或硬件处理，取决于：

- NIC 类型；
- packet metadata；
- offload 配置；
- EF_VI 发送路径；
- AF_XDP 或普通 EF_VI。

---

# 17. packet 发送完成后如何回收

packet 提交给 NIC 后，Onload 不能马上释放 packet buffer，因为 NIC 可能还在 DMA 读取它。

发送完成后，NIC 会产生 TX completion event，Onload 在 poll 过程中读取 event queue：

```text
NIC TX complete
      │
      ▼
EF_VI event queue
      │
      ▼
Onload poll
      │
      ▼
识别 packet ID
      │
      ▼
释放/回收 ci_ip_pkt_fmt
      │
      ▼
归还 packet buffer pool
```

packet 在提交时携带：

```c
OO_PKT_ID(pkt)
```

这个 ID 让完成处理路径知道哪个 packet 已经不再被 NIC 使用，可以回收。

如果 packet 还没有完成，Onload 必须保留它，因为 TCP 还可能需要：

- 等待 ACK；
- 重传；
- 记录发送时间；
- 处理拥塞控制；
- 根据 sequence range 确认发送状态。

---

# 18. 从 `send()` 返回到应用的时间点

一次 `send()` 的成功返回大致表示：

```text
应用数据已经被 Onload 接受
并且进入了 Onload 的 TCP 发送处理状态
```

它不严格表示：

```text
NIC 已经发出 frame
```

更不表示：

```text
对端应用已经 read() 到数据
```

可能存在以下情况：

## 情况 A：已经直接提交给 NIC

```text
send()
  -> packet constructed
  -> ef_vi_transmitv()
  -> NIC TX ring
  -> return
```

## 情况 B：仍在 TCP send queue

```text
send()
  -> packet constructed
  -> TCP send queue
  -> 因窗口/Nagle/cork 暂不发送
  -> return 或阻塞等待
```

## 情况 C：进入 DMA overflow queue

```text
send()
  -> packet constructed
  -> __ci_netif_send()
  -> TX ring 满
  -> dmaq
  -> 后续再提交
```

## 情况 D：socket 未加速

```text
send()
  -> Onload 拦截函数
  -> fd 不在 Onload fd table
  -> ci_sys_send()
  -> Linux kernel TCP stack
```

---

# 19. 以一个具体 5000 字节 send 为例

假设：

```c
send(fd, buf, 5000, 0);
```

当前条件：

```text
IPv4
MSS = 1460
TCP connection 已建立
TX ring 有空间
没有 MSG_MORE
```

大致过程如下：

```text
1. 应用调用 send(fd, buf, 5000, 0)

2. 进入 Onload 的 onload_send()

3. fd table 找到 fd 对应的 Onload TCP socket

4. 调用 citp_tcp_send()

5. 调用 ci_tcp_sendmsg()

6. 计算 total_unsent = 5000

7. 分配 4 个 packet buffer

8. 分段并复制：
   packet 1: 1460 bytes
   packet 2: 1460 bytes
   packet 3: 1460 bytes
   packet 4: 620 bytes

9. 为每个 packet 填充：
   Ethernet header
   IPv4 header
   TCP header
   sequence number
   ACK number
   TCP options

10. packet 放入 TCP send queue

11. 检查 TCP send window 和 congestion window

12. 可发送的 packet 进入 TX path

13. __ci_netif_send() 选择：
   PIO / CTPIO / DMA

14. 若走 DMA：
   packet buffer 转成 DMA iovec

15. 调用 ef_vi_transmitv()

16. descriptor 写入 NIC TX ring

17. doorbell 通知 NIC

18. NIC DMA 读取 packet buffer

19. NIC 发出 4 个 Ethernet frame

20. 后续收到 TX completion event

21. Onload poll 时回收 packet buffer
```

实际中可能不是一次 `send()` 就立即产生 4 个 wire packet，因为 TCP 还会受到：

- `snd_wnd`
- `cwnd`
- Nagle
- `MSG_MORE`
- TCP cork
- TSO
- retransmission queue
- NIC offload

等因素影响。

---

# 20. 这条路径中最值得跟踪的函数

如果你想在源码中设置断点或加日志，建议按下面顺序跟：

```text
onload_send
  src/lib/transport/unix/sockcall_intercept.c

citp_tcp_send
  src/lib/transport/unix/tcp_fd.c

ci_tcp_sendmsg
  src/lib/transport/ip/tcp_send.c

ci_tcp_send_alloc_pkts
  src/lib/transport/ip/tcp_send.c 或相关头文件/生成代码

ci_tcp_send_fill_pkts
  src/lib/transport/ip/tcp_send.c

ci_tcp_sendmsg_fill_pkt
  src/lib/transport/ip/tcp_send.c

ci_tcp_sendmsg_prep_pkt
  src/lib/transport/ip/tcp_send.c

ci_tcp_sendmsg_enqueue
  src/lib/transport/ip/tcp_send.c

ci_tcp_tx_advance
  TCP transmit advance 实现

__ci_netif_send
  src/lib/transport/ip/netif_tx.c

ef_vi_transmitv
  EF_VI 用户态发送接口

ef_vi_transmit_push
  EF_VI doorbell/提交接口
```

特别是这三处最适合作为分界点：

```text
应用 API -> Onload：
    sockcall_intercept.c: onload_send()

socket TCP 逻辑 -> packet：
    tcp_send.c: ci_tcp_sendmsg()

Onload packet -> NIC：
    netif_tx.c: __ci_netif_send()
```

一句话总结：

> Onload 在 `send()` 被调用后，先通过用户态拦截拿到应用 buffer，再由 TCP 协议栈按照 MSS、窗口和拥塞控制切成 packet，复制到 Onload 管理的 DMA buffer，填充完整的 Ethernet/IP/TCP header，放入 TCP 发送队列，最后通过 EF_VI 将 DMA 地址写入 NIC 的 TX ring，并通过 doorbell 通知网卡；网卡随后 DMA 读取这些 buffer，完成真正的线速发送。