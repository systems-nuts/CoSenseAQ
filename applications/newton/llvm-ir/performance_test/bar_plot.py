import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import os

breakdown = False


def bar_plot_groups(labels,
                    y1,
                    y2,
                    y1_label,
                    y2_label,
                    ylabel,
                    title,
                    save_fig=''):
    """
    A simple wrapper for matplotlib's barplot.

    :param labels:
    :param y1:
    :param y2:
    :param ylabel:
    :param xlabel:
    :param save_fig:
    :return:
    """
    x = np.arange(len(labels))  # the label locations
    width = 0.35  # the width of the bars

    plt.rc('text', usetex=True)
    plt.rc('font', family='serif')
    plt.rc('font', size=12)

    fig, ax = plt.subplots()
    ax.bar(x[1:len(y1)], y1[1:], width, label=y1_label, color='#c2a5cf')
    ax.bar(x, y2, width, label=y2_label, color='#7b3294')

    # Add some text for labels, title and custom x-axis tick labels, etc.
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.set_xticks(x)
    ax.set_xticklabels(labels)

    plt.xticks(rotation=45)
    ax.yaxis.grid()  # horizontal lines

    fig.tight_layout()

    if len(save_fig):
        plt.savefig(save_fig)
    else:
        plt.show()


def bar_plot_pairs(labels,
                   y1,
                   y2,
                   y1_label,
                   y2_label,
                   ylabel,
                   title,
                   save_fig=''):
    """
    A simple wrapper for matplotlib's barplot.

    :param labels:
    :param y1:
    :param y2:
    :param ylabel:
    :param xlabel:
    :param save_fig:
    :return:
    """
    x = np.arange(len(labels))  # the label locations
    width = 0.35  # the width of the bars

    plt.rc('text', usetex=True)
    plt.rc('font', family='serif')
    plt.rc('font', size=12)

    fig, ax = plt.subplots()

    if breakdown:
        ax.bar(x[0:4] - width / 2, y1[0:4], width, label=y1_label+"w/o Overload", color='#fdb863')
        ax.bar(x[0:4] + width / 2, y2[0:4], width, label=y2_label+"w/o Overload", color='#e66101')
        ax.bar(x[4:] - width / 2, y1[4:], width, label=y1_label+"w/ Overload", color='#c2a4cf')
        ax.bar(x[4:] + width / 2, y2[4:], width, label=y2_label+"w/ Overload", color='#7b3294')

        # Add some text for labels, title and custom x-axis tick labels, etc.
        if ylabel == 'Speedup':
            y_axis_length = [0.9, 2.4]
            ax.set_ylim(y_axis_length)
            ax.set_yticks(np.arange(y_axis_length[0]+0.1, y_axis_length[1]+0.1, 0.2))
        elif ylabel == 'Size reduction (\%)':
            y_axis_length = [2, 13]
            ax.set_ylim(y_axis_length)
            ax.set_yticks(np.arange(y_axis_length[0], y_axis_length[1], 2))
    else:
        ax.bar(x - width / 2, y1, width, label=y1_label, color='#c2a4cf')
        ax.bar(x + width / 2, y2, width, label=y2_label, color='#7b3294')

        # Add some text for labels, title and custom x-axis tick labels, etc.
        if ylabel == 'Speedup':
            y_axis_length = [1, 2.1]
            ax.set_ylim(y_axis_length)
            ax.set_yticks(np.arange(y_axis_length[0], y_axis_length[1], 0.2))
        elif ylabel == 'Size reduction (\%)':
            y_axis_length = [-5, 36]
            ax.set_ylim(y_axis_length)
            ax.set_yticks(np.arange(y_axis_length[0], y_axis_length[1], 5))

    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.legend(prop={'size': 9})
    fig.set_figheight(3)

    plt.xticks(rotation=45)
    ax.yaxis.grid()  # horizontal lines

    fig.tight_layout()

    if len(save_fig) > 0:
        plt.savefig(save_fig)


if __name__ == '__main__':
    if breakdown:
        df = pd.read_excel('breakdown_performance.xlsx',
                           sheet_name='speedup',
                           nrows=5,
                           usecols='C:F',
                           engine='openpyxl')
        df.rename(columns={
            "type compression": "Type\nCompression",
            "branch elimination": "Condition\nSimplification",
            "constant substitution": "Constant\nSubstitution",
            "overall": "Overall"
        },
            inplace=True)
        print(df)

        labels = list(df.columns.values)
        labels = labels + labels

        y1 = list(df.iloc[0]) + list(df.iloc[1])
        y2 = list(df.iloc[2]) + list(df.iloc[3])

        bar_plot_pairs(labels, y1, y2, 'arm', 'x86', 'Speedup',
                       'Speedup breakdown - fp_add [0,200]', 'breakdown_speedup.png')

        df = pd.read_excel('breakdown_performance.xlsx',
                           sheet_name='size',
                           nrows=5,
                           usecols='C:F',
                           engine='openpyxl')
        df.rename(columns={
            "type compression": "Type\nCompression",
            "branch elimination": "Condition\nSimplification",
            "constant substitution": "Constant\nSubstitution",
            "overall": "Overall"
        },
            inplace=True)
        print(df)

        labels = list(df.columns.values)
        labels = labels + labels

        y1 = list(map(lambda x: 100*x, list(df.iloc[0]) + list(df.iloc[1])))
        y2 = list(map(lambda x: 100*x, list(df.iloc[2]) + list(df.iloc[3])))

        bar_plot_pairs(labels, y1, y2, 'arm', 'x86', 'Size reduction (\%)',
                       'Size reduction breakdown - fp_add [0,200]', 'breakdown_size.png')

        # bar_plot_groups(labels, y1, y2, 'w/o Function Overload',
        #                 'w/ Function Overload', 'Speedup',
        #                 'Performance breakdown of CoSense optimizations',
        #                 'speedup_breakdown.png')
    else:
        # X86
        df = pd.read_excel('overall_speedup_microbenchmarks.xlsx',
                           sheet_name='x86',
                           nrows=10,
                           header=None,
                           names=['benchmark', 'speedup', 'size'],
                           usecols='A,B,D',
                           engine='openpyxl')
        df['benchmark'] = df['benchmark'].apply(
            lambda x: x.replace(' with time speed up', ''))

        labels = list(df['benchmark'].values)
        x86_speedup = list(df['speedup'].values)
        x86_size = list(df['size'].values)
        x86_speedup = list(map(lambda x: float(x.strip('%'))/100 + 1.0, x86_speedup))
        x86_size = list(map(lambda x: float(x.strip('%')), x86_size))

        # ARM
        df = pd.read_excel('overall_speedup_microbenchmarks.xlsx',
                           sheet_name='arm64',
                           nrows=10,
                           header=None,
                           names=['benchmark', 'speedup', 'size'],
                           usecols='A,B,D',
                           engine='openpyxl')

        arm_speedup = list(df['speedup'].values)
        arm_size = list(df['size'].values)
        arm_speedup = list(map(lambda x: float(x.strip('%'))/100 + 1.0, arm_speedup))
        arm_size = list(map(lambda x: float(x.strip('%')), arm_size))

        bar_plot_pairs(labels, arm_speedup, x86_speedup, 'arm', 'x86', 'Speedup',
                       'Speedup in microbenchmarks', 'speedup_microbenchmarks.png')

        bar_plot_pairs(labels, arm_size, x86_size, 'arm', 'x86',
                       'Size reduction (\%)', 'Library size reduction in microbenchmarks',
                       'size_reduction_microbenchmarks.png')

    os.system("mv *.png fig/.")