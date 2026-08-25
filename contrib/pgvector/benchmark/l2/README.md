# L2（欧氏距离）基线

针对 **L2（欧氏）** 距离的 IVFFlat 基线。本目录自包含，可独立运行完整流程。

- 数据集：**SIFT1M**（IRISA texmex）—— 128 维，100 万条，ground truth 按 L2 计算
- 运算符：`<->`，opclass：`vector_l2_ops`，表名：`sift_base`

## 一键运行

```bash
./run.sh              # setup → download → convert → load → index → baseline → report → plot
```

结果输出到本目录 `results/`（`summary.csv` + `baseline_curve.png`）。

## 分步运行

```bash
./setup.sh                    # 单机 initdb + start（幂等；stop/status 同理）
./download.sh                 # 下载 SIFT1M
python3 convert.py "$DATASET_SRC" "$CSV_DIR" 100
./run.sh                      # 或直接跑 run.sh 一键完成
```

## 剖析（可选）

```bash
./profile.sh 10 30            # perf 火焰图（probes=10，30s）
./profile.sh --stat 10 20     # perf stat（IPC/缓存）
./stat_sweep.sh 10 1 5 10 50 100   # 跨 probes 扫描 IPC/缓存
```

## 参数

```bash
LISTS=100 NQUERIES=100 PROBES_SWEEP="1 5 10 50 100" ./run.sh
```

## 三场景对照

| 项 | L2（本目录） | IP（`../ip/`） | Cosine（`../cos/`） |
|----|-------------|---------------|--------------------|
| 数据集 | SIFT1M (fvecs) | Last.fm (HDF5) | GloVe-100 (HDF5) |
| 表名 | `sift_base` | `lastfm_base` | `glove_base` |
| 运算符 | `<->` | `<#>` | `<=>` |
| opclass | `vector_l2_ops` | `vector_ip_ops` | `vector_cosine_ops` |
| ground truth 距离 | L2（欧氏） | 内积（dot） | 余弦（angular） |
