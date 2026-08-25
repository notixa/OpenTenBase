# pgvector (PostgreSQL 19) IVFFlat 性能基准

针对**单机 PostgreSQL 19beta3 + pgvector 0.8.6** 的 IVFFlat 基线：向量距离计算、索引
扫描延迟、lists/probes 参数扫描、召回率（recall@10）与延迟权衡曲线。

按距离类型分为**三个完全独立、可各自单独运行的场景目录**（无共用脚本）：

```
benchmark/
├── l2/        # L2 欧氏（SIFT1M）
├── ip/        # IP 内积（Last.fm）
├── cos/       # Cosine 余弦（GloVe-100）
└── README.md  # 本文件（总览）
```

每个场景目录结构完全一致，且自包含：

```
l2/  (或 ip/ cos/)
├── env.sh        # 场景配置（路径/端口/连接/参数 + TABLE/OPCLASS/OP/数据集）
├── setup.sh      # 单机 initdb + start/stop（幂等）
├── download.sh   # 下载数据集
├── convert.py    # 数据集 → base.csv + queries.csv + truth.ivecs + dim.txt
├── pipeline.sh   # load → index → sweep → report → plot
├── report.py     # 报告（summary.csv）
├── plot.py       # 曲线图
├── profile.sh    # perf 火焰图 / perf stat 剖析
├── stat_sweep.sh # perf stat 跨 probes 扫描
├── run.sh        # 一键完整流程：setup → download → convert → pipeline
└── README.md     # 场景说明
```

## 三个场景对照

| 场景 | 数据集 | 规模 | 表名 | 运算符 | opclass | ground truth 距离 |
|------|--------|------|------|--------|---------|------------------|
| **l2** | SIFT1M (fvecs) | 1M × 128 | `sift_base` | `<->` | `vector_l2_ops` | L2（欧氏） |
| **ip** | Last.fm (HDF5) | 292K × 65 | `lastfm_base` | `<#>` | `vector_ip_ops` | 内积（dot） |
| **cos** | GloVe-100 (HDF5) | 1.18M × 100 | `glove_base` | `<=>` | `vector_cosine_ops` | 余弦（angular） |

## 快速开始

```bash
cd contrib/pgvector/benchmark/l2      # 或 ip / cos
./run.sh                              # 一键：setup→download→convert→load→index→baseline→report→plot
```

结果输出到该场景目录下的 `results/`（`summary.csv` + `baseline_curve.png`）。

## 前置：编译安装（一次）

```bash
cd <repo>
./configure --prefix="$PWD/otb19_build" --with-openssl CFLAGS="-O2 -g"   # 必须带 -O2！
make -j16 && make install
cd contrib/pgvector
make PG_CONFIG=$PWD/../../otb19_build/bin/pg_config
make PG_CONFIG=$PWD/../../otb19_build/bin/pg_config install
```

> ⚠️ **`-O0` 陷阱**：configure 不写 `-O2` 会得到 `-O0` 调试构建（距离计算标量、慢 3~5 倍、
> 索引构建慢 22 倍）。务必 `CFLAGS="-O2 -g"`，并用 objdump 验证向量化（`vsubps/vmulps/vaddps`
> + `%ymm`）。

## 依赖

- python3 + numpy（转换/报告/绘图用）
- `h5py`（ip / cos 场景的 convert.py 解析 HDF5 用）：`pip install h5py`
- perf（可选，剖析用）

## 场景脚本说明（三场景同构）

| 文件 | 作用 |
|------|------|
| `env.sh` | 场景配置（路径、端口、连接、基准参数默认值 + SCENARIO/TABLE/OPCLASS/OP/数据集路径） |
| `setup.sh` | 单机 initdb/start（幂等），三场景共用一个数据目录 `$PGDATA` |
| `download.sh` | 下载数据集（l2 是 tarball，ip/cos 是 HDF5） |
| `convert.py` | 数据集 → `base.csv` + `queries.csv` + `truth.ivecs` + `dim.txt`（统一输出格式） |
| `pipeline.sh` | 建表加载 → 建 ivfflat 索引 → seq-scan 参照 + probes 扫描 → report → plot |
| `run.sh` | 编排 setup → download → convert → pipeline |
| `profile.sh` / `stat_sweep.sh` | perf 剖析（火焰图 / IPC+缓存） |

## 参数

在场景内覆盖基准参数：

```bash
LISTS=100 TOPK=10 NQUERIES=100 PROBES_SWEEP="1 5 10 20 50 100" ./run.sh
```

## 常见问题

| 现象 | 处理 |
|------|------|
| `unrecognized parameter "sql_mode"` | OpenTenBase 专属字段，单机 PG19 不识别，从 vector.control 删除 |
| `make install` 报 `dynloader.h` stat 失败 | 旧分支断链符号链接，`find src -type l ! -exec test -e {} \; -delete` |
| objdump 看不到 ymm/vsubps | 是 `-O0`，重 configure 加 `CFLAGS="-O2"` |
| ip/cos 报 `No module named h5py` | `pip install h5py` |
| `\timing` 输出混进结果 | 正常，report.py 会区分 `Time:` 行与 id 行 |
