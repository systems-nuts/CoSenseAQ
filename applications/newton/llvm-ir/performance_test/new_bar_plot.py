import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from matplotlib.ticker import MultipleLocator

# The data processing functions remain the same as the last version.
# They are included here so the script is complete.

def build_precision_map_from_quant_file(file_content):
    precision_map = {}
    current_benchmark_key = None
    lines = file_content.strip().split('\n')
    if lines and "test case\tparam\tprecision_bits" in lines[0]:
        lines = lines[1:]

    precision_bits_col_index = 2
    min_parts_for_precision_line = precision_bits_col_index + 1

    for line in lines:
        parts = line.split('\t')
        if not parts: continue
        test_case = parts[0].strip()
        if test_case.startswith("perf_j0"): current_benchmark_key = "j0"
        elif test_case.startswith("perf_y0"): current_benchmark_key = "y0"
        else: continue

        if len(parts) > min_parts_for_precision_line:
            params_str = parts[1].strip()
            if len(params_str.split()) >= 2:
                try:
                    precision_val = parts[precision_bits_col_index].strip()
                    if precision_val:
                        int(precision_val)
                        map_key = (current_benchmark_key, params_str)
                        if map_key not in precision_map:
                            precision_map[map_key] = precision_val
                except (IndexError, ValueError):
                    pass
    if not precision_map:
        print("Warning: Precision map is empty. Check 'perf_quant.log' format.")
    return precision_map

def parse_summary_perf_data(file_content, quantization_type):
    data = []
    current_benchmark_key = None
    lines = file_content.strip().split('\n')
    if lines and "test case\tparam" in lines[0]:
        lines = lines[1:]

    for line in lines:
        parts = line.split('\t')
        if not parts:
            continue
        test_case = parts[0].strip()
        if test_case.startswith("perf_j0") or (quantization_type=="w/o Auto Quantization" and test_case.startswith("perf_exp")):
            current_benchmark_key = "j0"
        elif test_case.startswith("perf_y0"):
            current_benchmark_key = "y0"
        elif test_case == "speed up after optimization":
            if current_benchmark_key is None or current_benchmark_key not in ["j0", "y0"]:
                continue
            if len(parts) < 7:
                continue
            try:
                params_str = parts[1].strip()
                param_values = params_str.split()
                if len(param_values) < 2:
                    continue
                formatted_params = f"[{float(param_values[0]):.1f}, {float(param_values[1]):.1f}]"
                time_speedup_pct = float(parts[3].replace('%', ''))
                library_size_reduction_pct = float(parts[5].replace('%', ''))
                speedup_factor = 1 + (time_speedup_pct / 100.0)
                data.append({
                    "benchmark": current_benchmark_key,
                    "params_raw": params_str,
                    "params_formatted": formatted_params,
                    "quant_type": quantization_type,
                    "speedup_factor": speedup_factor,
                    "library_size_reduction_pct": library_size_reduction_pct,
                    "precision_bits": None
                })
            except (IndexError, ValueError):
                continue
        else:
            current_benchmark_key = None
            continue
    return data

def process_performance_data(wo_quant_path, w_quant_path):
    quant_precision_map = {}
    try:
        with open(w_quant_path, 'r', encoding='utf-8') as f:
            content = f.read()
        quant_precision_map = build_precision_map_from_quant_file(content)
    except FileNotFoundError:
        print(f"CRITICAL: '{w_quant_path}' not found.")
        return pd.DataFrame()

    all_data_collected = []
    try:
        with open(wo_quant_path, 'r', encoding='utf-8') as f:
            content = f.read()
        all_data_collected.extend(parse_summary_perf_data(content, "w/o Auto Quantization"))
    except FileNotFoundError: print(f"Warning: '{wo_quant_path}' not found.")

    try:
        with open(w_quant_path, 'r', encoding='utf-8') as f:
            content = f.read()
        all_data_collected.extend(parse_summary_perf_data(content, "with Auto Quantization"))
    except FileNotFoundError: print(f"Warning: '{w_quant_path}' not found for summary parsing.")

    if not all_data_collected: return pd.DataFrame()

    for item in all_data_collected:
        item['precision_bits'] = quant_precision_map.get((item['benchmark'], item['params_raw']))
    df = pd.DataFrame(all_data_collected)

    if df.empty: return df

    df['range_over_precision_bits'] = df.apply(
        lambda row: f"{row['params_formatted']}/{row['precision_bits']}" if pd.notna(row['precision_bits']) else row['params_formatted'],
        axis=1
    )

    pivot_df = df.pivot_table(
        index=['benchmark', 'params_raw', 'range_over_precision_bits'],
        columns='quant_type',
        values=['speedup_factor', 'library_size_reduction_pct']
    )

    pivot_df.columns = [f'{val}_{quant}' for val, quant in pivot_df.columns]
    pivot_df.reset_index(inplace=True)

    pivot_df['relative_speedup'] = pivot_df.get('speedup_factor_with Auto Quantization', 1) / pivot_df.get('speedup_factor_w/o Auto Quantization', 1)
    pivot_df['size_reduction_improvement'] = pivot_df.get('library_size_reduction_pct_with Auto Quantization', 0) - pivot_df.get('library_size_reduction_pct_w/o Auto Quantization', 0)

    # #
    # print("------------- Original w/o Auto Quantization  -------------")
    # for index, row in pivot_df.iterrows():
    #      print(f"{row['range_over_precision_bits']}: {row.get('speedup_factor_w/o Auto Quantization', 'N/A')}")

    return pivot_df

