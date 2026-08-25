# Cosine（余弦 / 角度）基线

针对 **余弦距离（cosine / angular）** 的 IVFFlat 基线。本目录自包含，可独立运行完整流程。

- 数据集：**GloVe-100**（`glove-100-angular`，ann-benchmarks）—— 词向量，约
  118 万 × 100 维，ground truth 按角度（angular = 归一化余弦）计算
- 运算符：`<=>`，opclass：`vector_cosine_ops`，表名：`glove_base`

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

| 项 | L2（`../l2/`） | IP（`../ip/`） | Cosine（本目录） |
|----|---------------|---------------|-----------------|
| 数据集 | SIFT1M (fvecs) | Last.fm (HDF5) | GloVe-100 (HDF5) |
| 表名 | `sift_base` | `lastfm_base` | `glove_base` |
| 运算符 | `<->` | `<#>` | `<=>` |
| opclass | `vector_l2_ops` | `vector_ip_ops` | `vector_cosine_ops` |
| ground truth 距离 | L2（欧氏） | 内积（dot） | 余弦（angular） |
