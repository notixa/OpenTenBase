# IP（内积）基线

针对 **IP（内积）** 距离的 IVFFlat 基线。本目录自包含，可独立运行完整流程。

- 数据集：**SIFT1M**（IRISA texmex）—— 128 维，100 万条，ground truth 按 L2 计算
- 运算符：`<#>`，opclass：`vector_ip_ops`，表名：`lastfm_base`

## 一键运行

```bash
./run.sh              # 运行原生 tuplesort 基线测试
./run_opt.sh          # 运行 Top-K 堆 + 1-to-N 批处理优化测试
```

原生基线结果输出到 `results/` 目录下（包含 `summary.csv` 与 `baseline_curve.png` 等）。
优化测试结果将输出到 `results_scan_opt/` 目录下。

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
| 表名 | `lastfm_base` | `lastfm_base` | `glove_base` |
| 运算符 | `<#>` | `<#>` | `<=>` |
| opclass | `vector_ip_ops` | `vector_ip_ops` | `vector_cosine_ops` |
| ground truth 距离 | IP（内积） | 内积（dot） | 余弦（angular） |
