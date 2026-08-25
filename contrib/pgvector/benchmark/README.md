# pgvector (OpenTenBase) SIFT1M 性能基线测试指南

针对 OpenTenBase 集中式（单机）环境的 IVFFlat 基线：向量距离计算、索引扫描延迟、
lists/probes 参数扫描、召回率（recall@10）与延迟权衡曲线。

- 数据集：SIFT1M（128 维，100 万条，10,000 条查询 + 精确 top-100 ground truth）
- 部署形态：GTM + 1 CN + 1 DN（全部在本机）
- 默认测 DN 直连（纯内核路径延迟）；也可切 CN 测端到端
- 延迟：psql `\timing` 单会话客户端计时，先预热一轮再测量
- 召回率：返回 top-10 与 ground truth 交集比例

## 0. 环境

- OpenTenBase 源码
- python3 + numpy（转换/报告脚本用）
- 运行时产物放在仓库外：`~/otb_bench/`（数据集、集群数据目录）

## 1. 编译安装

```bash
cd OpenTenBase

# 1.1 编译并安装 OpenTenBase 本体（configure 已完成，直接 make）
make -j16                 # 约 10~30 分钟
make install              # 安装到 ./otb_build

# 1.2 编译并安装 contrib（含 pgvector）
#     注意：contrib 下的 opentenbase_ctl（C++ 集群管理工具）与 gcc 16 不兼容会编译失败，
#     不影响 pgvector —— 单独安装 pgvector 即可（我们的部署脚本用 initgtm/initdb/pg_ctl，不需要它）：
cd contrib/pgvector
make -j16
make install

# 1.3 验证
~/otb_build/bin/pg_config --version
command ls ~/otb_build/share/postgresql/extension/vector.control   # 应存在
command ls ~/otb_build/lib/postgresql/vector.so                    # 应存在
```

说明：
- pgvector 是 in-tree 编译（Makefile 用 `top_builddir=../..`），不需要 PGXS。
- pgvector 默认带 `-march=native -ftree-vectorize` 编译距离计算，基线与后续优化
  对比都在同一台机器上，公平性没问题。
- 若编译报错，常见原因：缺少 `libxml2-devel` / `openssl-devel`（configure 已通过则
  无此问题）、gcc 版本过新导致告警升级——configure 里已带
  `-Wno-error=incompatible-pointer-types`，仍失败时把具体报错贴出来排查。

## 2. 部署集中式实例（GTM + CN + DN）

```bash
cd OpenTenBase/contrib/pgvector/benchmark

./03_setup_cluster.sh            # 初始化并启动（目录已存在会拒绝）
./03_setup_cluster.sh status     # 查看状态
./03_setup_cluster.sh stop       # 停止
FORCE=1 ./03_setup_cluster.sh    # 删除 ~/otb_bench/cluster 后重新部署
```

脚本做的事（等价于官方手动部署流程）：
1. `initgtm` 初始化 GTM（端口 50001）并启动
2. `initdb --nodetype=datanode --nodename=dn1 --master_gtm_*` 初始化 DN（端口 56000，
   pooler 6621），`pg_ctl start -Z datanode`
3. 同样初始化 CN（端口 55000，pooler 6611），`pg_ctl start -Z coordinator`
4. 在 CN 上注册节点与节点组（`CREATE NODE dn1 ...` / `CREATE NODE GROUP group1`）

## 3. 数据集准备

```bash
./01_download_sift1m.sh          # 下载 sift.tar.gz（约 120MB，IRISA texmex）并校验
python3 02_convert_sift1m.py --sift-dir ~/otb_bench/sift1m --out ~/otb_bench/sift1m/csv
```

- 下载源是 FTP，如果慢/失败，浏览器打开 `http://corpus-texmex.irisa.fr/` 手动下载
  `sift.tar.gz` 放到 `~/otb_bench/sift1m/` 再重跑脚本（会跳过下载直接解压校验）。
- 转换输出 `csv/base.csv`（100 万行 `id\t[...]`，id=1..1M 与 ground truth 索引对应）
  和 `csv/queries.csv`（默认前 100 条查询，`NQUERIES` 可调）。

## 4. 加载数据与建索引

```bash
./04_load_data.sh                # 建库/建表 + \copy 100 万行 + ANALYZE（几分钟）
./05_build_index.sh 1000         # ivfflat, lists=1000（1M 行的推荐起点：rows/1000）
```

- 默认连 DN 直连（56000）。要测 CN 端到端：`PGPORT=55000 ./04_load_data.sh`
  （表会自动改为 `DISTRIBUTE BY REPLICATION`）。两套数据独立，别混用同一个表名结论。
