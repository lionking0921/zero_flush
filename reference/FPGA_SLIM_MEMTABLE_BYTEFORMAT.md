# InlineSkipList（Slim MemTable 后端）字节级数据结构规格 — FPGA 解析用

> 目标：为「FPGA 读取/解析 InlineSkipList 并按其 key 顺序参与排序」提供字节级、含端序标注的
> 数据结构规格。本文只描述**内存中实际存在什么字节、怎么被解释**，不含并发协议细节（读侧按
> frozen/只读列表处理即可）。
>
> 适用范围：`zeroflush/slim_memtable.h`（`SlimMemTableRep`）与 `zeroflush/partition_index.h`
> （`PartitionIndex`）共享的同一个 `InlineSkipList<Comparator>` 后端。
>
> 所有结论均标注源码位置，可交叉核对。

---

## 0. 平台假设（务必先确认）

| 项 | 值 | 依据 |
|---|---|---|
| 指针宽度 | 8 B（64-bit） | x86-64 构建 |
| 字节序 | **小端（LE）** | `port::kLittleEndian`，本仓库按 x86-64/Linux |
| 对齐 | 分配块 **≥ 16 B**（`kAlignUnit = alignof(max_align_t)`） | `memory/arena.h:35` |
| arena 指针值 | 主机**虚拟地址**，非物理地址 | 见 §7 注意 |

> 文中所有「LE」均指：数值的低字节存低地址。若目标平台改大端，指针与 fixed64 需做字节交换，
> varint 与排序语义仍不变（见 §4.3）。

---

## 1. 一句话总览

`InlineSkipList` 的每个节点 = **一段自包含的 arena 内存块**，从低地址到高地址为：

```
┌─────────────────────────────┐
│ level h-1 … level 1 指针     │  ← 8×(h-1) B，在 Node「下方」（负偏移）
├─────────────────────────────┤
│ level 0 指针（next_[0]）      │  ← Node 本体，8 B，链入后常驻
├─────────────────────────────┤
│ key 条目（变长，自描述）        │  ← 整条 MemTable 编码记录，Key() 指向此处
└─────────────────────────────┘
```

- **Node 结构体没有 key/value 指针字段**，也没有持久化的 height 字段（见 §5）。
- key 与「value」都在**同一块**里（value 在 key 之后），跳表不另存值。
- 逻辑「指针」只有每层一个的 next 后继指针；上层指针存在节点地址**下方**。

---

## 2. Node 内存布局（硬件寻址公式）

源码：`memtable/inlineskiplist.h:358-421`（结构体）、`:859-880`（AllocateNode）。

### 2.1 布局图（节点高度 h ≥ 1）

```
  低地址
  +------------------------------+
  │ level (h-1) next 指针        │  addr = B − 8(h−1)     ┐
  ├──────────────────────────────┤                       │ prefix 区
  │  …                          │                       │ = 8×(h−1) B
  ├──────────────────────────────┤                       │
  │ level 1 next 指针            │  addr = B − 8          ┘
  ├──────────────────────────────┤
  │ level 0 next 指针 = next_[0]  │  addr = B  （Node 本体；8 B）
  ├──────────────────────────────┤
  │ key 条目（len B，见 §3）       │  addr = B + 8 = Key()
  +------------------------------+
  高地址
```

分配函数 `AllocateNode`（`inlineskiplist.h:868`）一次性开出：

```
总字节 = 8×(h−1)  +  8  +  len
```

### 2.2 只保留「key 指针 K」时的全部换算公式（FPGA 用）

设已取得某节点条目首地址 `K`（即 `Key()`，`inlineskiplist.h:374` 的 `&next_[1]`）：

| 想取的东西 | 表达式 | 备注 |
|---|---|---|
| level 0 后继节点指针 | `load64(K − 8)` | level 0 指针在节点基址 B |
| level i 后继节点指针（i ≥ 0） | `load64(K − 8 − 8×i)` | `= load64(B − 8×i)` |
| 某后继节点 `N` 的条目首地址 | `N + 8` | 条目跟在 next_[0] 之后 |
| 空链表尾 | 指针值 `0x0` | 见 §5.3 |
| 真节点合法性粗查 | 节点指针 & 0xF == 0 | arena 16 B 对齐 |

> 注意：**next 指针值 = 节点基址 B，不是条目地址 K**。拿到 `N` 后条目在 `N+8`；
> 迭代链表用的是 `N`，比较/解码用的是 `N+8`。

### 2.3 端序

next 指针是**原生 8 B 指针值**，x86-64 下内存为小端。节点基址由 arena 分配保证 **低 4 位为 0**
（16 B 对齐），可与 `0x0` 哨兵区分。

---

## 3. 节点内「条目」编码（MemTable 记录格式）

源码：`db/memtable.cc:1079-1114`（`MemTable::Add` 写缓冲）；分配入口
`zeroflush/slim_memtable.cc:52-57`（`Allocate` → `skip_list_.AllocateKey(len)`，整条记录
落在节点内联区）；反向读见 `zeroflush/slim_memtable.cc:78-89`（Iterator）。

### 3.1 字段布局（内存升地址顺序）

```
偏移（自 K）   字段                   长度                内容
──────────────────────────────────────────────────────────────────────────────
0             varint ik_len           1..5 B              下列 internal key 总长
…             user key 字节            ik_len − 8         原始用户 key
…             8 B footer             固定 8 B             LE64((seq<<8)|type)
…             varint val_len          1..5 B              value 区字节数
…             value 字节              val_len B           见 §3.4
[可选]        checksum                protection_bytes    MemTable 校验尾，不参与排序
```

「internal key」= `user key 字节 ‖ 8B footer`，共 `ik_len` B。比较器（§4）只读这块，
varint `val_len` 之后的字节是**载荷**，跳表不比较。

> 当 `memtable_factory` = slim / partition 索引插入时（`partition_index.h:5`、`:71-84`），
> value 区就是 **[varint 16][SlimLocator 16B]**，实际用户值持久在分区 WAL。

### 3.2 端序与字节序约定

| 字段 | 编码格式 | 内存端序 |
|---|---|---|
| varint 长度 | 7-bit 分组，**最低组在前** | 小端风格（跨平台一致） |
| user key 字节 | 原始字节 | 无数值语义，按字节字典序比较 |
| footer | `fixed64 = (seq<<8) \| type` | **LE，8 B** |
| SlimLocator 内字段 | 原生 struct | 平台小端 |

### 3.3 具体字节示例（小端）

例：user key = `"ab"`（0x61 0x62），seq = 0x11223344556677，type = kTypeValue = 0x01：

- `ik_len = 2 + 8 = 10 = 0x0A` → 单字节 varint `0x0A`
- `packed = (seq<<8)|type = 0x1122334455667701`
- footer 内存 8 B（LE）：`01 77 66 55 44 33 22 11`（`01`=type 在最前，seq 在后 7 B 小端）

```
K → 0A | 61 62 | 01 77 66 55 44 33 22 11 | 10 | <16 B SlimLocator…>
     └varint┘ └ user ┘ └───── footer(LE) ───┘ └varint┘ └── value 载荷 ──┘
```

**关键坑：footer 的最低字节（type）在内存最前**。因此对同 user key 的两条记录做
**整体 memcmp 会得到错误顺序**——必须先按 user 段分界、把 footer 组装成 64-bit 数值再比
（§4）。

### 3.4 value 区（本工程 = SlimLocator）

- 通用格式 `[varint val_len][value bytes]`。
- ZeroFlush：`val_len` 恒为 16；`value` = `SlimLocator` 原始内存（16 B，
  `zeroflush/wal_format.h:52-57`）：
  ```
  struct SlimLocator {      // sizeof == 16，小端，紧密打包
    uint32_t part_id;       // 所在 WAL 分区
    uint32_t gen;           // 代际
    uint64_t wal_offset;    // 分区文件内字节偏移
  };
  ```
- 解析只需按 varint 通用解码即可，排序**不依赖** locator 字段。
- 冻结（已封存）条目如何由 locator 定位到真实 value（分区 WAL 记录帧）的完整流程见 §9、帧格式见 §10。

---

## 4. key 比较 / 排序语义（FPGA 排序器的目标规则）

### 4.1 排序规则

比较对象 = 条目解码出的 internal key；规则（`db/dbformat.h:1180-1199` `CompareKeySeq`）：

```
1) 先比 user key（默认 BytewiseComparator = 字节字典序升序）
2) 若 user key 相同：比 seq，seq 大者排前（降序）；type 字节不参与
```

即跳表 level-0 全序 = **user key 升序，同 user key 按 seq 降序**。

### 4.2 可硬件化的比较流程（不能整体 memcmp）

给定两条条目 `a`、`b`（都自 `varint ik_len` 起始）：

1. 各自解码 `varint ik_len`（≤5 B，规则见 §4.4）。
2. `user_a = a[va..va+ik_len_a−8)`、`user_b = b[vb..vb+ik_len_b−8)`。
   - 变长 `memcmp`（逐个字节比，记录首个不同字节即得结果）。
3. 若 user 段全等：各取末 8 B footer，组装 **LE u64**，`seq = x >> 8`；
   比较：`seq_a > seq_b ⇒ a 排前`，`seq_a < seq_b ⇒ b 排前`，相等视作重复。

> footer 比较的两种等价硬件实现：
> - 组装 `LE64(footer) >> 8` 做一次 64-bit 数值比较（推荐）；
> - 或反向（从高地址向低地址）扫 footer 的 byte[7..1]，首个不同字节定序——**不可**从低地址
>   正向扫（那样会先比 type 再比 seq 的低字节）。

### 4.3 端序：什么能 memcmp、什么不能

| 数据段 | 是否可直接字节比较 | 原因 |
|---|---|---|
| user key | ✅ 是 | 字节字典序，无 endianness |
| 整个 internal key | ❌ 否 | footer 段为 LE 数值，正向字节序与数值序相反 |
| footer / seq | ❌ 直接 memcmp | 需先组 u64 或反向扫 byte[7..1] |
| varint 长度 | 各自解码后比较 | 前缀长度不参与 key 序 |

### 4.4 varint32 解码（RocksDB 编码）

`util/coding.h`：`EncodeVarint32` / `GetVarint32Ptr`。

```
字节 i 的低 7 位 = 数值的第 7i..7i+6 位（最低组在第一个字节，小端风格）
最高位 = 1 表示后续还有字节；最多 5 字节。
ik_len ≤ 127 时（典型 user key ≤ 119 B）就是单字节。
```

### 4.5 查找探针也是同格式

`Seek` 用的 memtable key 由 `EncodeKey` 生成（`slim_memtable.cc:41-51`）：
`[varint ik_len][internal key 字节]`，**与节点条目同构**，只是后面不跟 value。FPGA 比较器对
两侧可统一按「先 varint、再 internal key」处理。

---

## 5. 影响解析正确性的关键语义

### 5.1 节点高度不持久化（不自描述）

链入后 height 仅存在于插入瞬间（借 `next_[0]` 传递，`inlineskiplist.h:361-372`，
`:1032` UnstashHeight）。此后：

- 读侧**不能**从节点内容推断其塔高；
- 只能按「沿某层指针抵达的节点，必有 ≥ 该层」来安全地使用上层指针；
- **不能**靠向上/负偏移扫内存重建塔结构（上层槽位在节点分配块**下方**，属于前一块内存）。

对排序器的影响：**排序只需 level-0 单链**（§6），level-0 指针每个节点都有，天然安全，不涉及
塔高。

### 5.2 Node 地址 ↔ 条目地址是算术关系

Node 结构体唯一字段 `next_[1]`（`inlineskiplist.h:420`）；无 key/value 指针字段。
key 位置 = Node + 8；反向 `Node = key_ptr − 8`（软件里 `Insert` 即如此换算，
`inlineskiplist.h:1030`）。

### 5.3 头节点（哨兵）与空指针

- 列表根：`head_`（`inlineskiplist.h:840`），arena 首部满高哨兵节点，key_size=0，永不参与比较。
- `head_` 的各层 next 初值 `nullptr`；`0x0` 一律视为「无穷大 / 链尾」。
- 当前最大层高 `max_height_`（`:242`，relaxed atomic）只增不减；读侧从 `head` 顶到
  `kMaxHeight_-1`（默认 12）起步都安全。

### 5.4 排序数据本身已按 key 有序

level-0 链即全序序列（§4.1）。FPGA 若要「排序」，通常是对 level-0 顺序流出做归并/校验，
或对多个列表/多个分区归并——单链不需要 FPGA 再排。

---

## 6. FPGA 建议解析流程（level-0 顺序扫描）

给定头节点地址 `H`（host 提供）：

```
N = load64(H)                    // 第一个真实节点（或 0x0 = 空）
while N != 0:
    K  = N + 8                    // 条目首地址
    (ik_len, m)   = VarintDecode(K)            // 1..5 B
    ik_start      = K + m
    user_len      = ik_len − 8
    footer        = LE64(ik_start + user_len)  // 末 8 B
    seq           = footer >> 8;  type = footer & 0xFF
    sort_key      = [ik_start, user_len] ‖ seq   // 供 §4 规则排序
    // 可选：解码 value
    (val_len, n)  = VarintDecode(ik_start + ik_len)
    value         = [ik_start + ik_len + n, val_len]
    N = load64(N)                 // level-0 下一个
```

每步内存访问 = `load64(N)` 一个指针 + 条目载荷（变长）。节点不等长，**不能按固定步长跳过**，
必须跟随 level-0 指针。载荷常跨 64 B cache line，建议按需取宽。

---

## 7. 落地注意（数据平面问题）

1. **arena 多块不连续**：`ConcurrentArena` 按 4 KB 级块分配，指针为**主机虚拟地址**。
   FPGA 直接 DMA 前需 pin 内存 / 共享物理区 / 建 VA→PA 页表。
2. **只读目标**：优先 offload **frozen 列表**（引用计数保活、只读）。活跃列表由 CPU 并发
   CAS + release 更新（`inlineskiplist.h:386-407`），结构可能边读边变。
3. 单条 8 B 指针加载互相依赖（链式追指针），顺序扫描吞吐高、随机 Get 延迟受限——本文面向
   扫描/归并，避开随机点查。
4. checksum（`protection_bytes_per_key`）默认关闭，如开启则在条目末尾，不影响排序字段。

---

## 8. 端序速查表（汇总）

