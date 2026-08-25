# IP（内积 / MIPS）基线

针对 **内积（inner product）** 距离的 IVFFlat 基线。本目录自包含，可独立运行完整流程。

- 数据集：**Last.fm**（`lastfm-64-dot`，ann-benchmarks）—— 音乐收听计数向量，
  约 29 万 × 65 维，ground truth 按内积（dot）计算
- 运算符：`<#>`，opclass：`vector_ip_ops`，表名：`lastfm_base`

## 依赖

```bash
pip install h5py     # convert.py 解析 HDF5 需要
```

## 一键运行

```bash
./run.sh              # setup → download → convert → load → index → baseline → report → plot
```

结果输出到本目录 `results/`。

## 剖析（可选）

```bash
./profile.sh 10 30
./profile.sh --stat 10 20
./stat_sweep.sh 10 1 5 10 50 100
```

## 参数

```bash
LISTS=100 NQUERIES=100 ./run.sh
```

## 三场景对照

| 项 | L2（`../l2/`） | IP（本目录） | Cosine（`../cos/`） |
|----|---------------|-------------|--------------------|
| 数据集 | SIFT1M (fvecs) | Last.fm (HDF5) | GloVe-100 (HDF5) |
| 表名 | `sift_base` | `lastfm_base` | `glove_base` |
| 运算符 | `<->` | `<#>` | `<=>` |
| opclass | `vector_l2_ops` | `vector_ip_ops` | `vector_cosine_ops` |
| ground truth 距离 | L2（欧氏） | 内积（dot） | 余弦（angular） |
