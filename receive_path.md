下面沿着一次普通的：

```c
recv(fd, buf, len, 0);
```

讲完整的 TCP receive 流程。先给结论：

```text
NIC 收到 Ethernet frame
    │
    ▼
NIC DMA 到 Onload RX buffer
    │
    ▼
Onload poll 读取 RX event
    │
    ▼
解析 Ethernet/IP/TCP
    │
    ▼
按 TCP sequence 放入 recv1/recv2
    │
    ▼
唤醒等待 recv() 的线程
    │
    ▼
recv() 从 receive queue 拷贝到应用 buf
    │
    ▼
更新 ACK/window，回收 packet
```

和 send 路径类似，Onload receive 也主要在用户态完成，不是先把数据交给 Linux socket，再由 Onload 读取。

---

# 1. 应用调用 `recv()`

Onload 首先拦截应用的 `recv()`。

```749:789:/home/yiweichi/onload/src/lib/transport/unix/sockcall_intercept.c
OO_INTERCEPT(ssize_t, recv,
             (int fd, void* buf, size_t len, int flags))
{
  citp_fdinfo* fdi;
  struct msghdr m;
  struct iovec iov[1];
  int rc;
  citp_lib_context_t lib_context;

  if( CI_UNLIKELY(citp.init_level < CITP_INIT_ALL) ) {
    citp_do_init(CITP_INIT_SYSCALLS);
    return ci_sys_recv(fd, buf, len, flags);
  }

  Log_CALL(ci_log("%s(%d, %p, %u, 0x%x)", __FUNCTION__, fd, buf,
                  (unsigned)len, flags));

  if( (fdi = citp_fdtable_lookup_fast(&lib_context, fd)) ) {
    iov[0].iov_base = buf;
    iov[0].iov_len = len;
    m.msg_iov = iov;
    m.msg_iovlen = 1;
    rc = citp_fdinfo_get_ops(fdi)->recv(fdi, &m, flags);
```

这个函数做的事情和 `send()` 的拦截类似：

1. 检查 Onload 是否已经初始化；
2. 根据 fd 查找 `citp_fdinfo`；
3. 把简单的 `buf + len` 包装成一个 `iovec`；
4. 把 `recv()` 转换成内部统一的 `recvmsg` 形式；
5. 通过函数指针表调用该 fd 类型对应的 `.recv`。

如果 fd 不是 Onload fd，则回退到 Linux：

```c
  else {
    rc = ci_sys_recv(fd, buf, len, flags);
  }
```

因此路径分成：

```text
Onload TCP fd:
    recv()
      -> onload_recv()
      -> citp_fdinfo_get_ops(fdi)->recv()
      -> citp_tcp_recv()

普通 Linux fd:
    recv()
      -> ci_sys_recv()
```

---

# 2. 通过函数表进入 `citp_tcp_recv()`

TCP 协议实现表中有：

```2310:2315:/home/yiweichi/onload/src/lib/transport/unix/tcp_fd.c
    .getsockopt         = citp_tcp_getsockopt,
    .setsockopt         = citp_tcp_setsockopt,
    .recv               = citp_tcp_recv,
    .recvmmsg           = citp_tcp_recvmmsg,
    .send               = citp_tcp_send,
    .sendmmsg           = citp_tcp_sendmmsg,
```

所以：

```c
citp_fdinfo_get_ops(fdi)->recv(fdi, &m, flags)
```

对于 TCP fd 实际展开为：

```c
citp_tcp_recv(fdi, &m, flags)
```

具体实现：