# --- UPDATED Plotting function for single bars and refined Y-axis ---
def create_single_bar_plot(df_benchmark_data, benchmark_id, metric_column, y_axis_label, plot_title_suffix, output_filename):
    if df_benchmark_data.empty:
        print(f"No data available for {benchmark_id}, skipping '{plot_title_suffix}' plot.")
        return

    df_sorted = df_benchmark_data.copy()
    df_sorted['sort_key_params_numeric'] = df_sorted['params_raw'].apply(lambda x: tuple(map(float, x.split())))
    df_sorted = df_sorted.sort_values(by='sort_key_params_numeric').reset_index(drop=True)

    is_speedup_plot = "Speedup" in y_axis_label

    num_param_groups = len(df_sorted.index)
    x_positions = np.arange(num_param_groups)
    fig_width = max(12, num_param_groups * 0.8)
    fig, ax = plt.subplots(figsize=(fig_width, 7))

    bar_data = pd.to_numeric(df_sorted[metric_column], errors='coerce').fillna(0)
    ax.bar(x_positions, bar_data, width=0.6, color='#6A0DAD', label=plot_title_suffix)

    ax.set_xlabel("Range/PrecisionBits")
    ax.set_ylabel(y_axis_label)
    ax.set_title(f"{benchmark_id.upper()} - {plot_title_suffix}")
    ax.set_xticks(x_positions)
    ax.set_xticklabels(df_sorted['range_over_precision_bits'], rotation=45, ha="right")

    if is_speedup_plot:
        ax.set_ylim(0, 1.5)
        ax.set_yticks([0, 1, 1.5])
        ax.axhline(1.0, color='grey', linestyle='-.', linewidth=1.0)
    else:
        ax.axhline(0, color='grey', linestyle='-.', linewidth=1.0)
        ax.grid(True, linestyle='--', alpha=0.7, axis='y')

    plt.tight_layout()
    plt.savefig(output_filename)
    print(f"Plot saved: {output_filename}")
    plt.close(fig)

# --- Main script execution ---
file_path_wo_quant = "perf_woquant.log"
file_path_w_quant = "perf_quant.log"

# Process data and calculate relative improvements
df_processed = process_performance_data(file_path_wo_quant, file_path_w_quant)

if not df_processed.empty:
    df_j0_final = df_processed[df_processed["benchmark"] == "j0"]
    create_single_bar_plot(df_j0_final, "e_j0", "relative_speedup",
                           "Relative Speedup (Quant / w/o Quant)",
                           "Quantization Speedup Improvement",
                           "e_j0_relative_speedup.png") # New filename
    create_single_bar_plot(df_j0_final, "e_j0", "size_reduction_improvement",
                           "Additional Size Reduction (%)",
                           "Quantization Size Reduction Improvement",
                           "e_j0_relative_size_reduction.png") # New filename

    df_y0_final = df_processed[df_processed["benchmark"] == "y0"]
    create_single_bar_plot(df_y0_final, "e_y0", "relative_speedup",
                           "Relative Speedup (Quant / w/o Quant)",
                           "Quantization Speedup Improvement",
                           "e_y0_relative_speedup.png") # New filename
    create_single_bar_plot(df_y0_final, "e_y0", "size_reduction_improvement",
                           "Additional Size Reduction (%)",
                           "Quantization Size Reduction Improvement",
                           "e_y0_relative_size_reduction.png") # New filename

    print("\n--- Plot generation complete ---")
else:
    print("\n--- No data processed, plot generation skipped ---")