| 内存对象 | 宽度 | 编码 | 内存端序 | 需否转换 |
|---|---|---|---|---|
| next 指针 | 8 B | 原生指针（VA） | LE | 是（仅大端平台） |
| Node 层槽定位 | — | 地址 = B − 8×i | — | 无 |
| varint 长度 | 1..5 B | 7-bit 组 LSB-first | 小端风格 | 按位拼 |
| user key | ik_len−8 | 原始字节 | 无 | 无 |
| internal key footer | 8 B | (seq<<8)\|type | LE | 组 u64 用 |
| SlimLocator | 16 B | struct | LE | 组字段用 |
| 哨兵 | 8 B | 0x0 | — | — |

---

## 9. 冻结条目 value 定位：locator → 段目录 → base+wal_offset → ZfRecord

> 本节回答：「条目已 freeze（封存）后，FPGA 拿到 16B `SlimLocator` 怎样才能读到它的真实 value？」
> 前提结论：**`wal_offset` 是文件内字节偏移，不是全局地址**；真实 value 是分区 WAL 里一条
> `ZfRecord` 的 body，`locator` 只会带你到这条帧的**帧首**，value 还要按 §10 帧格式再解一层。

### 9.1 判定「是否冻结」

冻结不是 locator 自带的属性，而是**读时**判定（`zeroflush_db.cc:1013`）：

```
ref.gen == ActiveGen(part_id)  → 活跃代：可能仍在写缓冲/文件尾，走内存+文件混合路径
ref.gen != ActiveGen(part_id)  → 已封存：走本节的 SealedFileCache 段目录
```

> 活跃代（gen == active）的记录**不是**安全的 FPGA 只读目标：段尾可能还在追加写缓冲
> （`wal_manager.cc:584`，offset ≥ flushed_size 时目标字节在 RAM）。排序/物化输入应取
> **已登记进段目录的封存代**。

### 9.2 两级解析（FPGA 必须复现的两级表）

```
locator ─ (part_id, gen, wal_offset)
              │
              ▼  L1：段目录查找（host 维护，FPGA 内复制一份）
   (part_id, gen) ──────► 段基址 base
                           ├─ 磁盘：该代 WAL 段文件起点（SSD 上的 zf-wal-<part>-<gen>.log）
                           └─ 或内存：整段载入的 DMA 缓冲 IOVA（物化路径）
              ▼  L2：帧内寻址
   record_addr = base + wal_offset         // 帧首，精确字节偏移
              ▼  §10：解析 ZfRecord
   value = record_addr + 24 + key_len（len = val_len 字节）
```

CPU 侧的段目录实现 = `SealedFileCache`（`sealed_file_cache.h`）：

| 要素 | 内容 | 位置 |
|---|---|---|
| 表 key | `(part << 32) \| gen`（`ZfFileKey`） | `sealed_file_cache.h:34-38` |
| 表 value | 该代 WAL 段的**只读句柄** `RandomAccessFile`（LRU 缓存） | `sealed_file_cache.h:63-66,161-165` |
| 登记 | freeze 时 `AddEpochWithRecoveryAdoption` 把 (part,gen) 全部入库 | `sealed_file_cache.h:82` |
| 删除 | `ReleaseGens` 引用归零 → `pending_unlink_` → `PurgePending` 真 unlink | `sealed_file_cache.h:109-124` |
| 取句柄 | `Get(part, gen, &rf)`；未登记返回 NotFound | `sealed_file_cache.h:115` |

**段文件名规则**：`<dir>/zf-wal-<part>-<gen>.log`（`wal_manager.cc:25-27`），即 `(part_id, gen)`
唯一对应磁盘上一个物理文件。

### 9.3 定点读 + 取 value（CPU 参考实现）

`zeroflush_db.cc:1029-1036` + `wal_manager.cc:318-…`（`ReadFromSealed`）：

```
rf = sealed_cache.Get(part_id, gen)                    // ① L1 段目录
读 rf@wal_offset 起始 ≥ 24 B                           // ② 一次预读 header+inline body
解 header 得 key_len, val_len；total = 24+key_len+val_len+4
  整条在缓冲内 → 直接用；超长 → 补读 body
DecodeZfRecord → h/k/v
若 h.type == kTypeDeletion(0x0) → value 为空
否则 out = v（value body，24+key_len 起，val_len B）
```

> CPU 侧另有按 locator 精确键控的 **value cache**（段不可变，缓存精确，`zeroflush_db.cc:993-1001`）；
> 与 FPGA 无关，可绕过。

---

## 10. ZfRecord 帧格式（WAL 单条记录，24 B 头，小端）

源码：`zeroflush/wal_format.h:11-15,28-43`；`wal_format.cc:12-13`。

### 10.1 帧总布局（文件内按此连续排布）

```
[header 24 B] ‖ [key key_len B] ‖ [value val_len B] ‖ [crc32c 4 B]
```
`ZfRecordLength = 24 + key_len + val_len + 4`（`wal_format.cc:12-13`）。`wal_offset` 指向
**header 起点**；记录可跨 4 KB 块，无需对齐，offset 是精确字节偏移。

### 10.2 Header 24 B 字段表（全部小端）

| 字节偏移 | 宽度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | magic | `kZfMagic = 0x31304655`（注释标 'ZF01'；按 4B 数值比对即可，勿当文本） |
| 4 | 2 | cf_id | 列族 id（v1 恒为 0） |
| 6 | 1 | type | RocksDB `ValueType`：kTypeValue=0x1 / kTypeDeletion=0x0 … |
| 7 | 1 | flags | bit0 保留（未来前缀压缩） |
| 8 | 4 | key_len | 下列 key body 字节数 |
| 12 | 4 | val_len | 下列 value body 字节数 |
| 16 | 8 | seq | 全局单调递增，显式落盘 |

Body：`key`（24..24+key_len−1）→ `value`（24+key_len … 24+key_len+val_len−1）。
Trailer：crc32c 4 B，覆盖 **header+body**（`wal_format.cc:65-68`）。

### 10.3 端序速查

| 对象 | 端序 | FPGA 处理 |
|---|---|---|
| magic / 各长度 / seq | LE | 小端平台直读；组装后按数值用 |
| body key/value | 原始字节 | 无转换 |
| crc32c | LE | 标准 crc32c（`util/crc32c.h`） |

---

## 11. FPGA 落地：段目录表的两种形态

**形态 A — 段在 SSD，FPGA 直读磁盘。**
host 把每个在册 `(part_id, gen)` 的段元数据（文件起点 → 存储 LBA/块设备偏移）同步进 FPGA
段描述符表；FPGA 查表 → 用 `wal_offset` 做文件内偏移发 SSD 随机读。缺点：每取一 value 一次
磁盘随机读，latency 高；只适合低频点取。

**形态 B — 整段载入 DMA 可见内存（推荐，贴合排序/物化）。**
物化路径本来就把整代封存 WAL 一次读入内存、之后用 `wal_offset` 当**内存下标**二分取值
（`materialize_aside.h:129,159-165`；`materialize_job.h:271` D1 整段读）。host 在
`AddEpoch`（或物化载入）时把 `(part_id, gen) → 连续缓冲 IOVA` 写入 FPGA 段描述符表
（BRAM/HBM 表均可），引用计数未归零则**基址稳定**：

```
addr = base[part,gen] + wal_offset          // 帧首
val  = addr + 24 + key_len,  长 = val_len    // 解析 §10
```

同段内多个 locator 近似有序时就是流式连续读——与 level-0 排序扫描同构。

**host ↔ FPGA 同步点**（缺一不可）：
- `AddEpochWithRecoveryAdoption` / 物化载入完成 → 表项 `(part,gen)→base` **生效**；
- `ReleaseGens` 引用归零 / `PurgePending` 删除前 → 先通知 FPGA **失效**该表项；
- FPGA 命中不到表项（未知 / 已回收 gen）→ 返回错误，**禁止野读**。

**坑清单**：
1. `wal_offset` 不能当全局地址用，必须 `base[part,gen] + wal_offset`；
2. 目标在 `wal_offset` 的是**帧首**不是 value，value 需再按 `24+key_len` 前跳并读 `val_len`；
3. `type == kTypeDeletion` 的记录 value 为空（值为删除标记）；
4. 活跃代（gen == active）不进段目录，别拿它当冻结数据 offload。

---

## 12. 对当前排序阶段的意义

排序/解析跳表**只需要 key 序，不需要解引用 locator**——把 16B locator 当作负载随 key 一起
透传即可。§9–§11 是为后续「物化取 value / 定点 Get」预留的读路径规格；实现顺序上应先做
level-0 扫描 + key 排序，再按 §10 在目标 value 需要落地时补帧解析。

---

# 下篇：SST / WAL 整体格式 + 输入输出接口（FPGA 排序器后端）

> 上文（§1–§12）解决「**读**：解析 memtable 得到排序键流」。本篇解决「**写**：排序键流 →
> value → 落成引擎可直接读的 SST」，并给出三路 I/O（slim memtable 进、WAL 进、SST 出）的接口设计。
> 位准约定（已确认）：**SST = 字节级标准 RocksDB BlockBasedTable**，能被本引擎原样打开；
> 最小可读基线（无压缩、crc32c、单层 binary-search 索引、无 bloom/ribbon 过滤器）。

---

## 13. SST 帧格式 —— 数据块 / 索引块单条记录与块尾

> 本引擎一切"块"（data / index / metaindex / properties）都由同一个
> `BlockBuilder` 编码（`table/block_based/block_builder.cc`），再用统一 5 B trailer 落盘。
> 所以先讲"**块内记录**"，再讲"**每个块的尾**"，最后（§14）讲块如何拼成整文件。

### 13.1 块内单条记录（BlockBuilder::Add 输出）

一条记录 = **键前缀压缩 + 定长值区**，依次为（全为 varint32，7-bit 组 LSB-first）：

```
[varint shared_bytes] [varint non_shared_bytes] [varint value_length]
[key 后缀字节 × non_shared] [value 字节 × value_length]
```

- `shared_bytes`：与**上一条 key** 共享的字节数；**restart 点记录恒为 0**。
- `key 后缀` = 本条 key 去掉前 shared 字节后的部分；重组规则：`key = 上一条 key 前 shared 字节 ‖ 后缀`。
- data / index 块的 key 是 **internal key**；metaindex / properties 块的 key 是名字字符串（§13.4 表）。
- `value_length` 恒存在（数据块 `use_value_delta_encoding=false`、index 块在 format_version 2 下也非 delta，
  `block_based_table_builder.cc:1066, 1087-1088`）。

源码锚点：`table/block_based/block_builder.cc:264-367`（Add 与 restart 判定）。

### 13.2 块尾三件套（restart 数组 + packed footer + 5 B trailer）

任意块内容写完（`BlockBuilder::Finish`，`block_builder.cc:189-220`）追加：

```
[restart_offset × num_restarts : 每项 u32 LE]   ← 各 restart 点相对块内容的字节偏移
[packed footer 4 B LE]
[文件层 trailer 5 B]                            ← §13.3
```

**packed footer**（`table/block_based/data_block_footer.h/cc`）单 u32，低位 **28 bit = num_restarts**，高位 4 bit 是特性位：

| bit | 含义 | 本基线值 |
|---|---|---|
| 31 | kDataBlockBinaryAndHash（块内含 hash 表） | 0 |
| 30 | 保留（不可用） | 0 |
| 29 | is_uniform（`uniform_cv_threshold>=0` 才可能置位） | 0 |
| 28 | separated KV（键值分区存放） | 0 |

> `uniform_cv_threshold` 默认 `-1`（`include/rocksdb/table.h:702`）→ `is_uniform=false`，packed 值
> 就**等于 num_restarts**。不要开启该选项，否则需额外扫描均匀性。

#### 13.2.1 packed footer 的高 4 位（bit 28..31）是干嘛的：特性标志 + 保留

先消歧：盘上整块 packed footer 就是 **u32 LE 32 bit**；其中**低 28 bit 才是 num_restarts**，高 4 bit 是
特性/元数据标志。数"restart 数 / data 块数"永远只读低 28 bit（§14.3.1 已按此写）。

**为什么 num_restarts 敢只给 28 bit**：`data_block_footer.h:53-56` 的推导 —— 块最长 4 GiB（u32 块长上限），
interval=1 时每条记录最小 ≈ 16 B（3 B varint + 9 B internal key + 空 value + 4 B restart 偏移），restart 数
物理上限 ≈ (2^32−4)/16 ≈ **2.68 亿 < 2^28**；顶 nibble 空出来放特性位没有任何代价。

| 位 | 标志（`data_block_footer.cc:17,19,21`） | 置位后发生什么 |
|---|---|---|
| 31 | `kHashIndexBit`：块内嵌哈希索引 | 该块以 `data_block_index_type=kDataBlockBinaryAndHash` 写入：在 restart 二分之外，块内还带一张哈希索引加速点查（reader 的 index_type 分支 `block.cc:1331`）。默认 `kDataBlockBinarySearch=0`（`table.h:295-300`）→ 恒不置位 |
| 30 | **保留 reserved** | 无任何含义；未升格式版本前**必须恒 0**。一旦被置位 reader 一律 Corruption（兼容安全阀，见下） |
| 29 | `kUniformKeysBit`：键近似均匀 | 写侧 `uniform_cv_threshold>=0` 才会评估置位；表示键均匀分布，reader 在 Bytewise 比较器下可选插值/自动搜索路径加速（`block.cc:1587`）。默认 `-1` → 恒不置位 |
| 28 | `kSeparatedKVBit`：键值分段存放 | data 块把键、值分两段存；置位时 packed footer 之前**还会多一个 u32 `values_section_offset`**（值段起点），该块 footer 合计 **8 B**。本 fork 构造处硬编码 `use_separated_kv_storage=false`（`block_based_table_builder.cc:1081`）→ 恒不置位，footer 恒 4 B |

> 高位只放"整个块怎么解析"级别的特性，不放每块可变的小参数（那些仍走记录流/restart 自描述）——
> 因为这 4 bit 一旦被旧代码误当 num_restarts 就会灾难性错位，只能承载"低频、格式级"的信号。

**reader 的解析顺序（为什么"未知位必须报错"）**：`DataBlockFooter::DecodeFrom` 从内容末 4 B 读出 u32，
先把三个已知标志（31/29/28）逐个剥掉，**再检查剩余值是否仍 > kMaxNumRestarts（=0x0FFFFFFF）**；
若是 → 返回 Corruption("reserved bits set")（`data_block_footer.cc:76-84`）。这样未来版本一旦使用 bit30
或更新的位，旧 reader 会**响亮地失败**而不是把 num_restarts 读成一个巨大的数、随后静默解析错位。
`data_block_footer.h:43-48` 还特别警告 bit30：更老的老代码会把整个 u32 当 num_restarts 使用、再乘 4，
溢出反而可能绕过 corruption 检查 —— 因此 bit30 被当成"碰都不能碰"的保留位。