```1685:1723:/home/yiweichi/onload/src/lib/transport/unix/tcp_fd.c
static int citp_tcp_recv(citp_fdinfo* fdinfo, struct msghdr* msg, int flags)
{
  citp_sock_fdi* epi = fdi_to_sock_fdi(fdinfo);
  ci_tcp_recvmsg_args a;
  int rc;

  if (epi->sock.s->b.sb_aflags & (CI_SB_AFLAG_O_NONBLOCK|CI_SB_AFLAG_O_NDELAY))
    flags |= MSG_DONTWAIT;

  if( (flags & (MSG_WAITALL | ONLOAD_MSG_ONEPKT)) ==
      (MSG_WAITALL | ONLOAD_MSG_ONEPKT) ) {
    Log_E(ci_log("WAITALL and ONEPKT is not a valid flag combination"));
    errno = EINVAL;
    return -1;
  };

  if( epi->sock.s->b.state != CI_TCP_LISTEN ) {
    if( (msg->msg_iovlen == 0 || msg->msg_iov == NULL) &&
        ! (flags & MSG_ERRQUEUE) ) {
      msg->msg_flags = 0;
      msg->msg_controllen = 0;
      return 0;
    }
    ci_tcp_recvmsg_args_init(&a, epi->sock.netif, SOCK_TO_TCP(epi->sock.s),
                             msg, flags);
    rc = ci_tcp_recvmsg(&a);
    return rc;
  }

  CI_SET_ERROR(rc, SOCK_RX_ERRNO(epi->sock.s));
  return rc;
}
```

这一层主要做四件事。

## 2.1 处理 non-blocking

如果 socket 设置了：

```c
O_NONBLOCK
```

就加入：

```c
MSG_DONTWAIT
```

这样后面如果没有可读数据，不能睡眠等待，而是返回：

```text
-EAGAIN / EWOULDBLOCK
```

## 2.2 检查非法 flags

这里禁止：

```text
MSG_WAITALL + ONLOAD_MSG_ONEPKT
```

因为两者语义冲突：

- `MSG_WAITALL`：尽量等待足够多的数据；
- `ONLOAD_MSG_ONEPKT`：只返回一个 Ethernet frame 中的数据。

## 2.3 检查监听 socket

如果是 `CI_TCP_LISTEN`，说明这是监听 socket，不是已建立连接的 socket。

监听 socket 没有普通 TCP data receive queue。它的 receive 语义是 `accept()`，不是 `recv()`。

## 2.4 构造 receive 参数，进入 TCP receive 层

```c
ci_tcp_recvmsg_args_init(...)
ci_tcp_recvmsg(&a)
```

此时会把：

```text
fdinfo
  -> Onload socket
  -> ci_netif
  -> ci_tcp_state
  -> 应用 iovec
```

整理成 TCP receive 层需要的参数。

---

# 3. 进入 `ci_tcp_recvmsg_impl()`

`ci_tcp_recvmsg()` 最终调用：

```951:958:/home/yiweichi/onload/src/lib/transport/ip/tcp_recv.c
int ci_tcp_recvmsg(const ci_tcp_recvmsg_args* a
                   CI_KERNEL_ARG(ci_addr_spc_t addr_spc))
{
  int rc = ci_tcp_recvmsg_impl(a, copy_one_pkt, NULL CI_KERNEL_ARG(addr_spc));
  if( rc < 0 )
    CI_SET_ERROR(rc, -rc);
  return rc;
}
```

核心是：

```c
ci_tcp_recvmsg_impl()
```

开头先准备本次 receive 的状态：

```587:628:/home/yiweichi/onload/src/lib/transport/ip/tcp_recv.c
static inline int ci_tcp_recvmsg_impl(const ci_tcp_recvmsg_args* a,
                                      pkt_copy_t copier,
                                      struct onload_zc_recv_args* zc_args
                                      CI_KERNEL_ARG(ci_addr_spc_t addr_spc))
{
  int have_polled;
  ci_uint64 sleep_seq;
  ci_tcp_state*         ts = a->ts;
  ci_netif*              ni = a->ni;
  int                   flags = a->flags;
  int                   timeout = ts->s.so.rcvtimeo_msec;
  struct tcp_recv_info  rinf;

  ci_assert(a);
  ci_assert(ni);
  ci_assert(ts);
  ci_assert(a->msg);

  rinf.stack_locked = 0;
  rinf.a = a;
  rinf.rc = 0;
  rinf.msg_flags = 0;
  rinf.copier = copier;
  rinf.zc_args = zc_args;
```

这里：

```text
ni
    当前 Onload stack

ts
    当前 TCP socket 的协议状态

rinf
    这次 recv 的临时上下文

rinf.piov
    指向应用提供的接收 buffer

rinf.rc
    已经拷贝给应用的字节数
```

