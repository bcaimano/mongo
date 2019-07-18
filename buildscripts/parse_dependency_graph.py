#!/usr/bin/env python3

import sys, yaml, pprint, os.path
import heapq

from collections import defaultdict

pp = pprint.PrettyPrinter(indent=2)

def abridge(nodes, matrix):
    next_bid = max(nodes.keys())+1
    heap = []
    for nid, row in matrix.items():
        heap.append((len(row), nid))

    heapq._heapify_max(heap)
    while heap:
        source_nid = heapq._heappop_max(heap)[1]
        source_row = matrix[source_nid]

        bridge_row = set()
        similar_nid = -1
        for other_nid, other_row in matrix.items():
            if other_nid == source_nid:
                continue

            shared_row = source_row.intersection(other_row)
            if len(shared_row) > len(bridge_row) and len(shared_row) > 1:
                bridge_row = shared_row
                similar_nid = other_nid

        if not bridge_row:
            continue

        #print("{} and {} share {}".format(source_nid, similar_nid, bridge_row))

        bid = next_bid
        next_bid += 1
        nodes[bid] = {
            "isBridge": True,
            "name": "bridge{}".format(bid),
            "leaves": set(),
        }

        for nid, row in matrix.items():
            if not bridge_row.issubset(row):
                continue
            row -= bridge_row
            row.add(bid)
            #print("Updated {} -> {}".format(nid, row))

        #print("New bridge {} -> {}".format(bid, bridge_row))
        matrix[bid] = bridge_row

        heap.append((len(bridge_row), bid))
        heap.append((len(source_row), source_nid))
        heapq._heapify_max(heap)

def condense(nodes, matrix):
    bridges = defaultdict(set)
    for nid, row in matrix.items():
        for edge_nid in row:
            bridges[edge_nid].add(nid)

    for nid, row in bridges.items():
        if not nodes[nid]["isBridge"]:
            continue
        #print("{} <- {}".format(nid, row))
        if len(row) != 1:
            continue

        base_nid = list(row)[0]
        if base_nid < 0:
            matrix[base_nid] |= matrix.pop(nid)

def prune(nodes, matrix):
    back_matrix = defaultdict(set)
    for head_nid, row in matrix.items():
        for tail_nid in row:
            back_matrix[tail_nid].add(head_nid)

    for head_nid, row in matrix.items():
        leaves = []
        for tail_nid in row:
            if len(back_matrix[tail_nid]) == 1 and tail_nid not in matrix:
                leaves.append(tail_nid)

        if len(leaves) == 1:
            continue

        nodes[head_nid]["leaves"] = leaves
        for tail_nid in leaves:
            row.remove(tail_nid)
            del nodes[tail_nid]

def make_filename(path, suffix):
    stem = os.path.splitext(path)[0]
    return '{}-{}.gv'.format(stem, suffix)

from graphviz import Digraph
def render_graph(graph_name, nodes, matrix):
    dot = Digraph("")

    dot.graph_attr["overlap"] = "scale"
    dot.graph_attr["splines"] = "ortho"
    #dot.graph_attr["outputMode"] = "nodesfirst"

    dot.edge_attr["arrowtype"] = "empty" 
    dot.edge_attr["arrowsize"] = "0.5" 

    dot.node_attr["shape"] = "box"
    dot.node_attr["fixedsize"] = "false"
    dot.node_attr["fontsize"] = "10.0"
    dot.node_attr["fontname"] = "arial"
    dot.node_attr["height"] = "0.5"
    dot.node_attr["width"] = "0.5"
    dot.node_attr["style"] = "filled,solid"

    in_edges = defaultdict(int)
    out_edges = defaultdict(int)
    for head_nid, row in matrix.items():
        for tail_nid in row:
            out_edges[head_nid] += 1
            in_edges[tail_nid] += 1
    rendered_nids = set()
    for nid, node in nodes.items():
        name = hex(nid)
        edge_diff = in_edges[nid] - out_edges[nid] - len(node.get('leaves', set()))
        if node["isBridge"]:
            dot.node(name, label='{:+d}'.format(edge_diff), shape="diamond", width="0.5", height="0.5") 
            #dot.node(name, label=str(nid)) 
        elif in_edges[nid] == 0:
            dot.node(name, label=node["name"], fillcolor="red", rank="source", shape="invhouse") 
        elif nid not in matrix:
            dot.node(name, label=node["name"], fillcolor="green", rank="sink")
        else:
            dot.node(name, label='{}\n{:+d}'.format(node["name"], edge_diff), fillcolor="red")
        rendered_nids.add(nid)

    for nid, row in matrix.items():
        node = nodes[nid]
        name = hex(nid)

        if node["leaves"]:
            leaf_name = "{}-leaves".format(name)
            label = str(len(node["leaves"]))
            dot.node(leaf_name, label=label, fillcolor="green", rank="sink", shape="octagon") 
            dot.edge(name, leaf_name)

        for dep_nid in row:
            dep_name = hex(dep_nid)
            dot.edge(name, dep_name)

    with open(graph_name, 'w') as f:
        f.write(dot.source)

def make_cluster(nodes, matrix):
    cluster_nodes = {}
    name2nid = {}
    nextnid = 0
    for _, node in nodes.items():
        if "cluster" in node:
            cluster = node["cluster"]

            if cluster not in name2nid:
                nid = nextnid
                nextnid += 1
                name2nid[cluster] = nid
                cluster_nodes[nid] = { "name": cluster, "count": 0, "isBridge": False, "leaves":
                        set() }

            cluster_nodes[nid]["count"] += 1

    cluster_matrix = defaultdict(set)
    for original_nid, row in matrix.items():
        head_node = nodes[original_nid]
        head_cluster = head_node["cluster"]
        head_nid = name2nid[head_cluster]

        for tail_nid in row:
            tail_node = nodes[tail_nid]
            tail_cluster = tail_node["cluster"]
            tail_nid = name2nid[tail_cluster]

            if head_nid == tail_nid:
                continue

            cluster_matrix[head_nid].add(tail_nid)

    return (cluster_nodes, cluster_matrix)

def parse_graph(path, graph):
    nodes = graph["nodes"]
    for nid, node in nodes.items():
        node["isBridge"] = False
        node["leaves"] = set()
    #pp.pprint(nodes)

    matrix = {}
    for nid, row in graph["matrix"].items():
        matrix[nid] = set(row)

    render_graph(make_filename(path, "original"), nodes, matrix)

    cluster_nodes, cluster_matrix = make_cluster(nodes, matrix)
    abridge(cluster_nodes, cluster_matrix)
    condense(cluster_nodes, cluster_matrix)
    render_graph(make_filename(path, "cluster"), cluster_nodes, cluster_matrix)

    abridge(nodes, matrix)
    condense(nodes, matrix)
    prune(nodes, matrix)
    render_graph(make_filename(path, "abridged"), nodes, matrix)

for path in sys.argv[1:]:
    with open(path) as f:
        graph = yaml.load(f)
    parse_graph(path, graph)