**FPGA 侧规则**：
- **读/校验**：`packed = u32 LE`；先查 `(packed >> 28) == 0` —— 非 0 说明 hash / 分离 KV / uniform 至少
  一个被置位，**该块内容布局已不是"记录流 + restart 数组"**（例如分离 KV 会另带值段偏移），不能按本文档
  解析，直接判非本基线文件；再取 `num_restarts = packed & 0x0FFFFFFF`。
- **写**：三标志对应的写侧开关在本基线都锁死 —— `data_block_index_type=kDataBlockBinarySearch`、
  `uniform_cv_threshold=-1`（§14.6 已列）、分离 KV 本 fork 无用户开关。任何一块 footer 高 nibble ≠ 0
  都说明引擎配置漂了。

### 13.3 每块 trailer 5 B（文件层统一加）

块内容（restart 数组 + packed footer 之后的整体）写出后紧跟 5 B：

```
byte0        = 压缩类型。kNoCompression = 0x00（本基线恒为 0）
byte1..4     = LE32 校验和
校验和 = Mask( crc32c::Value(块内容) 续算 1 字节压缩类型 byte0 )
Mask(x) = rotl32(x, 17) + 0xa282ead8   （mod 2^32）
```

源码锚点：`block_based_table_builder.cc:2184-2205`（trailer 组装）；
`table/format.cc:643-651`、`util/crc32c.h:37-46`（checksum = Masked crc32c，扩展最后一个 type 字节）。
文件层 `kBlockTrailerSize = 5`（`block_based_table_reader.h:78`），对所有块（含 metaindex/properties）一致。

### 13.4 各块类型参数与 value 语义

| 块 | 内容 key | 块内 value | engine 默认 | 基线建议 |
|---|---|---|---|---|
| data | internal key | 用户 value 原字节 | restart=16，delta 开（`table.h:351`、`use_delta_encoding=true`） | restart=1（每条全键，无前缀压缩，FPGA 最省事；§14.6 权衡） |
| index（单层） | separator internal key（必含 8 B seq/type，见 §14.3） | BlockHandle = `[varint offset][varint size]` | restart=1（`table.h:354`），无 value-delta（v2） | restart=1 |
| metaindex | meta 块名（字节序，如 `"rocksdb.properties"`） | BlockHandle | restart=1（`meta_blocks.cc:36`） | 同 |
| properties | 属性名（字节序） | varint / 字符串（§14.4） | restart=INT_MAX（单 restart）（`meta_blocks.cc:55-57`） | 同左或 1 |

> **restart 区间与 delta 编码是"写侧自由参数"**：reader 从块内 restart 数组自描述解码，不需配置文件侧用了多少。
> 选择 restart=1 时每条记录 shared=0、天然是 restart 点，键不共享 —— 编码最简、可逐条独立生成。

### 13.5 字节级示例（基线 restart=1，两键数据块）

两条记录：user key `"a"`、`"b"`，seq=0x100，type=kTypeValue(0x1)。
internal key 9 B = `user ‖ LE64((seq<<8)|type)`：`"a"`→`61 | 01 00 01 00 00 00 00 00`，`"b"`→`62 | ...`；值 `"v1"`、`"v2"`。

```
00 09 02 | 61 01 00 01 00 00 00 00 00 | 76 31        offset 0x00, 14 B
00 09 02 | 62 01 00 01 00 00 00 00 00 | 76 32        offset 0x0E, 14 B
└─varint×3─┘└──── internal key(9) ────┘└ value(2) ┘
restart 数组  00 00 00 00  0E 00 00 00                  (0 与 14)
packed footer 02 00 00 00                               num_restarts=2
trailer       00  3A F4 09 65                        type=0 + masked crc32c（算例见 §13.5.1）
```

decode 规则：末 4 B 是 packed footer → 得 num_restarts；其前 `num_restarts×4` 字节是 restart 偏移表；
restart[0]=0 → 从块头解出第一条（shared 必为 0）→ 依 non_shared 与 value_length 前跳，继续解析。

#### 13.5.1 校验和逐步算例（块内容 = 上表 40 B，压缩类型 = 0x00）

**被校验的字节** = 块内容(records + restart 数组 + packed footer,上表 40 B)**再加 1 字节压缩类型 0x00**
（校验和**不含** trailer 自身的 5 B；该结论由 `Value(content‖type)` 链等价推出，见 §13.3）。

```
内容(40B)：00 09 02 61 01 00 01 00 00 00 00 00 76 31 00 09 02 62 01 00 01 00 00 00 00 00 76 32
           00 00 00 00 0E 00 00 00 02 00 00 00
```

RocksDB 落地值（已用本仓库 `util/crc32c.cc` 编译验证）：

| 步 | 输入 | 结果 |
|---|---|---|
| ① | `crc32c::Value(content)` | `0D8E14E5` |
| ② | 续算 1 字节 `0x00`（即 `Extend(①, "\x00")`）＝`crc32c(content‖0x00)` | `84B16143` |
| ③ | `Mask(②)` | `6509F43A` |
| ④ | 写入 trailer：压缩字节 `00` ‖ ③ 的 LE32 | 落盘 5 B = `00 3A F4 09 65` |

Mask 展开（② = `84 B1 61 43`）：
```
rotl32(x,17) = (x>>15)|(x<<17)   →  C2870962      （00010962 | C2860000）
+ kMaskDelta  0xA282EAD8        →  6509F43A      （C2870962 + A282EAD8, mod 2^32）
```

**FPGA 侧为什么方便**：CRC-32C 是逐字节流式状态机，①和②之间不需要断点或重扫——
从块头起跑、把 block 全 40 B 灌入、再灌一个 `0x00`，比较寄存器最终值是否为 `6509F43A` 即可。
也等价于直接 `crc32c(41 B)` 一步到 ②。读取端校验 = `Unmask(读到的 4B)` 与 ② 比较，
任何一位翻转都会失配（CRC 不是哈希，但可捕获块内所有单位翻转与绝大多数多位错）。

**自己动手验证**：本仓库 `util/crc32c.cc` 与标准 CRC-32C（Castagnoli, poly `0x82F63B78`，init/xorout
均为 `0xFFFFFFFF`）一致，`crc32c("123456789")==0xE3069283` 是标准检验向量，可用来先自检工具链。

#### 13.5.2 CRC-32C 逐位机制与查表等价（FPGA 移植用真值）

> 本层讲"寄存器内部每一拍发生了什么"，是上一节结果的**生成过程**。CRC 的本质是 GF(2) 多项式
> 除法取余，这里略去；只给可直接照搬的算法与一个可手工复算的例子。

**寄存器版（逐位真值，和硬件 `crc32` 指令同一套）**——CRC-32C 是 bit-reflected 版本
（字节按 LSB 优先喂入），故不是"高位凑 1 就 XOR"，而是**右移、最低位为 1 才 XOR**，
反射多项式常数 `0x82F63B78`，两端各补一次全 1：

```
crc = 0xFFFFFFFF                       // 初值
for 每个字节 b:
    crc ^= b                           // b 异或进最低字节
    重复 8 次:                         // 每 bit 一步
        最低位为 1:  crc = (crc >> 1) ^ 0x82F63B78
        最低位为 0:  crc =  crc >> 1
return crc ^ 0xFFFFFFFF                // 末值
```

> `0x82F63B78` 是 CRC-32C（Castagnoli）反射常数；与 zlib 的 CRC-32（IEEE，`0xEDB88320`）
> **不是一回事**，别混用。

**手算一个真实字节 `'A'=0x41`（每步可验）**：`0xFFFFFFFF ^ 0x41 = 0xFFFFFFBE` 起，右移 8 次，
最低位为 1 时 `(>>1) ^ 0x82F63B78`：

| 步 | 上一步 | 最低位 | 动作 | 结果 |
|---|---|---|---|---|
| 0 | `FFFFFFBE` | 0 | `>>1` | `7FFFFFDF` |
| 1 | `7FFFFFDF` | 1 | `3FFFFFEF ^ 82F63B78` | `BD09C497` |
| 2 | `BD09C497` | 1 | `5E84E24B ^ 82F63B78` | `DC72D933` |
| 3 | `DC72D933` | 1 | `6E396C99 ^ 82F63B78` | `ECCF57E1` |
| 4 | `ECCF57E1` | 1 | `7667ABF0 ^ 82F63B78` | `F4919088` |
| 5 | `F4919088` | 0 | `>>1` | `7A48C844` |
| 6 | `7A48C844` | 0 | `>>1` | `3D246422` |
| 7 | `3D246422` | 0 | `>>1` | `1E923211` |

验算示例：步 2 `BD09C497 >> 1 = 5E84E24B`，`5E84E24B ^ 82F63B78 = DC72D933`。8 步后寄存器
`1E923211`，末值 `^ 0xFFFFFFFF` → **`crc32c("A") = E16DCDEE`**。

**多字节 = 接着滚**：`crc ^= b` + 8 次右移这组动作对每个字节**继续上次的寄存器**做——寄存器
只有一份，从头滚到尾。§13.5.1 的 40 B 块 = 初值起对 40 内容字节 + 1 个 `0x00` 各做一次，滚完
= `84B16143`。

**为什么能查表（等价于逐位）**：一个字节的 8 步只跟 `crc` 最低 8 位（异或 `b` 后）有关，
高 24 位只是被右移并叠加多项式反馈。故把 8 步预计算成 256 项表，每字节只需 1 步：

```
table[c] = 以「寄存器低字节 = c」起步做那 8 步的结果
每字节一步:  crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8)
```

这正是 RocksDB 内层 `util/crc32c.cc`：`c = (l & 0xff) ^ *p++;  l = table0_[c] ^ (l >> 8);`
（外层再包初值/末值补码）。

**FPGA 实现两条路**（常数、初值、末补码三处对齐即与引擎一致）：
1. **查表**：BRAM 放 256 项，每字节 1 拍；适合低频/小块。
2. **组合展开**：不用表，把 8 次「右移 + 按位 XOR 多项式」反馈并行掉，每字节 1 拍，
   纯 LUT，适合流水线吞吐主路径。

### 13.6 真实数据块规模（例中的 40 B 是教学用最小样例）

**varint 编码没有简化**：`[varint32]` 是通用 LEB128（7-bit 一组、LSB 在前、最高位=续传）。
上面例子三条 varint 都恰好 1 B，只是因为数值 < 128（shared=0、non_shared=9、value_length=2）。
数值 ≥ 128 时编码会自然变长，例如 `value_length=300` → `AC 02`（`300 = 0x02C…`，低 7 位 `2C|80`，次组 `02`）。
真实块里 `value_length`（value 长度）最常见到 2~4 B；`non_shared`≤内部键长，日常 <128 仍 1 B。
解析器必须按「读 varint → 依续传位决定是否多读 1 B」处理，不能假设定长。

**真实一个数据块的内容 ≈ `block_size` 选项（默认 4096 B）**，不是 40 B：

- 目标值 `block_size = 4*1024`、`block_restart_interval = 16`、`block_size_deviation = 10`
  （`include/rocksdb/table.h:337-352`）。块内容的刷新由 `FlushBlockBySizePolicy`
  （`flush_block_policy.cc:45-73`）在两种情形下触发：
  1. 现有估算 ≥ block_size（≈4096）；或
  2. 当前已超过 `block_size×90%`（deviation 10）且再加这条 KV 就会超过 → 提前封块。
- 所以**每个数据块内容通常落在 ~3.7 KB ~ ~4.3 KB**，与 40 B 例子相差约 100 倍；文件层再各加 5 B trailer。
- 例中 2 条记录只是「结构最小可见」：真实块 = 同一条记录格式**重复 ~几十到几百次** +
  每 `restart_interval`(=16) 条一个 restart 偏移（`num_restarts` ≈ 记录数/16，默认下 ~几十项），
  块结构（restart 数组 + packed footer + 5 B trailer + CRC 位置）与 40 B 例**逐字节同构**。

单条平均记录越大，每块条数越少。量级参考（内部键 32 B，restart=16）：

| value 平均大小 | 平均单条内容 | 每 4KB 块条数 |
|---|---|---|
| 100 B | ~140 B | ~29 |
| 512 B | ~550 B | ~7 |
| 4 KB | ~4.1 KB | 1（单条已近/超一整个块） |

而 properties/metaindex/index 尾块**不受 `block_size` 约束**：properties 通常仅几百 B，
metaindex ~百 B，index 大小 ≈ 数据块数 × 单条索引项（几十 B）。默认单层 SST 约 64 MB
（`target_file_size_base`），即一个 SST ≈ 上万个数据块、索引块随之几千~几万条。

### 13.7 varint 解码：为何不补 0、怎么切分、FPGA 实现

**为什么不补 0 到定长**：每个 varint 字节的**最高位（bit7 = 0x80）就是续传/终止标志**——

| 字节 bit7 | 含义 |
|---|---|
| 1 | 后面还有字节，继续读 |
| 0 | 这是本 varint 的**最后一字节** |

结束位置写在流内、跟着每个 varint 走，解析器永远不需要预先知道长度 → **自定界，无需补零**。
若改成定长补零：每条记录都要为「大概率很小的数」多付固定字节，4 KB 块装几百条时浪费可观的
块空间，RocksDB 不会接受。

**解码算法**（7-bit 累加循环，LEB128 的逆）：

```
value = 0; shift = 0
do {
    b = 读下一字节
    value |= (b & 0x7F) << shift   // 每字节只取低 7 位
    shift += 7
} while (b & 0x80)                 // bit7=1 继续；=0 收尾
```

**切分规则**：读 varint 不需要「它该有多长」的前置知识——语法固定一条记录 =
**3 个自终止的 varint**（shared / non_shared / value_length），随后精确地跟 `non_shared` 个
key 字节 + `value_length` 个 value 字节；读够这些字节，下一字节必是下一条记录的头部。
三个 varint 之间、varint 与 key/value 之间**不存在二义性**。

**混合长度示例**：一条 `shared=3, non_shared=22, value_length=300` 的记录，头部是 `03 16 AC 02`
（第三、四条头 varint 两字节长），逐字节：

| 读到 | bit7 | 动作 |
|---|---|---|
| `03` | 0 → 终止 | shared = 3，切到下一 varint |
| `16` | 0 → 终止 | non_shared = 0x16 = 22 |
| `AC` | 1 → 续传 | value_length = 0x2C = 44（低 7 位） |
| `02` | 0 → 终止 | value_length \|= 2<<7 → 44 + 256 = **300** |

随后跳过 22 B key + 300 B value 即到下一条记录。任何字节组合都唯一可逆——
这就是「短不补零也读得对」的原因：**终止位等价于每个 varint 内部自带长度**。