---

# 4. 先拿 per-socket lock

接下来先锁 socket：

```629:638:/home/yiweichi/onload/src/lib/transport/ip/tcp_recv.c
  /* Grab the per-socket lock so we can access the receive queue. */
  rinf.rc = ci_sock_lock(ni, &ts->s.b);
  if(CI_UNLIKELY( rinf.rc != 0 ))
    return rinf.rc;

  if( ts->s.b.state == CI_TCP_LISTEN )  goto check_errno;
```

这里拿的是：

```c
ci_sock_lock(ni, &ts->s.b)
```

它是 **per-socket lock**，作用是保护当前 TCP socket 的状态和 receive queue。

前面 send 路径中见到的：

```c
ci_netif_trylock(ni)
```

是 **per-stack lock**。

两者不同：

```text
per-stack lock:
    保护整个 Onload stack

per-socket lock:
    保护一个具体 TCP socket
```

receive 路径通常需要：

```text
先拿 socket lock
    │
    ▼
访问当前 socket 的 recv queue
    │
    ├─ 必要时再拿 stack lock 去 poll NIC
    └─ 处理完后释放
```

这样可以允许一个线程访问某个 socket 的 receive state，同时避免多个线程同时修改同一个 socket。

---

# 5. `recv()` 先看已有的 receive queue

初始化 iovec 后：

```641:655:/home/yiweichi/onload/src/lib/transport/ip/tcp_recv.c
  ci_tcp_recvmsg_init_piov(&rinf);

  LOG_TR(log(LNTS_FMT "recvmsg len=%d flags=%x bytes_in_rxq=%d",
	     LNTS_PRI_ARGS(ni, ts),
             ci_iovec_ptr_bytes_count(&rinf.piov), flags, tcp_rcv_usr(ts)));

 poll_recv_queue:
  rinf.rc += ci_tcp_recvmsg_get_inline(&rinf);
```

这里首先不是立即访问 NIC，而是先检查：

```text
当前 socket 的 receive queue 里是否已经有数据
```

这是非常重要的 fast path：

```text
packet 之前已经由 ci_netif_poll() 收到
    │
    ▼
已经放进 ts->recv1 / ts->recv2
    │
    ▼
这次 recv() 直接从队列拷贝到应用 buffer
```

如果 receive queue 中已经有数据，就不必再次 poll NIC。

---

# 6. `recv1` 和 `recv2` 是什么

这个版本的 Onload TCP receive path 使用了两个逻辑 receive queue：

```text
recv1
recv2
```

从代码可见：

```353:361:/home/yiweichi/onload/src/lib/transport/ip/tcp_recv.c
  ci_assert(OO_PP_NOT_NULL(ts->recv1.head));

  pkt = PKT_CHK_NNL(netif, ts->recv1_extract);
  if( oo_offbuf_is_empty(&pkt->buf) ) {
    if( OO_PP_IS_NULL(pkt->next) )  return total;  /* recv1 is empty. */
    ci_wmb();
    ts->recv1_extract = pkt->next;
    pkt = PKT_CHK_NNL(netif, ts->recv1_extract);
    ci_assert(oo_offbuf_not_empty(&pkt->buf));
  }
```

## 6.1 `recv1`

`recv1` 是当前可以直接交付给用户的连续、有序数据队列。

```text
recv1:
    已经按 TCP sequence 排好序
    可以直接复制给应用
```

## 6.2 `recv2`

`recv2` 用于暂存另一类尚未能直接交付的数据，通常和：

- out-of-order packet；
- urgent data；
- receive mark；
- sequence gap；

等情况有关。

代码中：

```1274:1285:/home/yiweichi/onload/src/lib/transport/ip/tcp_recv.c
static int ci_tcp_recvmsg_recv2(struct tcp_recv_info *rinf)
{
  ci_tcp_state* ts = rinf->a->ts;
  ci_netif* ni = rinf->a->ni;
  ci_ip_pkt_queue* recv2 = &ts->recv2;
  ci_ip_pkt_fmt* pkt, *head_pkt, *tail_pkt;
  unsigned rd_nxt_seq, n;
```

一般情况下可以先这样理解：