- 换 lists 对比时重复执行 `05_build_index.sh <lists>`（会先删旧索引）。

## 5. 运行基线并出报告

```bash
./06_run_baseline.sh             # lists 用 env.sh 默认值；或 ./06_run_baseline.sh 100
```

流程：
1. 精确参照组：禁用索引走全表 seq-scan top-10（recall 恒为 1.0，延迟上限）
2. ivfflat 扫描：probes ∈ {1,2,5,10,20,40,60,80,100,200,400,1000}
   （`PROBES_SWEEP="10 50 100" ./06_run_baseline.sh` 可自定义）
3. 每组先预热一遍（丢弃），再测量一遍；输出到 `results/raw_*.txt`
4. 自动调用 `07_report.py`，产出 `results/summary.csv` 与表格：

```
setting   | lists | recall | mean_ms | p50_ms | p95_ms | p99_ms | qps
----------|-------|--------|---------|--------|--------|--------|------
seq       | ...   | 1.0    | ...     | ...    | ...    | ...    | ...
l1000_p1  | 1000  | 0.48   | ...     | ...    | ...    | ...    | ...
l1000_p10 | 1000  | 0.84   | ...     | ...    | ...    | ...    | ...
...
```

对比优化前后：保存两份 `summary.csv`（如 `summary.baseline.csv`）再对齐比较。

## 6. 方法论说明

- **为什么经 CN 测**：OpenTenBase 的 DN 对应用直连**默认只读**（内核里
  `XactReadOnly |= IsPGXCNodeXactReadOnly()`，直连 DN 时强制只读）。写入必须走 CN。
  单 DN 场景下，IVFFlat 扫描和距离计算仍在 DN 内核执行，CN 只做规划和下发，
  延迟多一次 localhost 往返（亚毫秒级，基线测量口径一致即可）。
- **warmup**：每轮先完整跑一遍丢弃，排除计划缓存/页缓存冷启动影响。
- **计时口径**：客户端 `\timing`（含 CN↔DN 往返）；如需服务端口径可在 06 生成的
  SQL 里加 `EXPLAIN (ANALYZE, TIMING OFF)` 对照。
- **召回率口径**：top-10 集合与 ground truth top-10 的交集 / 10，对 100 条查询取平均。
- lists 建议扫描 `100 / 1000` 两档，probes 固定时 recall-lists 存在明显平台期，
  基线至少覆盖 `probes = sqrt(lists)` 附近（pgvector 官方经验起点）。

## 7. 常见问题

| 现象 | 处理 |
|------|------|
| `make` 报错 | 贴出最后 50 行报错排查；多半是 gcc 16 与 PG10 老代码的告警/标准问题 |
| `opentenbase_ctl` 编译失败 | 预期现象（gcc16 与旧 CLI11 不兼容），不影响本流程；用 `03_setup_cluster.sh` 手动部署 |
| `unrecognized configuration parameter "gtm_host"` | 本版本没有这些 GUC，GTM 位置由 `initdb --master_gtm_*` 写入 pgxc_node 目录；脚本已不设置它们 |
| 节点启动即崩（forward manager 端口冲突） | 每个节点的 `forward_port` 必须不同（脚本已设 6680/6681） |
| `node oid ... could not get nodeid` (建组 FATAL) | 建组前必须在**每个节点**执行 `SELECT pgxc_pool_reload()` 刷新共享内存节点表（脚本已处理） |
| `default group not defined` | 节点组必须用 `CREATE DEFAULT NODE GROUP` 创建 |
| 直连 DN 写入报 `read-only transaction` | 设计如此：DN 对应用直连只读，写入走 CN（脚本默认已走 CN） |
| 分布式写入报 `forward port 0` / 连接被终止 | 节点注册时缺 `FORWARD=<forward_port>`，见下方部署要点 |
| CN/DN 起不来 | 看 `~/otb_bench/cluster/*.log`；最常见是 GTM 没起来或端口占用 |
| `\timing` 输出混进结果 | 正常，07_report.py 会区分 `Time:` 行与 id 行 |
| 下载不动 | 手动下载见第 3 节 |
| 想清空重来 | `FORCE=1 ./03_setup_cluster.sh`，然后从第 4 步重来 |

> 部署要点（本 OpenTenBase 版本的非显而易见行为）：`CREATE NODE`/`ALTER NODE` 的
> `PORT` 填节点**主端口**，另需 `FORWARD=<forward_port>`（每节点不同，DN 6680 / CN 6681）；
> DN 的自节点 `dn1` 必须标记 `PRIMARY`；拓扑需在 CN 和 DN 两侧都注册，然后各自
> `SELECT pgxc_pool_reload()`；`ALTER NODE ... REFRESH NODE` 语法不存在。