**FPGA 实现**（两种都简单，无需猜长）：

1. **串行**：每周期取 1 字节，`acc |= b[6:0] << shift; shift += 7`，`b[7]==0` 即完成。
   3~5 周期解一个 varint。
2. **并行（高吞吐）**：varint32 最长 5 字节 → 一次取 5 字节，并行生成「取前 k 字节」的
   候选值（k=1..5），用**第一个 `b[7]=0` 的位置**多路选择：单周期出值 + 输出本次实际消费
   字节数（三条 varint 消费字节之和即头部总长，用于推进到 key/value 区）。

**错误兜底**：`value_length` 理论上限 ~4 GB，文件损坏可解出荒谬长度导致解析错位——
块级 CRC32c 覆盖全部内容字节会兜底；FPGA 读到长度超块长即可判坏块，触发整块重读或报
corruption，不必冒险继续解析。

### 13.8 internal key：seq 与 kTypeValue 为什么熔进 key（含三处存储对照）

**kTypeValue 是什么**：RocksDB 每条写操作的**类型标签**（`db/dbformat.h:42-44` ValueType 枚举）：

| 值 | 名字 | 含义 |
|---|---|---|
| 0x0 | `kTypeDeletion` | 墓碑：标记该 user key 此前的旧版本作废 |
| **0x1** | **`kTypeValue`** | 真数据（Put 的 value） |
| 0x2 | `kTypeMerge` | 待合并操作（未完成的原子操作，FPGA加速器可忽略） |

type 决定**读命中时这条算不算数**：点读沿 internal-key 序扫到某 user key 最新可见版本时，
`type=1` → 返回 value；`type=0` → 视为「key 不存在」（墓碑遮蔽更旧版本）。所以 type 不是
value 的附属，而是**排序流里决定可见性**的一等公民——这就是它必须跟 key 一起存的原因。

**为什么 key 要塞 seq/type**：LSM 存储是**有版本**的，同一 user key 可同时存在多条不同 seq
的版本（更新/快照/compaction 途中），SST 与 memtable 都要按 `(user key 升序, seq 降序)`
统一排序（见 §3.3 / §4），读路径也要沿这条序找「seq ≤ 快照的最新一条」。key 若只存 user key，
块内既无法排序也无法识别墓碑。internal key = `user_key ‖ 8B trailer`、`trailer = LE64((seq<<8)|type)`
（seq 高 56 位、type 最低 1 字节）正是这套语义的载体。编码/字节序详解见 §3.3（含「type 在最前」
的解析坑）、SST 例见 §13.5 的 9 B key。

**三处存储对照（seq/type 各自在哪）**：

| 存放处 | key 形态 | seq | type |
|---|---|---|---|
| slim memtable（分区跳表） | **internal key** = user key ‖ 8B trailer（`partition_index.h:5`，插索引时现拼，见下） | 熔在 trailer 高 56 位 | 熔在 trailer 最低字节 |
| SST data block | **internal key**（标准 BlockBasedTable，§13.5 例） | 熔在 trailer | 熔在 trailer |
| WAL ZfRecord | **裸 user key**（帧头 `key_len` 指定，无 trailer） | 帧头独立字段 8B（§10 表） | 帧头独立字段 1B（§10 表） |

跳表「现拼 trailer」的代码实证：写路径 WAL 先 `Append(part, key, value, type, seq)` 存裸 user key
（`zeroflush_db.cc:560-575`），随后才 `PutFixed64(&ik_buf, PackSequenceAndType(seq, type))`
拼成 internal key 插索引（`zeroflush_db.cc:588-596`）。**同一份 user key+seq+type**：WAL 拆开存
（append-only，按全局 seq 顺序、不按 user key 排序，无需 key 内排序键）；跳表/SST 熔成 internal key
（要按 user key 排序）。恢复时直接读帧头 seq 重建全局序，不用解 trailer。

**分界小结（两条都是"固定宽度计数"，不是扫描/标记）**：
- **分界 A：user key ‖ trailer** —— trailer **恒为末尾 8 B**，`user_key_len = ik_len − 8`
  （SST 记录里 = non_shared 解码出的内部键总长再减 8）。user key 可变长、可含任意字节含 NUL，
  分界不靠内容，只靠"末尾留 8 B"。
- **分界 B：type ‖ seq**（都在 trailer 内）—— trailer 小端写、低字节在前，故
  **type 是紧贴 user key 之后的那 1 字节**，其后 7 B 才是 seq（LE，最高字节是 internal key 的**最后 1 字节**）。

```
internal key（ik_len 总长）：
┌── user key（变长，任意字节）──┬ t ┬── seq ───────────────┬
                               │1B │   7B 小端，高字节在后   │
                               └ type│                      │
  分界A: 从末尾数 8B 得 trailer     │ 分界B: trailer 低字节=type
  剩下的都是 user key ─────────────┘ └ internal key 最后 1 字节 = seq 最高字节（不是 type）
```

> ⚠️ 常见误解（含本文档早期口头表述）：**internal key 的最后 1 字节不是 type**，而是 seq 的
> 最高字节。type 在"紧贴 user key 之后"那 1 字节（= LE64 的低字节，因小端写而排在最前，
> 见 §3.3 例 `01 77 66 …`）。FPGA 若把最后 1 字节当 type、或对整体做 memcmp，都会错序。

---

## 14. SST 整体文件格式（BlockBasedTable，format_version=2 基线）

### 14.1 整文件布局（自上而下，全部字节流线性拼装）

```
┌────────────────────────────────────────────┐
│ data block 0         …        （§13 编码）   │  +5B trailer
│ data block 1         …                     │  +5B trailer
│  …（本基线：任意数据块数）                     │
├────────────────────────────────────────────┤  ← 文件尾起点 tail_start
│ index block（单层 binary-search 索引）        │  +5B trailer   （§14.3）
│ properties block                            │  +5B trailer   （§14.4）
│ metaindex block                             │  +5B trailer   （§14.5）
│ footer 53 B（format_version<=5 恒长）         │  （§14.2）
└────────────────────────────────────────────┘
```

写序固定为：data 块（Add 时流式刷出）→ 尾部按
index → (compression_dict, 无) → (range_del, 无) → properties → metaindex → footer
（`block_based_table_builder.cc:2891-2913` Finish）。本基线无 filter、无压缩字典、无 range tombstone，
故尾部只有 index/properties/metaindex/footer 四项。

### 14.2 footer 53 B 字节地图（format_version 1..5 恒 53 B）

`Footer::kNewVersionsEncodedLength = 1 + 40 + 4 + 8 = 53`
（`table/format.h:297-299`；字节布局与 Build 见 `table/format.cc:225-328`）：

```
偏移  长度  内容
0      1    checksum_type = 0x01 (kCRC32c)
1..??  ≤20  metaindex_handle : [varint offset][varint size]
??..?? ≤20  index_handle      : [varint offset][varint size]（紧接上一 handle 续写）
??..40 ≤40  part2 合计 40 B；两 handle 之后剩余字节 0 填充
41..44  4    format_version  = 02 00 00 00   (u32 LE = 2)
45..52  8    magic (u64 LE)   = f7 cf f4 85 b7 41 e2 88   (kBlockBasedTableMagicNumber)
```

> 两个 handle 的 offset 都是**绝对文件偏移**；part2 是"**按最大编码长度预留 40 B**"，两 handle 串联写入、
> 不足部分以 0 填充（读取端忽略 padding，`format.cc:459-469`；`FooterBuilder::Build` 见 `format.cc:320-326`）。
> footer 在 v≤5 下**无**自身校验和（v≥6 才有）。读端先取文件**末 53 B** 判 magic/version，再据此读 meta/index。

#### 14.2.1 两个 handle 的精确字节组织（为什么"没有固定边界"）

设 footer 在文件里的起点为 `F`（= 文件总长 − 53）。精确字节地图：

```
F+0        1B   checksum_type = 0x01 (kCRC32c)
F+1..F+1+m mB   metaindex_handle = [varint64 offset][varint64 size]，m ∈ [2,20]
F+1+m..F+1+m+n nB  index_handle   = [varint64 offset][varint64 size]，n ∈ [2,20]，紧接上者续写
F+1+m+n..F+40   0 填充（m+n=40 时无字节；读取端跳过/不校验）
F+41..F+44   4B  format_version = 02 00 00 00 (u32 LE)
F+45..F+52   8B  magic (u64 LE) = f7 cf f4 85 b7 41 e2 88
```

要点：

1. **没有固定槽位/长度字段**：一个 `BlockHandle` = **两个连续 varint64**（先 offset 再 size，
   `format.cc:55-62` EncodeTo；`DecodeFrom` 顺序读两个 varint）。varint64 是 LEB128、每段 1..10 B，
   故 handle 编码长 `2..20 B`（"20 B 上限" = `kMaxEncodedLength = 2×kMaxVarint64Length`，
   `format.h:76`）——**不是每个 handle 都 20 B**，offset/size 小就短。
2. **边界是推导出来的，不是存的**：metaindex 的 size varint 读毕（遇终止位停），下一字节天然就是
   index 的 offset 开头；无需中间任何定界/长度头。
3. **handle 的 offset/size 语义**：offset = 块**内容**的绝对文件偏移；size = 内容长度、
   **不含**后 5 B trailer（`format.h:60` 注释）→ 磁盘 `[offset, offset+size)` 内容 + 
   `[offset+size, offset+size+5)` trailer。
4. **零填充可为空**：两 handle 填不满 part2（40 B）时余下全 0；若恰好 m+n=40 则零字节。

#### 14.2.2 完整 53 B 实例

设 metaindex 内容在文件偏移 9344、长 30；index 内容在偏移 8192、长 64（为便于阅读令 F=0）：

```
offset 9344 → varint 80 49       size 30 → 1e       ⇒ metaindex handle = 80 49 1e   (3 B)
offset 8192 → varint 80 40       size 64 → 40       ⇒ index handle     = 80 40 40   (3 B)

偏移  字节
 0      01                          ← checksum_type = 0x01
 1..3   80 49 1e                    ← metaindex_handle (offset 9344, size 30)
 4..6   80 40 40                    ← index_handle     (offset 8192, size 64)
 7..40  00 × 34                     ← 0 填充（m+n=6 B，剩余 34 B）
41..44  02 00 00 00                 ← format_version = 2
45..52  f7 cf f4 85 b7 41 e2 88     ← magic
```

**FPGA 解析顺序**：读 byte0 → 读 varint(offset) → 读 varint(size) = 得 metaindex 起点；
紧接再读两组 varint = index 起点；随后可直接推进到 byte41（跳过 `40−(m+n)` 个 0），无需校验 padding。
拿到 index_handle 后按它读 index 块，再由 index 条目（§14.3）获得各数据块 handle。

### 14.3 index block 语义（format_version=2 的关键简化）

- **键必含 8 B seq/type**：`index_builder.h:257` `must_use_separator_with_seq_ = format_version<=2`，
  恒 true → 单层索引键是**完整 internal key**（无"去 seq"优化分支）。
- 条目键 = 数据块 i 与其后块的 **separator**（`index_builder.h:267-296`）：
  - 非最后一块：`FindShortestInternalKeySeparator(last_key_i, first_key_{i+1})`
    —— 对两 user key 求公共前缀 p，若可把 p 后一字节 `c` 变 `c+1` 且仍 `<` 后块首键，则输出
    `p+(c+1) ‖ 8B(seq=kMaxSequenceNumber,type=kValueTypeForSeek)`；否则退化为**整条 last_key_i**（含 seq）。
  - 最后一块：`kShortenSeparators`（默认）下即 **last_key_last**（不缩短）—— default
    `index_shortening = kShortenSeparators`（`table.h:768-769`）。
- 条目值 = `[varint64 offset][varint64 size]` 完整 BlockHandle（v2 不做 size-delta，`block_based_table_builder.cc:1087`）。
- reader 据 props `index_key_is_user_key=0`、`index_value_is_delta_encoded=0` 选用**完整键 + 完整 handle**
  解码路径（`block_based_table_reader.cc:1135-1138`）。

#### 14.3.1 容器层面：index 块也是一个标准 block（restart=1）

> ⚠️ **粒度先分清：index 条目以"data 块"为单位，不是以 KV 为单位。** 一个 data 块里可以装几百条 KV
> （§13.6），但文件级 index block 中**一条记录 = 一个 data 块**，value 是那个整块的 handle。定位一条 KV 走
> **两级**：先在 index block 里二分找到所属 data 块（per-block），再在该块内部用**它自己的 restart 数组**找到
> 那条 KV（per-restart）。全链路三个粒度的索引别混：
>
> | 层 | 一个条目管多少 | 粒度 |
> |---|---|---|
> | slim memtable 跳表 | 一条 KV = 一个节点（无块结构） | per-key |
> | SST index block（文件级） | **一个条目 = 一个 data 块** | per-block |
> | data 块内部 restart 数组 | 一个 restart 点 ≤ interval 条 KV（默认 16；基线 1） | per-restart |
>
> 推论：下面"num_restarts == 条目数 == data 块数"**只对 index 块成立**。data 块自己的 num_restarts 是该块内
> restart 点个数（interval=16 时 ≈ ⌈块内KV数/16⌉；本基线 interval=1 时 == 块内 KV 数），跟"文件里有几个
> data 块"是两回事。即使 data 块 restart=1 做到块内 per-KV 定位，index block 依旧只有"每块一条"，不会因此
> 变成每条 KV 一条。

三层结构（记录流 → 块尾 → trailer）与 data 块**完全相同**（§13.1/13.2/13.3）：

```
index 内容 = 记录流 ‖ [restart_offset × num_restarts : u32 LE] ‖ [packed footer 4B] 
index trailer = 5B（同 §13.3，type=0x00 + masked crc32c）
```

要点：

1. **`index_block_restart_interval` 默认 = 1**（`table.h:354`）→ **每条记录都是独立 restart 点**，
   每条 `shared_bytes=0`、键**不共享**（BlockBuilder 虽开 key-delta 编码，interval=1 使前缀压缩永不生效）。
   于是 **num_restarts == 条目数 == 单层 data 块数**。块数**直接读 packed footer 的低 28 bit 即可**
   （restart 数组是它的物理随行物：长度恒为 `num_restarts×4` B，拿来数数反而绕路；它的用途是记录每条
   记录的起始偏移、供二分定位，见 §14.3.2 的 decode/seek）。两者由 BlockBuilder 在 Finish 时保证天然一致。