```text
recv1:
    当前已经连续、可读的数据

recv2:
    需要进一步整理后才能交付的数据
```

它们本质上仍然是 packet queue，队列元素是 `ci_ip_pkt_fmt`。

---

# 7. 从 packet queue 拷贝到应用 buffer

真正从 Onload packet 拷贝到应用 buffer 的代码在：

```322:353:/home/yiweichi/onload/src/lib/transport/ip/tcp_recv.c
static inline int
ci_tcp_recvmsg_get_impl(struct tcp_recv_info *rinf)
{
  ci_netif* netif = rinf->a->ni;
  ci_tcp_state* ts = rinf->a->ts;
  int n, peek_off, total;
  ci_ip_pkt_fmt* pkt;

  ci_assert(ci_sock_is_locked(netif, &ts->s.b));

  pkt = PKT_CHK_NNL(netif, ts->recv1_extract);
  if( oo_offbuf_is_empty(&pkt->buf) ) {
    if( OO_PP_IS_NULL(pkt->next) )  return total;
    ci_wmb();
    ts->recv1_extract = pkt->next;
    pkt = PKT_CHK_NNL(netif, ts->recv1_extract);
  }
```

随后会读取 packet 的 payload，使用 `rinf->copier` 拷贝到应用 iovec。

普通 `recv()` 使用：

```c
copy_one_pkt
```

零拷贝 receive 则会使用不同的 callback：

```c
zc_call_callback
```

普通复制路径的概念是：

```text
Onload RX packet buffer
    │
    ├── Ethernet header
    ├── IP header
    ├── TCP header
    └── payload
             │
             │ copy_one_pkt()
             ▼
应用 buf
```

对于一个 packet：

```text
packet payload = 1460 bytes
应用 recv buffer = 4096 bytes
```

可能只复制 1460 字节，然后：

- 如果没有更多数据，返回 1460；
- 如果还有下一个 packet，继续复制；
- 如果满足 `MSG_WAITALL`，可能继续等待；
- 如果有 `ONLOAD_MSG_ONEPKT`，只处理一个 Ethernet frame。

---

# 8. 普通 receive 和 `MSG_PEEK`

非 `MSG_PEEK` 情况下，数据被复制后通常会推进 receive queue 的 extract pointer，并更新已交付字节数：

```403:449:/home/yiweichi/onload/src/lib/transport/ip/tcp_recv.c
    if(CI_LIKELY( ! (rinf->a->flags & (MSG_PEEK | ONLOAD_MSG_ONEPKT) ))) {
      if( ci_tcp_recvmsg_get_nopeek(peek_off, ts, netif, &pkt, total, n,
                                    max_bytes) != 0 )
        break;
    }
    else {
      if( rinf->a->flags & MSG_PEEK ) {
```

`MSG_PEEK` 时：

```text
复制数据给应用
但不真正消费 receive queue
```

所以下一次 `recv()` 仍然可以看到这些数据。

普通 receive 则会：

```text
复制
  -> 推进 extract
  -> 增加 rcv_delivered
  -> 可能释放 packet
  -> 触发 receive window 更新
```

---

# 9. `recv()` 发现数据不足时，会 poll NIC

如果已有 receive queue 不能填满应用 buffer，Onload 会检查是否需要更新 receive queue：

```653:690:/home/yiweichi/onload/src/lib/transport/ip/tcp_recv.c
 poll_recv_queue:
  rinf.rc += ci_tcp_recvmsg_get_inline(&rinf);

  if( ci_iovec_ptr_is_empty_proper(&rinf.piov) &&
      ( rinf.rc != 0 || TCP_RX_DONE(ts) || tcp_rcv_usr(ts) ) ) {
    if( CI_UNLIKELY(rinf.rc == 0) )  goto check_errno;
    goto success_unlock_out;
  }

  if( (rinf.a->flags & ONLOAD_MSG_ONEPKT) && (rinf.rc > 0) )
    goto success_unlock_out;

  if( ! have_polled ) {
    have_polled = 1;

    if( ci_netif_may_poll(ni) &&
        ci_netif_need_poll_spinning(ni, start_frc) ) {
      if( ci_netif_trylock(ni) ) {
        ci_uint32 rcv_added_before = ts->rcv_added;
        int any_evs = ci_netif_poll(ni);
```

