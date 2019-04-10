from argparse import ArgumentParser
from collections import defaultdict
from IPython import embed
import itertools
import json
from enum import Enum
from math import sqrt
import pandas as pd
import numpy as np
from pathlib import Path
from pprint import pprint
from matplotlib.gridspec import GridSpec, GridSpecFromSubplotSpec
import matplotlib.pyplot as plt
import matplotlib.patheffects as PathEffects
from matplotlib.patches import Patch
from matplotlib.gridspec import GridSpec, GridSpecFromSubplotSpec
import matplotlib.ticker as ticker
import re
import yaml

from Graph import Grapher
from NDADataObject import NDADataObject

import pandas as pd

class NDAGrapher:

    def __init__(self, args):
        self.args = args
        self.data = NDADataObject(Path(args.input_file))
        self.output_dir = Path(args.output_dir)
        self.schema_file = Path(args.schema_file)
        assert self.schema_file.exists() and self.output_dir.exists()
        with self.schema_file.open() as f:
            self.schema_config = yaml.safe_load(f)
            self.schemas = self.schema_config['schemes']
            assert args.schema_name in self.schemas
            self.schema = self.schemas[args.schema_name]
            self.schema_name = args.schema_name
            self.config_sets = self.schema_config['config_sets']

    def _plot_single_stat(self, gs, layout):
        grapher = Grapher(self.args)

        dfs = self.data.data_by_benchmark()
        options = layout['options']

        config_set = layout['config_set'] if 'config_set' in layout else 'default'
        dfs = self.data.filter_configs(self.config_sets[config_set], dfs)
        if 'benchmarks' in layout:
            dfs = self.data.filter_benchmarks(layout['benchmarks'], dfs)
        dfs = self.data.reorder_data_frames(dfs)
        dfs = self.data.filter_stats(layout['stat'], dfs)

        means_df = None
        ci_df = None
        flush = True
        if 'average' in options and options['average']:
            dfs = self.data.average_stats(dfs)
            flush = True
            means_df = pd.DataFrame({'Average': dfs.T['mean']}).T
            ci_df = pd.DataFrame({'Average': dfs.T['ci']}).T
        else:
            means_df = self.data.filter_stat_field('mean', dfs)
            ci_df = self.data.filter_stat_field('ci', dfs)

            if 'benchmarks' in layout and 'Average' in layout['benchmarks']:
                # Also add an average bar:
                dfs = self.data.data_by_benchmark()
                dfs = self.data.filter_configs(self.config_sets[config_set], dfs)
                dfs = self.data.reorder_data_frames(dfs)
                dfs = self.data.filter_stats(layout['stat'], dfs)
                avg_dfs = self.data.average_stats(dfs)
                avg_mean = avg_dfs.T['mean']
                ci_zero = avg_mean.copy()
                ci_zero[:] = 0
                means_df = means_df.append(pd.DataFrame({'Average': avg_mean}).T)
                ci_df = ci_df.append(pd.DataFrame({'Average': ci_zero}).T)
                flush = True

        if 'benchmarks' in layout:
            means_df = means_df.reindex(layout['benchmarks'])
            ci_df = ci_df.reindex(layout['benchmarks'])

        return grapher.graph_single_stat(means_df, ci_df, gs,
                                         flush=flush, **options)

    def _plot_cpi_breakdown(self, gs, layout):
        grapher = Grapher(self.args)

        dfs = self.data.data_by_benchmark()
        print(dfs)
        config_set = layout['config_set'] if 'config_set' in layout else 'default'
        dfs = self.data.filter_configs(self.config_sets[config_set], dfs)
        dfs = self.data.calculate_cpi_breakdown(dfs)

        options = layout['options']
        if 'average' in options and options['average']:
            dfs = dfs.mean(level=1)
            mi_tuples = [('Average', i) for i in dfs.index]
            dfs.index = pd.MultiIndex.from_tuples(mi_tuples)


        return grapher.graph_grouped_stacked_bars(dfs, gs, **options)

    def plot_schema(self):
        grapher = Grapher(self.args)

        dimensions = self.schema['dimensions']

        artists = []
        for layout_config in self.schema['plots']:
            width = layout_config['size'][0]
            height = layout_config['size'][1]
            axis = plt.subplot2grid(dimensions, layout_config['pos'],
                                    rowspan=width, colspan=height)

            if layout_config['type'] == 'single_stat':
                a = self._plot_single_stat(axis, layout_config)
                artists += [a]
            elif layout_config['type'] == 'cpi_breakdown':
                artists += self._plot_cpi_breakdown(axis, layout_config)

        plt.subplots_adjust(wspace=0.05, hspace=0.0)

        fig = plt.gcf()
        print_size = self.schema['print_size']
        fig.set_size_inches(*print_size)
        fig.tight_layout()

        output_file = str(self.output_dir / self.schema['file_name'])
        plt.savefig(output_file, dpi=300, bbox_inches='tight', pad_inches=0.02,
                    additional_artists=artists)
        plt.close()

    @classmethod
    def add_parser_args(cls, parser):
        parser.add_argument('--input-file', '-i', default='report.json',
                            help='Where the aggregations live')
        parser.add_argument('--output-dir', '-d', default='.',
                            help='Where to output the report')
        parser.add_argument('--config', '-c', default='graph_config.yaml',
                            help='What file to use for this dataset.')

        parser.add_argument('--schema-file', default='graph_schemes.yaml',
                            help='File containing relevant schemas.')
        parser.add_argument('--filter', '-f', default=None, nargs='+',
                            help='what benchmarks to filter, all if None')
        parser.add_argument('schema_name', help='Name of the schema to use.')
        graph_schema_fn = lambda args: cls(args).plot_schema()
        parser.set_defaults(fn=graph_schema_fn)