2. **一条记录 = 一个键值对**（`ShortenedIndexBuilder::AddIndexEntryImpl` → `index_block_builder_.Add`，
   `index_builder.h:226, 350-356`），字节 = §13.1 的记录头 + 键 + 值：

   ```
   [varint shared=0] [varint non_shared=len(index key)] [varint value_length=len(handle 编码)]
   [index key 字节 × non_shared] [BlockHandle 编码字节 × value_length]
   ```
3. **value = BlockHandle 编码**：内部就是**两个自定界 varint64 续写** `[varint64 offset][varint64 size]`
   （**没有**自己的长度/头，记录头的 `value_length` 即它的字节数，通常 2~5 B、上限 20 B）。
   v2 不写"首键"也不做 value-delta：`IndexValue::EncodeTo` 在 `previous_handle=nullptr` 时仅调
   `handle.EncodeTo`（`format.cc:100-113`）；构造期 `use_value_delta_encoding=false`
   （`block_based_table_builder.cc:1087-1088`）。offset/size 语义同 §14.2.1（offset 绝对文件偏移、size 不含 trailer）。
4. **键的两种形态**（都是 internal key，user 部分 + 8 B trailer）：
   - **缩短键**：能缩短时 = `p+(c+1) ‖ 合成 trailer`。合成 trailer =
     `PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek)`；本 fork `kValueTypeForSeek =
     kTypeValuePreferredSeqno = 0x18`（`dbformat.cc:28`）→ trailer 8 B = **`18 FF FF FF FF FF FF FF`**。
     ⚠️ **这是假键**：任何真实 DB 键都不会是 `(seq=2^56-1, type=0x18)`，它只用来排在"同一 user key 的
     一切真实版本之上"（同 user key 比较时 seq 越大越靠前），从而严格落在块 i 末键与块 i+1 首键之间。
   - **整键**：缩短条件不满足，或**末块**（默认 `kShortenSeparators`）时 = 块末键**原样**
     （带真实 seq/type，与 data 块里那 8 B 逐字节相同）。
5. **单层、kBinarySearch**：块内无 hash、无二级索引；查找 = 二分 restart 数组 → 落进一个 interval
   （此处 interval=1 → interval 内就一条，直接整键比较）。props `rocksdb.block.based.table.index.type=0`
   供 reader 选 `BlockBasedTableReader` 的二分路径（§14.4）。

> **byte-exact 与 `index_shortening`**：默认 `kShortenSeparators` 下非末块键会被缩短（且缩短键**必须照抄**，
> 不能"省事退回整键"——否则文件虽能读、但字节与 RocksDB 默认产物不一致）。FPGA 只有两条路可选：
> (a) 在写侧实现 FindShortestSeparator（输入两块交界两个 user key，纯字节操作，无状态）与合成 trailer 生成；
> (b) 在 §14.6 把 `index_shortening` 锁成 `kNoShortening`，令**所有**条目键 = 整条 last_key（省去全部缩短逻辑，
> 但这会让文件与"默认配置 RocksDB"逐字节不同，只能在自家引擎两侧同时锁定后成立）。

#### 14.3.2 字节级示例（2 个 data 块 → 2 条索引记录）

沿用 §14.1 文件：data0 内容 40 B（§13.5 的 a、b @seq 0x100），磁盘 `[0,40)` + trailer `[40,45)`；
data1 内容 30 B、首键 user `"k"`、末键 z @seq 0x100，磁盘 `[45,75)` + trailer `[75,80)`；
index 内容 = 下表 40 B，从文件偏移 `[80,120)`，trailer `[120,125)`。

**条目键推导**
- entry0（data0）：块末键 `"b"`@0x100 ↔ 后块首键 user `"k"`。公共前缀空，`0x62+1=0x63 < 0x6b('k')`
  → 可缩短 → 键 = `"c" ‖ (kMax, 0x18)`：`63 18 FF FF FF FF FF FF FF`（9 B）。
- entry1（data1，末块）：默认 `kShortenSeparators` → = 末键 `"z"`@seq 0x100 原样：
  `7A 01 00 01 00 00 00 00 00`（9 B）。

**value**
- entry0 → BlockHandle(offset 0, size 40)：varint64(0)=`00`、varint64(40)=`28` → 值 `00 28`（2 B）。
- entry1 → BlockHandle(offset 45, size 30)：varint64(45)=`2D`、varint64(30)=`1E` → 值 `2D 1E`（2 B）。

```
00 09 02 | 63 18 FF FF FF FF FF FF FF | 00 28        offset 0x00, 14 B
00 09 02 | 7A 01 00 01 00 00 00 00 00 | 2D 1E        offset 0x0E, 14 B
└varint×3┘└───── internal key(9) ─────┘└ value(2) ┘
restart 数组  00 00 00 00  0E 00 00 00               (0 与 14 → 两条记录起点)
packed footer 02 00 00 00                             num_restarts=2 = data 块数
trailer       00  5E A5 C5 E2                          type=0 + masked crc32c
```

校验和（与 §13.5.1 同法，对上面 40 B 内容续算压缩类型字节；已用 `util/crc32c.cc` 语义复核）：

| 步 | 输入 | 结果 |
|---|---|---|
| ① | `crc32c::Value(index 内容 40B)` | `0895875F` |
| ② | 续算 1 字节 `0x00`（`Extend(①,"\x00")`） | `5D432021` |
| ③ | `Mask(②)` | `E2C5A55E` |
| ④ | 落盘 5 B = 压缩字节 `00` ‖ ③ 的 LE32 | `00 5E A5 C5 E2` |

decode / seek：读 packed footer（内容末 4 B）→ num_restarts=2 → 回退读 8 B restart 数组（0、14）→
二分 restart 偏移 → 在 interval 内逐条前跳（shared=0 ⇒ 每条键全长在条目里，直接与 seek 目标 internal key
逐字节比，无需重组前缀）。FPGA 顺带收益：逐条读出的 `(offset,size)` 应**恰好排满**每个 data 块
（块 i 的 `offset+size+5` == 块 i+1 的 `offset`），可当一致性自检。

#### 14.3.3 key 与 value 的职责分工（index 键通常不是块的末键；value 是块的地址）

**常见误解：把条目键当成"它管的 data 块里最后一条 KV 的键"，把条目值当成"块里某条 KV 的
value"。两个都不对。** 这两者根本不在同一个概念平面上，各管一件事：

**key → 管"路由"（这条查询该进哪个块）**

- key 是**覆盖块内全部键的"上界代表"**，不是块内某条真实键的副本。写侧构造时它的取值空间是
  `[last_i, first_{i+1})`（§14.3），落在哪取决于能不能缩短：
  - **可缩短** → 虚拟键 `p+(c+1) ‖ (kMax, 0x18)`，**严格大于块 i 的真实末键**（虚拟 trailer 保证它不会
    撞上任何真实键，见 §14.3.1 要点 4）。例如 §14.3.2：entry0 键 `"c"@kMax` **不是**块0的末键——
    块0的真实末键是 `"b"@0x100`；`"c"` 只是用来给桶 `[前一键, "c")` 做右端点，把"末键 b 与后块首键 k
    之间的空档"具象成一个可比较的键而已。
  - **不可缩短**（两边界 user key 有公共前缀且无法进位）或该块是**末块**（默认 `kShortenSeparators`）
    → 退化为**块末键原样**。这才是"条目键 == 块末键"唯一成立的两种情形。
  - 一句话：不变式是 `S_i ∈ [last_i, first_{i+1})`，"== last_i" 只是该闭区间左端的一个特例。
- 读侧路由方向（易错，源码为准）：`IndexBlockIter::SeekImpl`（`block.cc:343-389`）与 data 块内 Seek
  **同一条代码路径**（`BinarySeekRestartPointIndex` 二分 restart + `FindKeyAfterBinarySeek` 前跳），
  语义都是 **lower-bound：停在第一条键 ≥ 查询 internal key 的条目**；Get 从命中的条目读 handle
  （`block_based_table_reader.cc:2542`）。因为键单调且 `S_{i-1} < first_i ≤ (块i内任意键) ≤ last_i ≤ S_i`，
  一个属于块 i 的查询目标必满足 `S_{i-1} < target ≤ S_i`，lower-bound 恰好落在**条目 i** 上 →
  读到的正是块 i 的 handle。target 越过全部 index 键时落在末块。
  - FPGA 收益：index 键数组 = 一份**现成、单调、可二分**的分桶表，定块只做纯 internal-key 比较，
    不需要任何"键→真实记录"映射；真正逐键搜索留到桶（data 块）内部用它的 restart 数组。

**value → 管"物理定位"（块在文件哪里、多大），与 KV value 同名异物**

| 出现位置 | 值是什么 | 指的单位 |
|---|---|---|
| index 条目 value | `BlockHandle` = `[varint64 offset][varint64 size]`（§14.3.1 要点 3） | **一个 data 块** |
| data 块内 KV value | 用户负载（§13.5 的 8 B） | 一条 KV |

- value 是 **data 块整块的地址**：`offset` = 该块内容在文件中的绝对字节偏移，`size` = 内容长度
  （不含 trailer）。它不是"块里某条记录的指针"，而是"整个块的门牌号"。
- 拿 handle 后的动作：按 `offset` 读出 `size` 字节内容 + 紧跟的 5 B trailer → 校验 type/CRC（§13.3）
  → 解压（基线 `kNoCompression` 直通）→ 进块内 restart 结构按 key 搜索（§13.5）。v2 的 index value
  不做 size-delta、不带 first-key（`format.cc:100-113`），所以定界**全靠该条目 record header 的
  `value_length`**，FPGA 读取时必须先解出它才能知道 handle 占几字节。

**两级二分全流程 = 先定块、再在块内定 KV。** 关键认知：文件里有**两套互不隶属的 restart 数组**——
index 块一套、**每个 data 块各自**一套。一次 point read（查 internal key T）依次在它们上面各做
**一次 restart 二分**，中间隔着一次 data 块载入，全程"二分 → 极短前跳 → 二分 → 极短前跳"：

```
查询键 T
  │  ① index 块 restart 数组上二分：定位第一条"键 ≥ T"的条目
  │     （lower-bound；index 条目键 = separator，见上）
  ▼
index 条目 i ── value ──▶ BlockHandle(offset,size)      ← 一级粒度：一个 data 块
  │  ② 按 offset 载入 data 块 i、验 CRC                ← SSD random read 发生在这里
  ▼
data 块 i 的 restart 数组上再二分 → 命中某 interval
  │  ③ 该 interval 内前跳 ≤ block_restart_interval 条 KV（默认16；本基线锁更小）
  ▼
命中的 KV（或判定不在本块）                              ← 二级粒度：一条 KV
```

- 两次二分是**同一种物理机制**：`BlockIter` 一族共用 `BinarySeekRestartPointIndex`（restart 数组
  二分）+ `FindKeyAfterBinarySeek`（interval 内前跳），data 块与 index 块走同一条代码路径
  （`block.cc`：index `IndexBlockIter::SeekImpl` 已见上文；data 同构）。差别只在"条目代表谁"：
  一级条目 = 一个 data 块、二级条目 = 一条 KV——正是 §14.3.1 粒度表的落地形态。
- 为什么块内不是"线性扫全块"：data 块常装几百条 KV，全扫就浪费了 restart 机制。二级二分只触碰
  O(log restart数) 个定位键 + 一个 interval 内的前几条。interval=1 时 ③ 只有 0~1 步 ≈ 块内直寻址。
- IO 分工：index 块通常常驻缓存，①不产生 SSD 读；真正的 random read 只在 ② 按 `BlockHandle` 载入
  **目标 data 块**时发生一次。FPGA 省 IO 的重点是让 ② 只读"目标块本身"（offset/size 精确给出的
  那段 + 5 B trailer），而不是读进整文件或多块再挑。
- 本基线的两条捷径：index `interval=1` ⇒ ① 无前跳、直接命中单条记录；data 块若也锁
  `restart=1` ⇒ ③ 退化为 0 步，二级也近乎"每 KV 一个 restart 点直接二分"（§14.6 锁定项）。

### 14.4 properties block：reader 打开文件的最弱要求 + engine 全量

块 = 属性名/值对，名按字节序排序（`meta_blocks.cc:198-212` KVMap）。**本 fork reader 在 Open 时必须
找到并解析 properties 块**（`block_based_table_reader.cc:1086-1092`，找不到即 corruption），并从其中取：

| 必须满足 | 属性键 | 本基线值 |
|---|---|---|
| `kIndexType`（**必须存在**，否则 Corruption，`reader:1140-1145`） | `rocksdb.block.based.table.index.type` | 4 B LE = `00 00 00 00`（kBinarySearch=0） |
| index 键含 seq | `rocksdb.index.key.is.user.key` | varint(0) |
| index 值 = 完整 handle | `rocksdb.index.value.is.delta.encoded` | varint(0) |
| 解压器选择（`reader:573-675`） | `rocksdb.compression` | `"NoCompression"` |
| 读侧一致性提示 | `rocksdb.data.block.restart.interval` / `rocksdb.index.block.restart.interval` | 与写侧一致（基线=1） |
| 比对/诊断 | `rocksdb.comparator` | `"leveldb.BytewiseComparator"` |

> 数值型属性一律 **varint64**（`Add(name,uint64)`，`meta_blocks.cc:65-70`）；布尔即 varint(0/1)。
> `kIndexType` 是 user-collected 属性，值是 **PutFixed32 4 B LE**（`block_based_table_factory.cc:168-171`）。
> 其余（num.entries、raw.key.size、column.family.*、db_session_id…）对本 fork 的读取**非必需**，
> 但推荐按 `PropertyBlockBuilder::AddTableProperty`（`meta_blocks.cc:79-196`）全量补齐，使
> `GetTableProperties`/compaction/统计等路径所见一致 —— **本节字节最适合放在 host 侧拼装**（§16.5）。下面三小节给出 properties 的**字节级视图**与 **FPGA 写侧字段分工**（§16.5 分工表落到字段级）。

#### 14.4.1 字节级结构：标准 block、单 restart、三类值编码

properties 内容本身是**标准 block**（块尾三件套同 §13.1-13.3），唯一直径差异是 restart 间隔：
`PropertyBlockBuilder` 用 **restart interval = INT_MAX**（`meta_blocks.cc:53-57`），故整块**只有一个
restart 点（第 0 条记录，偏移 0）**：

```
properties 内容 = 记录流 ‖ [restart 数组：仅 1 项 u32 LE = 00 00 00 00] ‖ [packed footer u32 = 01 00 00 00]
trailer 5 B = 00 ‖ masked crc32c（同 §13.3；被校验字节 = 内容 + 压缩类型 0x00）
```

记录流条目 = §13.1 标准记录，**键被前缀增量编码**：

```
[varint shared_bytes] [varint non_shared_bytes] [varint value_length]
[键的"尾段"non_shared 字节] [值的 value_length 字节]
```