这里是 receive path 的关键：

```c
if( ci_netif_trylock(ni) )
    ci_netif_poll(ni);
```

意思是：

1. 当前 receive queue 数据不够；
2. 尝试获取整个 stack 的 lock；
3. 如果拿到，poll NIC event queue；
4. 处理新收到的 packet；
5. 新 packet 进入对应 TCP receive queue；
6. 回到 `poll_recv_queue`；
7. 再次从 receive queue 拷贝给应用。

完整逻辑：

```text
recv()
  │
  ▼
查看 recv1
  │
  ├─ 有数据：复制到应用
  │
  └─ 数据不够：
       │
       ▼
   获取 stack lock
       │
       ▼
   ci_netif_poll()
       │
       ├─ 读取 NIC RX event
       ├─ 得到 packet ID
       ├─ 找到 RX packet buffer
       ├─ 解析 Ethernet/IP/TCP
       ├─ TCP sequence 检查
       ├─ 放入 recv1/recv2
       └─ 更新 ACK/window/socket readiness
       │
       ▼
   再次读取 recv queue
```

---

# 10. `ci_netif_poll()` 接收的是什么

`ci_netif_poll()` 主要轮询当前 Onload stack 关联的 NIC event queue。

它可能同时看到：

```text
RX event
TX completion event
错误事件
其他 NIC event
```

对于 RX event，逻辑上大致是：

```text
NIC 已收到 frame
    │
    ▼
NIC DMA 将 frame 写入 Onload RX buffer
    │
    ▼
RX event queue 出现 event
    │
    ▼
ci_netif_poll()
    │
    ▼
取出 packet ID / buffer
    │
    ▼
解析 Ethernet header
    │
    ▼
解析 IP header
    │
    ▼
解析 TCP header
    │
    ▼
匹配 socket/filter
    │
    ▼
按 sequence 放入 TCP receive queue
```

和普通 Linux 路径不同，packet 一开始就在 Onload 管理的 packet buffer 中，不需要先构造成 Linux `sk_buff` 再复制到用户态。

---

# 11. TCP 如何处理收到的 packet

一个收到的 TCP packet 不会无条件直接放进用户 receive queue。

TCP 层需要检查：

- 目标 IP；
- 源 IP；
- 源端口；
- 目的端口；
- TCP checksum；
- sequence number；
- ACK number；
- receive window；
- SYN/FIN/RST；
- SACK；
- timestamp；
- urgent data；
- 是否重复包；
- 是否乱序包。

例如连续收到：

```text
packet A: seq=1000, len=1000
packet B: seq=2000, len=1000
```

如果 TCP 当前期待：

```text
rcv_nxt = 1000
```

那么 A 和 B 可以连续放入可读路径。

但如果先收到：

```text
packet B: seq=2000, len=1000
```

而当前期待：

```text
rcv_nxt = 1000
```

中间缺少：

```text
seq=1000..1999
```

那么 packet B 不能立刻交给应用，通常需要放到乱序相关队列，也就是你在代码中看到的 `recv2` 体系中，等缺失的 packet 到达后再整理。

---

# 12. receive queue 中 packet 的生命周期

以一个 RX packet 为例：

```text
NIC RX buffer
    │
    ▼
RX event
    │
    ▼
ci_netif_poll()
    │
    ▼
TCP protocol processing
    │
    ├─ 丢弃：checksum 错误、没有匹配 socket、重复等
    │
    └─ 接收：
          │
          ▼
       recv1 / recv2
          │
          ▼
       recv()
          │
          ▼
       copy_one_pkt()
          │
          ▼
       应用 buffer
          │
          ▼
       packet consumed
          │
          ▼
       packet buffer 回收到 RX pool
```

如果启用了 `MSG_PEEK`：

```text
packet 暂时不会被真正消费
```

如果使用 Onload zero-copy receive：

```text
packet buffer 可以直接交给应用 callback
```

不会立刻把 payload 拷贝到应用普通 buffer，但应用必须按 Onload API 的规则释放或确认 packet。

---

# 13. receive 后为什么可能发送 ACK 或 window update

