import os
import csv

scenarios = ['l2', 'ip', 'cos']
probes_to_show = ['p10', 'p20', 'p60', 'p100', 'p200', 'p1000']

print("## 🚀 pgvector IVFFlat 综合评测报告 (优化前后对比)")
print("\n本次评测针对 `l2` (欧氏距离), `ip` (内积), `cos` (余弦距离) 三大核心场景，深入对比了基线（原生 `tuplesort` 全量排序）与我们新加入的优化路径（**Top-K 堆过滤 + 1-to-N 批处理**）的性能表现。")

for s in scenarios:
    print(f"\n### 场景：{s.upper()}")
    
    base_summary = f"benchmark/{s}/results/summary.csv"
    opt_summary = f"benchmark/{s}/results_scan_opt/summary.csv"
    
    if not os.path.exists(base_summary) or not os.path.exists(opt_summary):
        print(f"> ⚠️ {s.upper()} 场景的测试结果尚未完全生成。")
        continue

    def read_summary(path):
        data = {}
        with open(path, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                setting = row['setting']
                data[setting] = row
        return data

    try:
        base_data = read_summary(base_summary)
        opt_data = read_summary(opt_summary)
        
        print("| Probes | 原生基线 QPS | 优化后 QPS | 提升倍数 | 基线平均延迟(ms) | 优化后平均延迟(ms) |")
        print("|--------|--------------|------------|----------|------------------|--------------------|")
        
        def probe_num(k):
            try: return int(k.split('_p')[1])
            except: return 9999
            
        for k in sorted(base_data.keys(), key=probe_num):
            if k == 'seq': continue
            probe_name = k.split('_')[1]
            if probe_name not in probes_to_show: continue
            
            b_qps = float(base_data[k]['qps'])
            o_qps = float(opt_data[k]['qps']) if k in opt_data else 0
            b_lat = base_data[k]['mean_ms']
            o_lat = opt_data[k]['mean_ms'] if k in opt_data else "-"
            
            if o_qps > 0:
                boost = f"{o_qps / b_qps:.2f} X"
            else:
                boost = "-"
            
            print(f"| **{probe_name}** | {b_qps:.1f} | {o_qps:.1f} | **{boost}** | {b_lat} | {o_lat} |")
    except Exception as e:
        print(f"> 解析错误: {e}")

print("\n#### 🔍 硬件指标剖析 (以 LLC Miss 三级缓存未命中率为例)")
for s in scenarios:
    base_stat = f"benchmark/{s}/results/stat_sweep.csv"
    opt_stat = f"benchmark/{s}/results_scan_opt/stat_sweep.csv"
    if os.path.exists(base_stat) and os.path.exists(opt_stat):
        def read_stat(path):
            data = {}
            with open(path, 'r') as f:
                reader = csv.DictReader(f)
                for row in reader:
                    data[row['probes']] = row['LLCmiss%']
            return data
        b_s = read_stat(base_stat)
        o_s = read_stat(opt_stat)
        
        p_eval = '100'
        # If '100' isn't available, find highest available probe
        if p_eval not in b_s:
            p_eval = max((k for k in b_s.keys() if k.isdigit()), key=int, default='N/A')
            
        print(f"\n- **{s.upper()}** (Probes={p_eval}): 原生基线 {b_s.get(p_eval, '-')} -> 优化后 **{o_s.get(p_eval, '-')}**")