- 只有**第 0 条**（全块唯一 restart 点）是 `shared=0`、存完整键名；
- 后续条只存"上一条键砍掉 shared 前缀后的尾段"，完整键由 reader 用上一条键补回（§14.4.2）；
- 键 = 属性名**原文字节串**；不是 internal key，无 8 B seq/type 尾（区别于 §14.3 的 separator 键）。

**值有四类编码，别混**（写侧 `meta_blocks.cc:65-76`；读侧 `ParsePropertiesBlock` 分派
`meta_blocks.cc:351-441`）：

| 值类型 | 编码 | 例（基线） | 读法 |
|---|---|---|---|
| 预定义 uint64（`num.entries`/`data.size`/`format.version`…） | **varint64** | `02` | `GetVarint64` |
| 布尔（`index.key.is.user.key`） | varint64 `00`/`01` | `00` | `GetVarint64` |
| 字符串（`comparator`/`compression`/`creating.*`） | 原始字节（定界靠 `value_length`） | `"NoCompression"` | 原样 |
| user-collected `kIndexType` | **fixed32 4 B LE** | `00 00 00 00` | 原样存 `user_collected`，Open 时 `DecodeFixed32`（`reader:1150`） |
| user-collected `whole/prefix.filtering` | ASCII `"1"`/`"0"` | `31` | 原样 |

> 易错点：数值一律 varint64；唯独 `kIndexType` 是 fixed32；布尔"flag"又写成 ASCII `"1"/"0"`（非 varint）。

#### 14.4.2 记录顺序、键的物理形态、读取流程

**顺序 = 键全名字节升序。** `PropertyBlockBuilder` 用 `std::map` 收纳（`meta_blocks.cc:62`），`Finish()`
依 map 序逐个入块（`:204-214`，Add 先后无关）；reader 逐条校验严格递增，否则
`Corruption("properties unsorted")`（`:372-378`）。引擎默认（无 filter/ts/二级索引）确定性记录的
真实字节序（`LC_ALL=C` 排序）：

```
rocksdb.block.based.table.index.type          ← 整块第 1 条（shared=0，存全键）
rocksdb.block.based.table.prefix.filtering
rocksdb.block.based.table.whole.key.filtering
rocksdb.column.family.id
rocksdb.comparator
rocksdb.compression
rocksdb.creation.time
rocksdb.data.block.restart.interval
rocksdb.data.size
rocksdb.deleted.keys
rocksdb.file.creation.time                    ← 仅当 >0
rocksdb.filter.size
rocksdb.fixed.key.length
rocksdb.format.version
rocksdb.index.block.restart.interval
rocksdb.index.key.is.user.key
rocksdb.index.size
rocksdb.index.value.is.delta.encoded
rocksdb.key.largest.seqno
rocksdb.key.smallest.seqno                    ← 仅当文件非空
rocksdb.merge.operands
rocksdb.newest.key.time
rocksdb.num.data.blocks
rocksdb.num.entries
rocksdb.num.filter_entries
rocksdb.num.range-deletions
rocksdb.num.uniform.blocks
rocksdb.oldest.key.time
rocksdb.original.file.number
rocksdb.raw.key.size
rocksdb.raw.value.size
rocksdb.tail.start.offset
rocksdb.user.defined.timestamps.persisted
```

都带 `"rocksdb."`（8 B）前缀，故实际比较的是后缀段字节序；`block.based.*` 因 `b` 最小恒排最前。
用户收集器加的任意名（不必 `rocksdb.` 前缀）按同样规则插位排序。

**每条记录的 key（两种视角）：**
- **逻辑键** = 属性名完整字符串（reader 重建后见到的键）。
- **物理盘上键**：仅第 0 条存全名；第 k 条只存尾段，重建 = **上一条完整键的前 `shared` 字节 + 本条
  `non_shared` 尾段**。

前三条示例（键长 36/42/45，值 = fixed32 / `"0"` / `"1"`）：

```
record0  00 24 04 | rocksdb.block.based.table.index.type(36B) | 00 00 00 00
record1  1A 10 01 | prefix.filtering(16B)                     | 30     ← 前 26B 复用 record0（…table.）
record2  1A 13 01 | whole.key.filtering(19B)                   | 31     ← 前 26B 复用 record1（…table.）
          └varint×3┘└──────── 键尾段(仅 non_shared) ────────┘└─值─┘
restart 数组  00 00 00 00 ；packed footer  01 00 00 00
```

即 record1 的键在盘上只有 16 B，解它必先解出 record0 的前 26 B 再拼 → 这是"只能顺序读"的根因。

**读取某条记录的流程（两级）：**
- **A. 经 metaindex 定位 properties 块（可二分）**：footer → `metaindex_handle` → 读 metaindex 块。
  metaindex **restart interval = 1**（`meta_blocks.cc:44`）→ 每条存全名 → 可 restart 数组二分：
  `meta_index_iter->Seek("rocksdb.properties")` + 判等（`FindOptionalMetaBlock`，`meta_blocks.cc:580-595`）
  → value = 完整 BlockHandle（`DecodeFrom`）。找不到 → `Corruption`（`reader:1088-1092`）。
- **B. 块内取某记录（只能顺序）**：按 handle 读内容 + 5 B trailer、验 CRC → `ParsePropertiesBlock` 用
  `MetaBlockIter`：`SeekToFirst()` 起**逐条 `Next()`** 重建键，直到键 == 目标（键有序可提前终止）。
  **单 restart ⇒ 无法像 data 块那样二分跳中**——读第 k 条须先解 0..k−1 条。解出的值按 §14.4.1 表分派。

> FPGA 读已有文件的要点：metaindex 可二分；properties 必须从头顺序拼前缀扫（它极小、常驻缓存，
> 顺序读代价可忽略——引擎正是为此选单 restart）。

#### 14.4.3 FPGA 产 properties：三类字段分工 + 几何公式 + 收尾

compaction 输出文件的 properties **描述输出文件自身，不是输入**；三类字段由不同方负责
（§16.5 分工表在此落到字段级）：

| 类别 | 谁决定/生成 | 责任 |
|---|---|---|
| **① 解码硬开关** | FPGA（按实际写入）+ host 落盘 | `index.type`（fixed32；缺失即 Corruption）、`index.key.is.user.key`=0、`index.value.is.delta.encoded`=0、`compression`=`"NoCompression"`、两个 `restart.interval` = FPGA 真用的间隔、`comparator`=`"leveldb.BytewiseComparator"` |
| **② 输出几何/统计** | FPGA 累计计数器，host 套公式 | `num.entries`、`raw.key.size`、`raw.value.size`、各 type 计数、`num.data.blocks`、`data.size`、`index.size`、`key.{smallest,largest}.seqno`、`tail.start.offset`、`format.version`=2、filter/uniform/fixed=0 |
| **③ DB 运行时元数据** | host 喂给拼装器 | `original.file.number`（新文件号）、`column.family.id/name`、`creating.{db,session,host}.identity`、`creation.time`/`file.creation.time`/`oldest/newest.key.time` |

**② 的几何公式**（令 index 内容长 I、内容起点文件偏移 `O_idx`；无 filter 时 properties 是第一个 meta 块）：

| 属性 | 公式（引擎赋值点） | 说明 |
|---|---|---|
| `data.size` | `O_idx`（`builder:1875` 每 data 块后取 offset） | = Σ(每 data 块内容 + 5 B trailer) |
| `index.size` | `I + 5`（`builder:2507` = `IndexSize()+kBlockTrailerSize`） | 含 trailer |
| `tail.start.offset` | `O_idx + I + 5`（`builder:2887` Flush 后取 offset） | = properties 内容起点 |

**② 的流式计数器**（FPGA 写输出时累加，锚点 = 引擎增量点）：`num.entries`（`:1642`）、
`raw.key.size` += 每条 internal key 字节含 8 B seq/type trailer（`:1643`）、`raw.value.size` += value
（`:1647`）、`num.deleted.keys`/`num.merge.operands`/`num.range-deletions` 按 type 字节（`:1650-1655`）、
seq min/max（`:1569-1570`）。

**记录条数**：确定性核心 ≈ 33 条（§14.4.2 清单；`file.creation.time`/`key.smallest.seqno` 视空文件
与否增减）；再叠加用户收集器、二级索引（+2：`index.partitions`/`top-level.index.size`）、filter（+1：
`filter.policy`）。**FPGA 不许照抄输入文件的数值**；唯一"沿用"的是 comparator/CF 这类引擎不变式。

**收尾形态（建议 host 做）**：host 用引擎自己的 `PropertyBlockBuilder` 把 ①+②+③ 字段在内存拼成
单 restart + 前缀增量块 → 算 5 B trailer → 写在 `tail.start.offset` → metaindex（restart=1、排序）
登记 `"rocksdb.properties"` → footer（index/metaindex handle）。**FPGA 不必实现前缀增量/CRC**；若确实
要 FPGA 全 offload，须自实现单 restart+delta+CRC-32C mask，并把 ③ 运行时字段逐文件下发——首版不建议。

**byte-exact 边界**：properties 含 `creating.*`/时间戳等**跨运行不稳定**字段；compaction 输出要与
"另一时刻 CPU 引擎"逐字节一致时，host 须喂入引擎当时会用的同一组运行时值；否则别把 properties 纳入
byte-exact 断言（呼应 §16.5 = 宿主拼装，天然与引擎字节一致）。

#### 14.4.4 补遗一：`*.block.restart.interval` 记录存哪、谁消费、为何写实值

`rocksdb.data.block.restart.interval` / `rocksdb.index.block.restart.interval` 两条记录存的是**写侧构造
data 块 / 顶层 index 块时真正使用的 restart 间隔**。要点：**块自身从不记录自己的 interval**——任何 block
只落盘 restart *偏移数组*（§13.1），间隔是纯生成期参数、解码不需要它。因此这两条 properties 记录是
间隔的**唯一持久化载体**：

```
写: 选项 table_options.{block_restart_interval(默认16, table.h:351), index_block_restart_interval(默认1, table.h:354)}
      └→ 拷进 TableProperties(builder:1370-1372)
      └→ AddTableProperty 仅当 >0 写记录, 值 varint64(meta_blocks.cc:184-191)
      └(同一份值同时送 index_builder:36-41, 决定索引块 restart 打点)
读: ParsePropertiesBlock 命中 predefined 表 → GetVarint64 还原(meta_blocks.cc:354-357)
      └→ Open 拷回 rep_->{data,index}_block_restart_interval(reader:1109-1112)
      └→ 注入 BlockCreateContext(reader:866-871) → 解析 index/filter 块对象时用(block_cache.cc:24)
```

读侧并非"必须"——缺失也能 Open（按 0 处理），但会影响 index/filter 块对象化、均匀块与缓存等读侧语义/
统计；引擎侧因两值恒 >0 必写。**FPGA 意义**：这两条必须 = FPGA 拼块时真用的间隔，不是抄输入文件，
而是与"块内 restart 数组实际形态"自洽（§14.4.3 ①、§14.6 锁定 index=1）。若 interval 与 restart 数组
矛盾，文件仍可能被打开，但块二分语义与 statistics 失真——自相矛盾文件。

#### 14.4.5 补遗二：值的写/读编码分派（三档 + 两条特例）

写侧一切值最终都进 `std::string` map（`meta_blocks.cc:59-63`），"类型"只活在产生代码与读侧分派。
物理编码**只有三档**；ASCII `"1"/"0"` 是档②字符串的特例、不是独立一档：

| 档 | 编码 | 成员 | 写点 |
|---|---|---|---|
| ① 数字/结构体布尔 | **varint64** | 全部 uint64 属性 + `TableProperties` 里 bool（`index.key.is.user.key`/`index.value.is.delta.encoded` → varint `00`/`01`） | `Add(name,uint64)` `meta_blocks.cc:65-70,92-94` |
| ② 字符串 | 原始字节 | `db.id`/`db.session.id`/`db.host.id`/`column.family.name`/`comparator`/`compression`/`merge.operator`/`prefix.extractor`/`property.collectors`/`filter.policy`/`compression.options`/`seqno.to.time.mapping` | `Add(name,string)` `meta_blocks.cc:136-177` |
| ③ user-collected 特例 | **fixed32 4 B LE**（仅 `index.type`） | `builder:168-171` |  |
| ③' user-collected 特例 | ASCII `"1"`/`"0"`（仅 `whole.key.filtering`/`prefix.filtering`；0x31/0x30） | `builder:172-175` |  |

**读侧分派（`ParsePropertiesBlock`，`meta_blocks.cc:286-434`，逐条顺序扫），先看键再定吃法**：
1. 键 ∈ predefined_uint64 表（`meta_blocks.cc:292-360`，含两个 bool）→ **`GetVarint64`** 还原成类型化
   字段；其中 `deleted.keys`/`merge.operands` 另镜像进 user_collected（API 兼容，`:388-393`）。
2. 键 ∈ 上述字符串白名单 → `raw_val.ToString()` 进 `comparator_name`/`compression_name` 等 string 字段
   （`:406-429`）。
3. **其余一律**原样进 `user_collected_properties`（`:430-433`）——`index.type`、`whole/prefix.filtering`、
   以及任意自定义 user collector 名都落这。
4. Open 后再**特判** user_collected 里三个名：`index.type` → **`DecodeFixed32`**（缺→Corruption，
   `reader:1141-1147`）；`whole.key.filtering`/`prefix.filtering` → **字符串判等 `=="1"`**
   （`IsFeatureSupported`，`reader:477-491,1128-1133`）。

闭环即"写什么型读什么型"：写 fixed32 ↔ 读 `DecodeFixed32`；写 ASCII ↔ 读字符串判等——**ASCII 从不当
varint 布尔解**。两条易混分开记：结构体 bool 字段序列化是 varint(0/1)（档①）；ASCII `"1"/"0"` 只出现在
user_collected 的两个 filtering flag（档③'）。给 FPGA 的读表：**预定义名查 varint、`index.type`
DecodeFixed32、filtering 判等 `"1"`、其余当字符串**；写侧对称，唯一要特编的是 `index.type`(fixed32) 与
filtering(ASCII)，数字照抄 varint、字符串照抄字节。另注意成员多为**条件存在**（空串/0/`UINT64_MAX`
守卫，`meta_blocks.cc:88-191`），别按固定条数硬凑（呼应 §14.4.2/§14.4.3 的计数说明）。

### 14.5 metaindex block