receive path 不仅仅是“从队列拷贝数据”。

当应用消费 receive queue 后，TCP 的接收窗口可能扩大。代码中：

```100:115:/home/yiweichi/onload/src/lib/transport/ip/tcp_recv.c
/* This is called after we've pulled a certain amount of data from the
** receive queue, and sends a window update if appropriate.
*/
static void ci_tcp_recvmsg_send_wnd_update(ci_netif* ni, ci_tcp_state* ts)
{
  if( ! ci_netif_trylock(ni) ) {
    ci_bit_set(&ts->s.s_aflags, CI_SOCK_AFLAG_NEED_ACK_BIT);
    if( ! ci_netif_lock_or_defer_work(ni, &ts->s.b) )
      return;
```

当应用读取了一些数据：

```text
应用消费 recv queue
    │
    ▼
可用 receive buffer 增加
    │
    ▼
TCP advertised window 可以扩大
    │
    ▼
发送 ACK/window update
```

如果当前拿不到 stack lock，代码不会强行阻塞，而是设置延迟处理标记：

```c
CI_SOCK_AFLAG_NEED_ACK_BIT
```

之后再由持锁的进程或后续 stack progress 处理。

---

# 14. receive path 的阻塞行为

如果没有数据：

```c
recv(fd, buf, len, 0)
```

且不是 non-blocking，receive path 通常会：

```text
1. 检查已有 receive queue
2. poll 一次或多次 NIC
3. 仍然没有数据
4. 检查 TCP 状态/错误
5. 睡眠等待 socket 可读
6. 收到 RX event 或 FIN/RST 后被唤醒
7. 再次进入 receive queue 处理
```

如果是：

```c
recv(fd, buf, len, MSG_DONTWAIT)
```

没有数据就会返回：

```text
-EAGAIN
```

如果是 `MSG_WAITALL`，则可能继续等待，直到：

- buffer 填满；
- 对端关闭；
- 发生错误；
- 超时；
- 收到信号。

代码中有对应逻辑：

```718:747:/home/yiweichi/onload/src/lib/transport/ip/tcp_recv.c
  /* We've done at least one ci_netif_poll(), so we're up-to-date.  But we
  ** haven't filled the app's buffer.
  */

  if( rinf.rc && FLAGS_AND_LOWAT_PERMIT_FAST_RET_WITH_DATA(ts, rinf.rc, flags) )
    goto success_unlock_out;

  if( TCP_RX_DONE(ts) )  goto rx_done;

  if( rinf.rc == 0 && (flags & MSG_DONTWAIT) ) {
    rinf.rc = -EAGAIN;
    goto unlock_out;
  }
```

---

# 15. 一次完整的 TCP receive 示例

假设对端发送：

```text
"hello"
```

流程如下。

## 第一步：远端 NIC 发 frame

远端发出一个 Ethernet frame：

```text
Ethernet
  IPv4
    TCP
      payload = "hello"
```

## 第二步：本地 NIC 接收

本地 NIC：

1. 根据硬件 filter 接收 frame；
2. 通过 DMA 将 frame 写入 Onload RX buffer；
3. 在 EF_VI event queue 中生成 RX event。

## 第三步：Onload poll

某个执行 Onload progress 的线程调用：

```c
ci_netif_poll(ni);
```

它处理 RX event，并得到对应 packet。

## 第四步：TCP 解析

Onload 检查：

```text
Ethernet type
IP protocol = TCP
四元组
checksum
sequence number
window
TCP flags
```

然后找到对应的 `ci_tcp_state`。

## 第五步：放入 receive queue

如果 packet sequence 正好等于：

```text
tcp_rcv_nxt(ts)
```

则进入有序接收路径：

```text
ts->recv1
```

如果是乱序 packet，可能进入：

```text
ts->recv2
```

## 第六步：应用调用 `recv()`

```c
recv(fd, buf, sizeof(buf), 0);
```

路径：

```text
onload_recv()
  -> citp_tcp_recv()
  -> ci_tcp_recvmsg()
  -> ci_tcp_recvmsg_get_inline()
```

## 第七步：拷贝 payload

