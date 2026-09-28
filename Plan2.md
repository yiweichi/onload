嫌疑人 A：poll 耦合（头号）。 write 路径里 ci_tcp_tx_advance 可能触发 ci_netif_poll()，一次 poll 处理 EVQ 里攒的事件——别的 socket 的 ACK、RX burst、定时器。burst 大小决定耗时，几百 ns 到几十 µs 都有。特征：tail 和别的 socket 的流量活动正相关；EF_SEND_POLL_THRESH=64（每发 64 包 poll 一次）、EF_EVS_PER_POLL=64 这些默认值都是为吞吐不是为 tail。

嫌疑人 B：spin 里拿锁。 偶发窗口紧张 → 进 send_q_full → 自旋 trylock。看 spin_tcp_send、tcp_send_ni_lock_contends 计数是否非零。

嫌疑人 C：多线程锁竞争 / prequeue 委托路径被唤醒延迟。

嫌疑人 D：MMIO/doorbell 抖动。 WC 内存 posted write 偶尔 flush 慢、TX push threshold、IRQ 打断你的核（EF_INT_DRIVEN、中断亲和）。

怎么查（区分性实验，按成本排序）：

# 1. 零成本：抓分布特征
#    你的时间戳日志里，20µs事件是否成簇出现（burst后）还是均匀随机（抢占/IRQ）
# 2. 一次实验区分A和B/C：单socket跑，tail是否消失
onload_stackdump <s>:<sock> dump     # 记录tx_sleep/tx_stuck/spin基线
#    压测后对比：tcp_send_ni_lock_contends, spin_tcp_send, unlock_slow_*
# 3. 针对A的修复实验：减少write路径里的poll负担
EF_SEND_POLL_THRESH=0       # 不在send路径做批量poll，改成更激进poll或相反方向测试
EF_EVS_PER_POLL=16          # 限制单次poll事件数，摊平长尾
#    重测p99——如果显著下降，poll耦合实锤
# 4. perf 抓现场
perf record -e cycles -g -- your_app   # 20µs尖峰时CPU在哪
最优雅的终解：让 write 永远不进入可能慢的路径。

20µs 的尾巴无论修到多小，都不如根本不发生。HFT 订单路径的写法：

// 用非阻塞语义 + 保证socket常可写
int avail; onload_fd_stat(fd, &info);   // sndbuf_pkts_avail
// 监控: 若avail持续接近0, 说明窗口/对端跟不上——那是连接层问题,
// 不该在write syscall里硬扛
保证 sndbuf_pkts_avail > 0 是发送前的检查而不是 write 里的 spin；
EF_TCP_SNDBUF_MODE=2 自动把 sndbuf 跟对端窗口对齐，EF_MAX_TX_PACKETS 给足；
极端场景甚至绕过 TCP（组播/专用协议）——但那是架构选择，不是这里的话题。
总结三个问题的答案：profile 没关 Nagle（你自己关：EF_FORCE_TCP_NODELAY=1 + EF_NONAGLE_INFLIGHT_MAX=-1 + 对端 NODELAY）；EF_POLL_USEC 只影响卡住时的自旋预算、与 20µs 无关；20µs tail 是 µs 级 micro-contention，头号嫌疑是 write 路径里 ci_netif_poll() 的事件 burst（poll 耦合），先做单 socket 对照实验 + EF_EVS_PER_POLL 限制来确认。