块条目 = `meta名 → BlockHandle`，名按字节序排序（`meta_blocks.cc:38-49`）。v2 引擎写侧把 handle 放 footer，
**不**在 metaindex 记 `rocksdb.index`（那仅 v≥6，`block_based_table_builder.cc:2491-2495`）。
最小文件只需一条：`"rocksdb.properties" → properties_handle`。
下面三小节给 metaindex 的**字节级结构**；与 §14.4.1 的 properties 对照：properties 是"单 restart、只能顺序读"，
metaindex 是"**每条一个 restart、可二分**"——它是 SST 尾部的"元块目录"。

#### 14.5.1 字节级结构：标准 block、interval=1、每条全键、值=BlockHandle

metaindex 内容本身是**标准 block**（块尾三件套同 §13.1-13.3），唯一差异是 restart 间隔：
`MetaIndexBuilder` 用 **restart interval = 1**（`meta_blocks.cc:36`）→ **每条记录都是 restart 点、键完整存放、
不做前缀增量**（每条可独立定位/比较 → 块内可二分）。写盘强制不压缩（`builder:2908` 传 `kNoCompression`）。

```
metaindex 内容 = 记录流 ‖ [restart 数组：每条记录一个 u32 LE，按偏移升序，首项恒 00 00 00 00]
                ‖ [packed footer u32 = num_restarts(=记录条数, 低 28 位; 高 4 位=0)]
trailer 5 B = 00 ‖ masked crc32c（同 §13.3；被校验字节 = 内容 + 压缩类型 0x00）
```

单条记录 wire（标准记录，§13.1）：

```
[varint shared=0] [varint non_shared=键长] [varint value_length]
[键：元块名全文，原文字节串] [值：BlockHandle = varint64(offset) ‖ varint64(size)]
```

- restart=1 ⇒ 每条记录 `shared` **恒为 0**，键与值（BlockHandle）都非增量 → 每条完整自洽，可从 restart
  数组任意落点直接开始解。
- `value_length` 用于切分**不定长**的 handle 字节；BlockHandle 无内部长度前缀，固定两段 varint
  （`BlockHandle::EncodeTo` = `PutVarint64Varint64`，`format.cc:48-53`）。
- restart 数组第 i 项 = 第 i 条记录起点在流内字节偏移；`num_restarts` = 记录条数（首条由预置 `[0]`
  起步，之后 interval=1 → 每条一 push，`block_builder.cc:129,294-298`）。

**具体字节（baseline 单条；offset/size 为示意值 `AC 02`=300 / `C8 01`=200）**：

```
record0    00 | 12 | 04 | rocksdb.properties(18 B) | AC 02 C8 01
           │   │    │    └───────────┬────────────┘ └── varint64(300)+varint64(200) ──┘
        shared=0 键=18 值长=4          键=元块名全文          值=BlockHandle(varint 对)
restart 数组  00 00 00 00             ← 首条起点偏移 0（interval=1 ⇒ 每条一个 restart）
packed footer 01 00 00 00             ← num_restarts=1（低 28 位）
trailer       00 ‖ crc32c(内容+00)
```

内容 1+1+1+18+4 + 4 + 4 = 33 B，另加 5 B trailer。两条及以上的文件：restart 数组 =
`[0, 第 2 条起点, …]`，每记录一项，`num_restarts` = 条数。

#### 14.5.2 键（元块名）与值的字段语义

| 字段 | 用处 |
|---|---|
| `shared` varint | 通用增量头；interval=1 时恒 0、占 1 B |
| `non_shared` varint | = 键长；决定键段读多少字节 |
| `value_length` varint | 切 value 段（BlockHandle 无内部长度前缀，靠它定界） |
| 键段 = 元块名 | **查找键**：reader 要哪个元块就 binary-Seek 哪个名（bytewise comparator，`reader:1506`） |
| 值段 = BlockHandle | **文件定位**：`offset`=元块内容首字节的文件内字节偏移、`size`=内容长度(不含 5 B trailer)；据此抓内容 + trailer 验 CRC |
| restart 数组 + `num_restarts` | 每条一个 restart → 二分跳表；`num_restarts` = 记录条数 |
| trailer `00‖crc32c` | 完整性；类型字节 = 0（写侧强制 kNoCompression） |
| （块外）footer.metaindex_handle | 指向本块的入口 handle（v2 的 footer 直接存它） |

**可能出现键**（全名按字节序升序；存于 `KVMap`=`std::map`，`meta_blocks.cc:41`/`meta_blocks.h:54`）：

| 键 | 指向 | 基线(v2, 无 filter) |
|---|---|---|
| `fullfilter.<CompatibilityName>` | 全量 bloom/ribbon filter 块（无 `rocksdb.` 前缀 → 排序在 `rocksdb.*` 之前） | 无 |
| `partitionedfilter.<CompatibilityName>` | 分区 filter 顶层块 | 无 |
| `rocksdb.compression_dict` | 压缩字典块 | 无 |
| `rocksdb.index` | 索引块 handle（**仅 format_version ≥ 6**） | 无（v2 放 footer） |
| `rocksdb.properties` | 表属性块 | **必有一条**（缺 → Open Corruption） |
| `rocksdb.range_del` | range tombstone 分片块 | 通常无 |

> 名字常量 `meta_blocks.cc:29-33`；filter 前缀 `builder:3059-3061`。多条时记录间相对顺序即上表列序。

#### 14.5.3 读取、与 properties 差异、format_version 注记

**读取某条元块（两级）**：footer → `footer.metaindex_handle()` → 取内容 + 5 B trailer 验 CRC
（`ReadMetaIndexBlock`，`reader:1483-1509`）→ `NewMetaIterator`（bytewise）→ 按名字 binary-Seek
（restart 数组二分；interval=1 ⇒ 命中即该条）+ 判等 → `DecodeFrom` 解出 BlockHandle
（`FindOptionalMetaBlock`，`meta_blocks.cc:580-595`）→ 再按该 handle 独立抓目标元块（properties 块内
顺序扫见 §14.4.2 Stage B）。

**与 properties 的差异**：

| | properties（§14.4） | metaindex（本节） |
|---|---|---|
| restart interval | INT_MAX（单 restart 点） | **1（每条一个 restart）** |
| 键前缀增量 | 是（只第 0 条存全名） | **否（每条存全名）** |
| 值语义 | 类型化标量（varint64/fixed32/ASCII/字符串，§14.4.5） | **固定 BlockHandle 二元组** |
| 块内读取 | 只能从头顺序拼前缀 | restart 数组二分直达 |
| 读侧必需性 | 缺整块 → Corruption | 只按名字查，个别元块缺失可容忍（唯 properties 必需） |

**format_version 注记**：v2 下索引 handle 放 footer、**不放** metaindex
（`FormatVersionUsesIndexHandleInFooter(2)=true`，`format.h:215-217`；`builder:2492-2494` 因而跳过
`Add(kIndexBlockName)`）。仅 **format_version ≥ 6** 才把索引 handle 挪进 metaindex 的 `rocksdb.index`
条目、footer 不再自带 index_handle。**基线（v2）metaindex 通常只有 `rocksdb.properties` 一条** →
host/FPGA 拼装时该块形态固定、零偏移分叉风险（呼应 §16.5）。

### 14.6 写侧必须锁定的配置（FPGA/宿主两侧一致，否则不字节可预期）

```
BlockBasedTableOptions: {
  format_version            = 2
  checksum                  = kCRC32c   (注意：本 fork 默认是 kXXH3，必须显式覆盖)
  index_type                = kBinarySearch   (无 kBinarySearchWithFirstKey/kTwoLevel)
  filter_policy             = nullptr         (无 bloom/ribbon)
  compression               = kNoCompression
  use_delta_encoding        = 写侧自由；基线 restart=1 → 共享前缀恒 0
  block_restart_interval    = 1   (data；最小实现成本)  或 16（对齐引擎默认，需前缀压缩逻辑）
  index_block_restart_interval = 1
  block_align / super_block_alignment_size / uniform_cv_threshold = 关(0/-1)（不产生填充位/均匀位）
  enable_index_compression  = 无影响(kNoCompression 下恒原样) 
}
```

> 读侧无需任何特殊配置：reader 由 footer 自适应 format_version / checksum / magic，索引类型取自
> properties。即 FPGA 产出的 v2+crc32c 文件可被本引擎**默认参数打开**。format_version=2 也被
> RocksDB ≥3.10 支持，兼容面最宽（本 fork 读下限=2，`table/format.h:172-180`）。

### 14.7 键序与去重语义（写 data 块前）

- 块内/internal key 排序 = 用户键升序、同键 seq 降序（与 §4 排序规则一致，即 memtable 序）。
- 同一 user key 的多版本按（seq 降序）流式扫到 → **只输出 seq 最大的一条**（较老版本被遮蔽）；
  若最高版本是 kTypeDeletion，SST 仍写这条 tombstone 记录（value 为空）还是丢弃，取决于物化边界，
  需与引擎的 compaction 语义对齐（建议先做"保留每条 seq 最大者、删除其余"）。

### 14.8 读取全流程（Open bootstrap + 单次 Get）

> 前提：文件按 §14.6 配置产出（v2 / crc32c / kBinarySearch 整索引 / 无 filter / 无压缩）。
> "读数据"分两个时间尺度——(A) 每文件**打开一次**的 bootstrap（把"怎么读"全部常驻内存）；
> (B) 之后**每次点查 Get** 才真正去 data block 翻 KV。二者共享 §13-§14.5 的全部字节格式。

```
┌─ (A) Open：每文件一次（BlockBasedTable::Open, reader:734）─────────────────┐
│ 尾部 prefetch → ① footer(v2: magic/format/crc + metaindex_handle + index_handle)│
│ → ② 读 metaindex(restart=1, 可二分) → ③ 读 properties(取 index.type 等解码开关)    │
│ → ④ 配解压器/设 rep_ 语义位 → ⑤ 建 IndexReader、读入顶层 index block(常驻)         │
└──────────────────────────────────────────────────────────────────────────┘
┌─ (B) 每次点查 Get(key)（reader:2489）─────────────────────────────────────┐
│ bloom 探测(若命中"不在"→ NotFound, 零 data IO；基线无 filter 则恒过)             │
│ for (IndexBlockIter.Seek(key); valid; Next()) {      ← 第一级：index 二分      │
│   取 IndexValue: separator(上界) + data BlockHandle(offset,size)              │
│   边界预判(entry 首键已 > 目标)? → 判不存在 break                                │
│   data block = cache ? : 读 offset+size+5B trailer → 验 CRC → 解压            │
│   DataBlockIter.SeekForGet(key)                     ← 第二级：块内 restart 二分 │
│   区间内前缀解码线性扫 internal key → 命中版本? 取 value / tombstone / 缺         │
│   未覆盖 → iiter->Next() 下一条 index entry 再试                               │
│ }                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

#### (A) Open bootstrap：把文件"读法"固化进 rep_

1. **尾部 prefetch**（`PrefetchTail`, `reader:996`）：从文件尾向前读一大块（tail_size 或启发式），
   一次 IO 覆盖 footer/metaindex/properties/index 所在的 meta 区。
2. **footer 解码**（`ReadFooterFromFile`, `reader:801`）：文件最后 ~53 B；验 magic
   `0x88e241b785f4cff7` 与 format_version，得 **`metaindex_handle` + `index_handle`**（v2 两者都在
   footer，§14.5 注记）。
3. **读 metaindex 内容**（`reader:842` → `ReadMetaIndexBlock` `reader:1483`）：按
   `footer.metaindex_handle` 取内容 + 5 B trailer、验 CRC；生成 MetaIterator（bytewise）。
4. **读 properties 块**（`reader:850` → `ReadPropertiesBlock` `reader:1081`）：
   `FindOptionalMetaBlock(metaindex, "rocksdb.properties")`（§14.5：metaindex 单 restart 可二分拿
   handle）→ 抓内容 → `ParsePropertiesBlock` 顺序扫，解出全部**解码开关**（§14.4.5 分派）：
   `index.type`(kIndexType，缺失即 Corruption)、`index.key.is.user.key`、`index.value.is.delta.encoded`、
   两个 restart.interval、`compression`、`comparator`…
5. **按属性配置读侧**：`compression_name` 配解压器（`reader:856-860`）；properties 各值拷进 `rep_`
   语义位（`reader:1109-1138`）；`SetupBaseCacheKey` 稳定 cache key（`reader:947`）。
6. **建索引 reader**（`PrefetchIndexAndFilterBlocks` `reader:961`/`:1213` → `CreateIndexReader`
   `reader:2998`）：读顶层 **index block**（`footer.index_handle`）构造 `Block_kIndex` + IndexReader，
   此后 `NewIndexIterator` 直接用（断言 `index_reader != nullptr`，`reader:1676`）；filter 存在时于此登记。

结束后 `rep_` 持有 footer / properties / metaindex / **完整 index block**——每查询只剩"定位并读
data block"。

#### (B) 单次点查：`BlockBasedTable::Get`（`reader:2489`）

入参是 **internal key**（user key + seq + type，§14.7）。逐层：

1. **入口剪枝**：`TimestampMayMatch`（`reader:2495`）。
2. **filter 探测**（`FullFilterKeyMayMatch`，`reader:2518-2521`/def `:2325`）：whole-key bloom/ribbon
   判"绝不在本文件"→ 直接 NotFound，**零 data IO**。基线 filter_policy=nullptr → 恒 may_match → 下钻。
3. **第一级：index 二分**（`NewIndexIterator` `reader:2530`/`:1671` → `iiter->Seek(key)` `reader:2542`）：
   `IndexBlockIter::SeekImpl`（`block.cc:343-389`，**lower-bound**）在 index block restart 数组上二分落
   区间 → 区间内解码命中条目（§14.3.3 第一步）。index 块已常驻，纯内存。
4. **取 `IndexValue`** = separator（data 块上界键）+ `v.handle`（BlockHandle：文件内字节 offset/size，
   单位字节、不含 5 B trailer）+ first_internal_key（边界用）。
5. **边界预判**（`reader:2545-2553`）：若 entry 首 internal key 的 user key 已 > 目标 user key →
   key 落在上一块与本块之间（后文更不会有）→ `break` → NotFound。
6. **取 data block**（`NewDataBlockIterator` `reader:2563`）：先查 block cache（`GetDataBlockFromCache`
   `reader:1531`）→ miss 走 `RetrieveBlock` `reader:2090`：按 handle **读 offset+size 内容 + 5 B
   trailer → 验 CRC → 依 trailer type 字节解压**（基线 kNoCompression）→ 依 restart 数组解析成
   DataBlock 对象 → 回填 cache。
7. **第二级：块内定位**（`biter.SeekForGet(key)` `reader:2583` → `DataBlockIter::SeekImpl`
   `block.cc:181`）：data 块 restart 数组二分落区间 → 区间内**前缀解码线性扫** internal key
   （§14.3.3 第二步）。比较 = 用户键升序、同键 seq 降序（§14.7）。
8. **命中/汇聚**（`reader:2594-2619`）：每条 internal key `ParseInternalKey` 分解 user key/seq/type →
   `get_context->SaveValue`：
   - 命中 seq ≤ snapshot 的最高版本是 value → 取 value、Found；
   - 最高版本是 tombstone 或块内无此键 → 由 GetContext 决定 NotFound/继续；
   - 一旦定案 → `SaveValue` 返回 false → short-circuit，不再读多余块。
9. **未覆盖则推进**：index entry 的键是**上界 separator**（§14.3.3）——目标可能落在当前 entry 之后 →
   `iiter->Next()` 取下一条 index entry 回到第 3 步（外层 for，`reader:2542`）。
10. **结束**：本文件级结论（Found/NotFound/需继续合并）交回 DB 层，由其跨 memtable、L0..Ln、多 SST
    汇总（版本读逻辑在 table reader 之上，不属于本文件格式）。

#### 一次命中最简 IO 账（基线文件）

- index block：Open 已读、常驻 → 二分纯内存；
- data block：cache 命中零 IO；**冷读 = 1 次按 handle 的块读 + CRC + 解压**；
- filter（若有）判"不在"→ 0 次 data IO。

即冷点读的存储访问通常 = **1 个 data block**。扫描/compaction 走同一 index→data 寻址，逐块顺序迭代，
只是免去 filter 探测。FPGA 若直接按 §14.6 产出文件，上述路径无需任何改动即可被本引擎 reader 打开。

---

## 15. WAL 整体文件布局（分区日志 + ZFPROPS）

> 帧格式已见 §10。本节回答"文件这一层长什么样、offset 从哪来、几份文件怎么组织"。

### 15.1 目录与命名

- 日志根目录 = `zeroflush_db.h:48` 的 `wal_subdir = "zfwal"`，物理目录 `<DB>/zfwal/`。
- **每 (分区, 代) 一个文件**：`zf-wal-<part>-<gen>.log`（`wal_manager.cc:25-27`）。
- 全局元数据独立文件 `<DB>/zfwal/ZFPROPS`（`zeroflush_db.cc:1169`），**不在**日志文件内部。

### 15.2 单个日志文件 = ZfRecord 的纯拼接

```
zf-wal-<part>-<gen>.log
┌──────────────────────────────┐  ← 文件头 offset 0（无文件头/无 magic/无分块）
│ ZfRecord[0]  header24+key+val+crc4 │   记录可跨 4KB，无对齐要求（wal_format.h:9）
│ ZfRecord[1]                     │
│  …（同 gen 内按落盘序追加）        │
└──────────────────────────────┘
```

- `wal_offset`（SlimLocator）＝ 该条 **ZfRecord 帧首**在此文件内的**精确字节偏移**，从 0 起。
- 无 32 KB 物理块分层、无 block checksum、无 padding 到扇区/页（`wal_format.h:4-9`）：
  active 段的 4 KB flush（`Partition` 的 `flushed_size`，`wal_manager.h:178-193`）只是**主机写缓冲**，
  不是格式。
- gen 递增与 freeze 由 `wal_manager.cc` `Freeze` 完成：登记进 `SealedFileCache` 后该文件**不可变**、
  可按 offset 随机读（`ReadFromSealed`）。
- 因此对 FPGA：**封存段的 value 读取 = 读一个纯 ZfRecord 流**，天然适合"整段顺序搬入缓冲后按 offset
  下标取帧"（见 §9.3、§11 形态 B）。数据仍在活跃代（gen==active）时尾部在内存，非只读目标。

### 15.3 跨分区/代序 与 ZFPROPS

- seq **显式落盘**（header 16..24），恢复/全局归并不依赖文件顺序（`wal_format.h:8`）；
- 每个表的路由/边界/版本记录在 ZFPROPS：v1 16 B 定长（`wal_format.h:74-89`）；v2 变长
  （`wal_format.h:100-115`：`'ZFP2'|version=2|routing_mode|comparator_name|每表(version,partitions,
  part_ids[],boundaries[])|current_version|crc32c`）。v2 与 v1 都以 magic 区分，FPGA 物化侧若需
  知道"分到了哪些分区、边界键是什么"应解析 v2（解码 `DecodeZfPropsAuto`，`wal_format.cc`）。

---

## 16. 输入 / 输出接口设计（slim memtable 进、WAL 进、SST 出）

> 面向 SmartSSD/直连 NVMe 拓扑，给出每路接口的形态、缓冲、回压与推荐方案。

### 16.0 数据流总览（三条流）

```
[A] slim memtable(冻结段, host DRAM) ──PCIe──► [sort kernel]
                                              │  输出 = 按 §4 全序的内部键流（含 16B locator）
