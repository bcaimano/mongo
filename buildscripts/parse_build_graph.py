#!/usr/bin/env python3

import sys, yaml, pprint
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

        print("{} and {} share {}".format(source_nid, similar_nid, bridge_row))

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
            print("Updated {} -> {}".format(nid, row))

        print("New bridge {} -> {}".format(bid, bridge_row))
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
        print("{} <- {}".format(nid, row))
        if len(row) != 1:
            continue

        base_nid = list(row)[0]
        if base_nid < 0:
            matrix[base_nid] |= matrix.pop(nid)

def prune(nodes, matrix):
    leaves = defaultdict(set)
    for nid, row in matrix.items():
        for edge_nid in row:
            leaves[edge_nid].add(nid)

    for nid, row in leaves.items():
        if len(row) != 1:
            continue

        base_nid = list(row)[0]
        if nid not in matrix:
            matrix[base_nid].remove(nid)
            nodes[base_nid]["leaves"].add(nid)
            del nodes[nid]

graph = {}
with open(sys.argv[1]) as f:
    graph = yaml.load(f)

nodes = graph["nodes"]
for nid, node in nodes.items():
    node["isBridge"] = False
    node["inEdges"] = 0
    node["leaves"] = set()
#pp.pprint(nodes)

matrix = {}
for nid, row in graph["matrix"].items():
    for row_nid in row:
        nodes[row_nid]["inEdges"] += 1
    matrix[nid] = set(row)

abridge(nodes, matrix)
condense(nodes, matrix)
prune(nodes, matrix)

from graphviz import Digraph
dot = Digraph("")

dot.graph_attr["overlap"] = "scale"
dot.graph_attr["splines"] = "ortho"
dot.graph_attr["outputMode"] = "nodesfirst"

dot.edge_attr["arrowtype"] = "empty" 
dot.edge_attr["arrowsize"] = "0.5" 

dot.node_attr["shape"] = "box"
dot.node_attr["fixedsize"] = "true"
dot.node_attr["fontsize"] = "10.0"
dot.node_attr["fontname"] = "arial"
dot.node_attr["height"] = "0.25"
dot.node_attr["width"] = "0.325"
dot.node_attr["style"] = "filled,solid"

for nid, node in nodes.items():
    name = hex(nid)
    if node["isBridge"]:
        #dot.node(name, shape="point", width="0.0625", height="0.0625") 
        dot.node(name, label=str(nid)) 
    elif node["inEdges"] == 0:
        dot.node(name, fillcolor="red", rank="source", shape="invhouse") 
    elif nid not in matrix:
        dot.node(name, fillcolor="green", rank="sink")
    else:
        dot.node(name, fillcolor="red")

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

dot.render("deps.gv")