Onload 把 packet 中 TCP payload 的 `"hello"` 拷贝到：

```c
buf
```

返回：

```text
5
```

## 第八步：更新 TCP receive state

Onload 更新：

```text
rcv_delivered
recv1_extract
receive queue occupancy
advertised window
```

必要时发送 ACK/window update。

## 第九步：回收 packet

packet 不再需要后回收到 Onload packet pool，供以后 RX/TX 使用。

---

# 16. 发送路径和接收路径的对照

```text
发送：
应用 send()
    -> 拷贝到 Onload TX packet
    -> TCP send queue
    -> ci_tcp_tx_advance()
    -> NIC TX ring
    -> NIC 发出

接收：
NIC 收到 frame
    -> DMA 到 Onload RX packet
    -> ci_netif_poll()
    -> TCP receive queue
    -> 应用 recv()
    -> 拷贝到用户 buffer
```

更详细地对照：

```text
TX:
ci_tcp_sendmsg()
    -> ci_tcp_send_alloc_pkts()
    -> ci_tcp_send_fill_pkts()
    -> ci_tcp_sendmsg_prep_pkt()
    -> ts->send
    -> ci_tcp_tx_advance()
    -> __ci_netif_send()
    -> ef_vi_transmitv()
    -> NIC TX ring

RX:
ci_netif_poll()
    -> RX event
    -> packet parse
    -> TCP sequence processing
    -> ts->recv1 / ts->recv2
    -> ci_tcp_recvmsg_get_impl()
    -> copy_one_pkt()
    -> application buffer
```

---

# 17. 哪个线程在跑 receive

和 TX 类似，Onload 通常不会为每个 socket 创建专门的 receive 线程。

以下线程都可能推进 receive：

```text
1. 正在执行 recv() 的应用线程
2. 执行 poll()/epoll_wait() 的应用线程
3. 执行 select()/poll 的线程
4. 执行其他 Onload socket 操作、触发 stack poll 的线程
5. Onload timer/progress 相关执行上下文
```

最典型的是：

```text
应用调用 recv()
    -> receive queue 没数据
    -> recv() 内部调用 ci_netif_poll()
    -> 该应用线程直接从 NIC 拉取 RX event
```

所以 Onload 的 receive 不是：

```text
后台线程收到数据
    -> Linux socket queue
    -> recv() 再读取
```

而更接近：

```text
recv()
    -> 直接 poll NIC
    -> 处理 packet
    -> 放入 Onload receive queue
    -> 直接复制给应用
```

---

# 18. 最关键的代码调用链

普通 TCP `recv()` 的核心调用链可以记成：

```text
onload_recv()
  src/lib/transport/unix/sockcall_intercept.c

citp_fdinfo_get_ops(fdi)->recv()
  src/lib/transport/unix/sockcall_intercept.c

citp_tcp_recv()
  src/lib/transport/unix/tcp_fd.c

ci_tcp_recvmsg()
  src/lib/transport/ip/tcp_recv.c

ci_tcp_recvmsg_impl()
  src/lib/transport/ip/tcp_recv.c

ci_tcp_recvmsg_get_inline()
  src/lib/transport/ip/tcp_recv.c

ci_tcp_recvmsg_get_impl()
  src/lib/transport/ip/tcp_recv.c

ci_netif_poll()
  处理 NIC RX/TX event

copy_one_pkt()
  把 Onload packet payload 拷贝到用户 buffer
```

最核心的两个阶段是：

```text
数据已经在 receive queue：
    ci_tcp_recvmsg_get_impl()

数据还没有到 receive queue：
    ci_netif_poll()
```

一句话总结：

> TCP receive 时，Onload 先通过拦截层把 `recv()` 路由到 `citp_tcp_recv()`，再由 `ci_tcp_recvmsg()` 先检查当前 TCP socket 的 `recv1/recv2` 队列；如果数据不够，就获取 stack lock、调用 `ci_netif_poll()` 从 NIC event queue 取 RX packet，解析并按 TCP sequence 放入 receive queue，最后由 `ci_tcp_recvmsg_get_impl()` 将 payload 拷贝到应用 buffer，并在数据被消费后更新 ACK、advertised window 和 packet buffer 状态。