[B] 封存 WAL 段(SSD) ────NVMe/DMA──────────► [value 解析：base+wal_offset → ZfRecord]
                                              │
                                              ▼  合并/物化（同键去重，§14.7）
[C] SST 流式写出 ─────────────────────────► NVMe 文件 <编号>.sst
```

三路用**队列 + 信用量回压**衔接，不共享锁；FPGA 侧每路配独立 DMA 通道与环缓冲。

### 16.1 [A] slim memtable 输入：优先"宿主线性化整批 DMA"，不做逐指针追链

- **不要**让 FPGA 对着 arena 虚拟地址逐个 `load64` 追 level-0 指针（跨 4 KB arena 块、VA 需页表、
  链式延迟高，§7）。
- 推荐：**宿主侧按 partition 的 level-0 全序，把条目拷贝成连续字节流**填入 DMA 提交环。
  条目本身已是自描述 `[varint ik_len][internal key][varint16][locator]`（§3），零重组直接 memcpy；
  每批携带（起始指针、字节长、条目数、校验）。freeze 只读引用计数保证拷贝期安全（§7.2）。
- FPGA 收到即**流式 parse**（varint 解码 + footer→seq），喂给排序核；单批一条 `desc {dst_iova, len,
  seq_first, seq_last, part_gen 集}` 便于按 (part,gen) 分组、预留 WAL 读取窗口。
- 缓冲/回压：DMA 双环（pending/done），宿主侧每提交一批发 doorbell；FPGA 消耗完发完成+credit。
  建议批粒 64 KB–1 MB，条目宽 ~24–几百 B，解析吞吐不再受内存往返限制。

> 若必须省掉这一次 host 拷贝（零拷贝红线），备选：给 FPGA 共享 pin 住的 arena 段表
> （VA→IOVA scatter list），由 FPGA 对每分区发 **gather DMA** 直读各 4 KB 块、本地拼有序流 —— 复杂度高，
> 收益仅在拷贝带宽成为瓶颈时才有意义，基线不推荐。

### 16.2 [B] WAL 输入（取 value）：整段顺序搬移优先，逐条随机读禁用

- 物理事实：同 (part,gen) 段内记录是**写入序**，与全局键序无关；逐 locator 随机读必然打乱 NVMe 顺序。
- **形态 B（推荐，与物化现有路径一致）**：把封存段 `zf-wal-<part>-<gen>.log` **整段一次顺序读入**
  FPGA 可见内存（SmartSSD 上用卡的 HBM/CMEM），写入段描述符表 `(part,gen)→base`（§11），此后
  `val = base + wal_offset` 即在缓冲内下标取帧 —— 每段只付 **1 次顺序读**，换取段内全部随机解析为内存操作。
- 代的生命周期同步沿用 §11 的 host↔FPGA 三同步点（生效/失效/野读拒绝）。
- 缓冲预算：物化需要同时"打开"的代集合 = 当前输出键区间触及的所有 (part,gen)。若排序流跨分区交织广，
  按 **key 范围切段物化**（每段只碰少量代），或对段做 LRU 驻留；段回收（`ReleaseGens` 归零）前先让 FPGA
  失效并等引用排空（§9.2 的目录是权威）。
- 直连 NVMe 变体：若卡内存装不下活跃代集，退化为"**按 (part,gen,wal_offset) 对待取值排序后发起
  NVMe 批量顺序读**"——一次命令队列取回一条连续窗（如 1 MB）再本地切帧，保持 SSD 侧近顺序。

### 16.3 [C] SST 输出：单遍流式写 + 尾块缓冲，天然无需回填

- 布局约束：footer/metaindex 需要前导块的 handle，而 data 块条数/偏移只有写完才知道。解法 = **分两段**：
  1. **data 阶段**：排序+取值核逐键生成条目，按 target block 大小（建议 4 KB）累积成一块，随满随写
     NVMe（块 + 5 B trailer，trailer 的 masked crc32c **流水线内联计算**，不用二次读）；
     同时把每块 `(首键, 末键, 该块 handle, 累积统计)` 记入卡内 RAM。
  2. **tail 阶段**：data 写完即得 `tail_start`。在卡内拼 index 块（对每块算出 separator 键，§14.3；
     handle=记录的 offset/size）→ 顺序写 index；之后由 §16.5 的宿主封口器补
     properties/metaindex/footer（或卡内已有拼装引擎时直接写）。
- 因为文件是**尾端一次性追加**、且 index/properties/metaindex 都在 tail 里按序生成，全程**单写游标、零回写**；
  写满一个 4 KB 页对齐缓冲再发 NVMe 写，减少命令数。
- NVMe 队列：SST 写用独立 SQ，与 WAL 读分开；doorbell 批量；`block_size`(4 KB) 与 NVMe 原子写单位无关，
  无需对齐约束（trailer 允许跨界，与 WAL 帧同理）。

### 16.4 [A/B/C] 三流之间：sort 输出的两种落法

- **直接流水**（排序核 → 取值核 → 写核同卡链路，缓冲 ≤ 数个 block）：低延迟、省内存，但要求 value 就绪
  才能写，峰值速率受"最慢一代 WAL 载入"限制。
- **两级落盘**（排序输出先写一份紧凑 run 文件到 NVMe/CMEM，再起物化 pass 读 run+WAL 出 SST）：
  解耦排序速率与 WAL 载入，利于把 A 阶段做成分区并行、B 阶段按 §16.2 的 key 区间切片 —— 推荐用于
  SmartSSD 资源受限（卡上内存装不下全部活跃代）场景。

### 16.5 host/FPGA 分工建议（最小且字节可预期）

| 部件 | 谁做 | 原因 |
|---|---|---|
| data 块 + trailer + 索引键（separator） | FPGA | 吞吐主路径、需键序与 locator |
| handle 表、tail_start、统计（entries/size/最大 seq 等） | FPGA 产出 manifest 描述符 | 一行小结即可 |
| properties 块、metaindex 块、footer 及最终目录登记 | **宿主封口器** | 内容依赖 DB 选项（列族/comparator/统计），且极小（几十 B–几 KB）；直接用引擎自身 `PropertyBlockBuilder`/`FooterBuilder` 代码 → 零格式漂移，字节级可信（§14.4） |

> 该分工是"byte-exact 引擎可读"的最稳平衡点：**FPGA 生成所有可能超过单块大小的数据与索引**，
> 宿主用同一版本引擎的序列化器拼 4 个小块，天然与引擎写出一致。若后续要全 offload，再照 §13/§14
> 的字节规范把 properties/metaindex/footer 也搬进卡即可。

---

## 17. 源码位置索引

| 内容 | 位置 |
|---|---|
| InlineSkipList::Node 定义 | `memtable/inlineskiplist.h:358-421` |
| AllocateNode / AllocateKey | `memtable/inlineskiplist.h:853-880` |
| Key() / Next(i) / CAS | `memtable/inlineskiplist.h:374-407` |
| 条目编码（Add） | `db/memtable.cc:1079-1114` |
| SlimMemTableRep Allocate/Insert | `zeroflush/slim_memtable.cc:52-89` |
| EncodeKey（探针格式） | `zeroflush/slim_memtable.cc:41-51` |
| 比较器（排序规则） | `db/dbformat.h:1180-1199` |
| PackSequenceAndType | `db/dbformat.h:181-186` |
| PartitionIndex 条目布局 | `zeroflush/partition_index.h:5, 66-93, 115-130` |
| SlimLocator | `zeroflush/wal_format.h:45-57` |
| ZfRecord 帧格式 / 常量 | `zeroflush/wal_format.h:11-43,69`、`wal_format.cc:12-13` |
| 活跃/封存 value 读取 | `zeroflush/zeroflush_db.cc:986-1043` |
| 封存定点读 | `zeroflush/wal_manager.cc:318-…` |
| 段目录（SealedFileCache） | `zeroflush/sealed_file_cache.h` |
| 段文件名规则 | `zeroflush/wal_manager.cc:25-27` |
| 物化整段载入 + 内存二分取 value | `zeroflush/materialize_aside.h:129,154-165`、`materialize_job.h:271` |
| varint / fixed64 编码 | `util/coding.h` |
| arena 对齐 | `memory/arena.h:35`、`memory/arena.cc:108-142` |
| 块内记录 + restart 判定 | `table/block_based/block_builder.cc:189-220, 264-367` |
| data block packed footer | `table/block_based/data_block_footer.h/cc` |
| 5B trailer 组装 / crc | `block_based_table_builder.cc:2184-2205`、`table/format.cc:643-651`、`util/crc32c.h:37-46` |
| footer 53B 布局 / Build / Decode | `table/format.h:223-316`、`table/format.cc:225-328, 330-471` |
| magic 0x88e241b785f4cff7 | `block_based_table_builder.cc:136` |
| 写序 index→props→metaindex→footer | `block_based_table_builder.cc:2891-2913` |
| format_version=2 ⇒ 索引含 seq | `table/block_based/index_builder.h:257, 318-358, 437-449` |
| 索引值非 delta（v2） | `block_based_table_builder.cc:1087-1088, 2547-2548`、`table/format.h:118-129` |
| separator 算法 | `table/block_based/index_builder.h:267-296`、`block_based_table_builder.cc` |
| metaindex/properties 组装 | `table/meta_blocks.cc:35-213`、`table/meta_blocks.h` |
| 属性键名常量 | `table/table_properties.cc:284-360`、`block_based_table_factory.cc:1133-1145` |
| reader 必需属性 / 索引解码 | `table/block_based/block_based_table_reader.cc:1081-1166, 573-675` |
| 写侧配置默认值（checksum/interval/version） | `include/rocksdb/table.h:312,338,351,354,677,702,768` |
| WAL 目录/文件名 | `zeroflush/wal_manager.cc:25-27`、`zeroflush_db.h:48` |
| ZFPROPS v1/v2 | `zeroflush/wal_format.h:71-143`、`zeroflush_db.cc:1169` |
