先回答你的元问题，再给一套可执行的方案。因为你的两个目标其实指向**两条不完全相同**的学习路径。

## 1. 现在读 Onload 的 send/recv，是不是效率不高？

**对“搞懂 Onload”很高效，对“提升 kernel-bypass 能力”不是最快路径。**

你已经写了 `send_path.md` / `receive_path.md`，这是理解 Onload 的正确姿势。但要注意：`Onload` 是**全功能 TCP 协议栈**，它的复杂度（Nagle、重传、超时、延时 ACK、队列管理、慢路径分配……）恰恰是 kernel-bypass 场景里你要**绕开**的东西。TCP 的尾部延迟大头往往不在“内核旁路”本身，而在协议语义带来的阻塞。

而真正顶级的 kernel-bypass 能力，来自更小更纯的东西：**RX/TX descriptor 双环、EVQ、doorbell、spin-loop、零拷贝、时钟精确测量**。这些在 Onload 里被 TCP 的复杂度淹没了。

所以我的建议是：**Onload 的 TCP 只是你的验证靶场，你的技能增长点应该在更小的参考实现上。**

## 2. 先诊断，别猜：tail latency 是哪里来的

TCP write 的尾部延迟，来源完全不同，**修法也完全不同**。先分清：

### 来源 A：write 真的被阻塞了（窗口/ACK）

表现为：你的 write 没有立刻返回全部字节，而是卡在 `send_q_full -> spin -> ci_sock_sleep`。就是咱们前面讨论的慢路径。

- 触发条件：你的写入速率超过了 ACK 回来的速率。
- 关键问句：**对端是不是也让 onload 加速？对端开启 TCP_NODELAY 了吗？** 如果对端是内核 TCP 且有 delayed ACK(40ms) + Nagle，你的低水位 write 可能被 40ms 级停顿吞掉——这是 HFT tail 的最经典大坑。
- 确认方法：`onload_stackdump lots` 看这个 socket 的 `tx_stop_nagle`、`tcp_send_ni_lock_contends`、`TXSLEEP/TXSTUCK` 计数在 tail 时刻是否爆增。
- 修复：对本端 `TCP_NODELAY=1`、对端同样；调 `EF_TCP_SERVER_LOOPBACK/EF_TCP_CLIENT_LOOPBACK` 让 loopback 也 onload；在发送 socket 上 `SO_SNDBUF` 匹配窗口，避免提前占满 `so_sndbuf_pkts`。

### 来源 B：stack lock 竞争（多线程/多连接共享一个 stack）

- 触发：多个活跃 socket 挤在同一 stack，`eplock` cacheline 弹跳；或者一个 socket 被多线程共享。
- 确认：`tcp_send_ni_lock_contends`、`sock_lock_buzz`、`unlock_slow` 呈常态增长。
- 修复：`EF_STACK_PER_THREAD=1` 或按线程/按连接拆 stack + 绑核 + RSS。

### 来源 C：与锁无关的调度/GC/分页抖动

- 触发：慢路径上的 `pkt` 分配触发 `mmap`/hugepage 分页；线程调度竞争；`poll` 被他连接的 burst 拖累（poll 耦合）。
- 确认：静态阶段跑 ping-pong，比较 p50 和 p99；把 CPU 绑死、关闭超线程邻居、锁 TSC/DVFS，再看 tail 是否收窄。
- 修复：`EF_MAX_RX_PACKETS/EF_MAX_TX_PACKETS` 预分配 + `EF_USE_HUGE_PAGES=2`，把所有 pkt 分配收进启动阶段；`onload_thread_set_spin()` 开当前线程 spin。

### 诊断动作顺序（先做这个）

1. 在应用里用 **TSC/`clock_gettime(CLOCK_MONOTONIC)`** 打时间戳，抓 `p50/p99/p99.99`，**不止平均**。
2. 每次 tail 事件发生时，同时抓 `onload_stackdump lots` 的快照，对比那几秒的计数器。
3. 用二分法逐个开关：`EF_STACK_PER_THREAD`、spin、huge pages、nodelay、sndbuf。每开一个重测，不要一次开一堆。

关键认知：**如果你的 write 都是小包（小于一个 cwnd），它通常根本不该走到 `send_q_full`。** 那 tail 就基本不是 ACK/窗口造成的，而是 B 或 C（锁竞争 / 调度 / 分页 / poll 耦合）。这个判别能帮你把调查范围缩小一半。

## 3. 提升 kernel-bypass 能力：分层练，别直接啃 TCP

给你一条从“会读 Onload”到“自己能造”的阶梯：

**第 1 层 · 裸队列（2–3 天）**
硬读 `src/lib/ciul/`（`event_q.c`、`open.c`）和 `src/tests/ef_vi/` 下的 `efsink.c / efsend.c / eflatency.c / efrss.c`。跑起来，理解：描述符怎么进 RX/TX 环、EVQ 怎么收事件、doorbell 什么时候发、PIO 和缓冲 TX 的区别。**这一层决定了你懂不懂硬件。**

**第 2 层 · 写一个自己的 UDP over ef_vi（1–2 周）**
不要碰 Onload 的 UDP/TCP。自己用 `ef_vi` 写一个 UDP ping-pong：
- TX：构造包 → 填 TXQ → doorbell → 链事件回收。
- RX：poll EVQ → 解析 RXQ。
- 加精确计时（`ef_timer`/TSC），输出 p50/p99。
- 再加：RSS 多队列、双核一 spin 一处理。
做完这个，你对“环、事件、doorbell、spin”的直觉就超过读 10 遍 Onload 了。**kernal bypass 的核心能力其实在这里。**

**第 3 层 · 对照 Onload 验证 & 填空（可长时间并行）**
这时再回头读 Onload 的 `udp_send.c` / `udp_rx.c`（注意：**UDP 比 TCP 简单得多，先 UDP**），你已经能预测每行在干嘛。然后读 `netif_event.c` 的 `ci_netif_poll` 和 `tcp_send.c` 的慢路径——这些是 Onload 的“手艺细节”，不是通用 kernel-bypass 知识。

**第 4 层 · 你的 deliverable**
把你的 `send_path.md / receive_path.md` 扩展成一份“**我自己的最小旁路栈设计**”：

```text
1. ef_vi 描述符环 + EVQ + doorbell       ← 你已具备（第1层）
2. UDP 收发 + 计时                       ← 第2层产出
3. 你版本的 spin vs poll vs interrupt    ← 对照 Onload EF_POLL/spin
4. 针对 tail: 你测出的 p50/p99, 哪个环节在 P99 跳
5. 说明每一项 Onload 用 EF_* 怎么调、为什么
```

这份文档才是你的能力证明，比读完全部源码有用得多。

## 4. 一句话给你实操路线

> 用一周把你的 tail 问题固化成一个“**可复现 ping-pong + 计数器快照**”的测量环境，先用这套环境**确认 tail 来自 A/B/C 哪一类**，对症修；同时用 `src/tests/ef_vi` 做第 1、2 层训练，把 Onload 的 UDP 路径当作你的“参考答案”。

我可以直接帮你往下走任一步，二选一：

- 帮你**搭 tail 诊断的环境**（写一个度量脚本/Ping-Pong 工具，接 `onload_stackdump` 的计数），或者
- 陪你**逐行过 `src/lib/ciul/event_q.c` + `efsink.c`**，把第 1 层的队列机制讲透。

你选哪条？