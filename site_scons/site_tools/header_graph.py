import os.path
import SCons
import shlex
import subprocess
import yaml

from collections import defaultdict

SEEN_FILES = set()
link_matrix = defaultdict(set) 
link_dirs = {}

def _dump_graph_yaml(path, nodes, matrix):
    out_matrix = {}
    for nid, row in matrix.items():
        out_matrix[nid] = list(row)

    with open(path, "w") as f:
        print("==Writing graph to {}==".format(path))
        yaml.dump({
            'nodes': nodes,
            'matrix': out_matrix
        }, f, default_flow_style=True)

def link_graph(env, target, source):
    entry = env.Entry(target[0])
    target_name = str(entry.name)
    target_name = target_name[:target_name.rfind('.')]
    
    print("==Forming build graph for {}==".format(target_name))

    matrix = defaultdict(set)

    nids_by_name = {target_name: 0}
    name_stack = [(0, target_name)]

    next_nid = 1
    while name_stack:
        nid, name = name_stack.pop()
        row = link_matrix[name]
        #print(name, row)
        for dep_name in row:
            if dep_name in nids_by_name:
                matrix[nid].add(nids_by_name[dep_name])
                continue

            dep_nid = next_nid
            next_nid += 1
            nids_by_name[dep_name] = dep_nid
            matrix[nid].add(dep_nid)

            name_stack.append((dep_nid, dep_name))

    nodes = {}
    for name, nid in nids_by_name.items():
        nodes[nid] = {
            "name": name,
            "cluster": link_dirs[name],
        }

    entry = env.Entry(target[0])
    path = entry.get_abspath()
    _dump_graph_yaml(path, nodes, matrix)

    return []

def _get_header_includes(env, filepath):
    cmd = [
        'gcc',
        '-o',
        '/dev/null',
        '-w',
        '-std=c11',
        '-std=c++17',
        '-E',
        '-Isrc'
    ]

    cmd += ["-I" + env.Dir(d).get_abspath() for d in env['CPPPATH']]
    cmd.append('-H')
    cmd.append(filepath)
    #print(" ".join(cmd))

    args = shlex.split(env.subst(" ".join(cmd)))

    output = subprocess.run(args, capture_output=True)

    header_graph_lines = []
    for line in output.stderr.decode('utf-8').split('\n'):
        if not line or line[0] != '.':
            continue
        if line == "Multiple include guards may be useful for:":
            break

        header_path = line.lstrip('.')
        header_level = len(line) - len(header_path)
        header_graph_lines.append((header_level, header_path.strip()))
    return header_graph_lines

def header_graph(env, target, source):
    """Generate a header graph for source."""

    paths = set()
    edges = set()

    build_dir = env.Dir('$BUILD_ROOT/$VARIANT_DIR').get_abspath() + '/'
    src_dir = env.Dir('#src').get_abspath() + '/'
    for s in source:
        source_path = s.get_abspath().replace(build_dir, '').replace(src_dir, '')
        source_path = source_path.replace('src/mongo/','')
        paths.add(source_path)
        parent_stack = [source_path]
        last_level = 0
        
        for (level, header_path) in _get_header_includes(env, s.get_abspath()):
            if level == 0:
                continue

            header_path = os.path.abspath(header_path)
            header_path = header_path.replace(build_dir, '').replace(src_dir, '')
            header_path = header_path.replace("mongo/", '')

            #print("path: {}, level: {}".format(header_path, level))
            for i in range(level, last_level + 1):
                last_path = parent_stack.pop()
                #print("Popped {}".format(last_path))
            last_level = level
            parent_stack.append(header_path)
            
            if header_path.startswith("third_party/"):
                continue
            
            if "/usr/" in header_path:
                continue


            edges.add((parent_stack[-2], header_path))
            paths.add(header_path)

    matrix = {}
    nodes = {}

    nodes_by_id = {}
    next_nid = 0
    for path in paths:
        nid = next_nid
        next_nid += 1

        cluster = os.path.dirname(path)
        nodes[nid] = {"name": path, 'cluster': cluster }

        nodes_by_id[path] = nid

    for edge in edges:
        head_nid = nodes_by_id[edge[0]]
        tail_nid = nodes_by_id[edge[1]]

        if head_nid not in matrix:
            matrix[head_nid] = set()
        matrix[head_nid].add(tail_nid)

    entry = env.Entry(target[0])
    _dump_graph_yaml(str(entry), nodes, matrix)
    return []
    

def header_graph_emitter(target, source, env):
    """For each appropriate source file emit a graph builder."""

    build_dir = env.Dir('$BUILD_ROOT/$VARIANT_DIR').get_abspath() + '/'

    source_files = []
    for s in source:
        for child in s.sources:
            entry = env.Entry(child)
            suffix = entry.get_suffix()
            if suffix in [".h", ".cpp", ".c", ".hpp"]:
                source_files.append(entry)

    if not source_files:
        return (target, source)

    for t in target:
        entry = env.Entry(t)
        path = "{}graphs/include/{}.yml".format(build_dir, str(entry.name))

        if path in SEEN_FILES:
            continue
        SEEN_FILES.add(path)

        target_name = str(entry.name)
        alias_name = "header-graph-{}".format(target_name)
        infixes = {
            '.so': 'shlib',
            '.a': 'lib',
        }
        target_suffix = entry.get_suffix()
        if target_suffix in infixes:
            alias_name = "header-graph-{}-{}".format(infixes[target_suffix], target_name[3:-3])

        env.Alias(
            alias_name,
            env.HeaderGraph(
                target=path,
                source=source_files,
            )
        )

    return (target, source)

link_targets = set()
def link_graph_emitter(target, source, env):
    """For each appropriate source file emit a graph builder."""

    build_dir = env.Dir('$BUILD_ROOT/$VARIANT_DIR').get_abspath() + '/'
    #print("BUILD_DIR={}".format(build_dir))
    get_libdeps = env['_LIBDEPS_GET_LIBS']
    libdeps = get_libdeps(source, target, env, False)

    for t in target:
        target_entry = env.Entry(t)
        target_name = str(target_entry.name)

        for libdep in libdeps:
            entry = env.Entry(libdep)
            link_matrix[target_name].add(str(entry.name))

        if target_name in link_targets:
            continue

        target_dir  = os.path.dirname(target_entry.get_abspath().replace(build_dir,''))
        link_dirs[target_name] = target_dir

        alias_name = "link-graph-{}".format(target_name)
        infixes = {
            '.so': 'shlib',
            '.a': 'lib',
        }
        target_suffix = target_entry.get_suffix()
        if target_suffix in infixes:
            alias_name = "link-graph-{}-{}".format(infixes[target_suffix], target_name[3:-3])
        link_targets.add(target_name)

        graph_path = "{}graphs/link/{}.yml".format(build_dir, target_name)
        env.Alias(
            alias_name,
            env.LinkGraph(
                target=graph_path,
                source=[],
            )
        )

    return (target, source)

def exists(env):
    """Always on"""
    return True

def generate(env):
    """Add our builder"""
    env.Append(BUILDERS = {'HeaderGraph': SCons.Builder.Builder(action = header_graph)})
    env.Append(BUILDERS = {'LinkGraph': SCons.Builder.Builder(action = link_graph)})

    for builder in ["Program", "SharedLibrary", "LoadableModule", "StaticLibrary"]:
        builder = env["BUILDERS"][builder]
        base_emitter = builder.emitter
        new_emitter = SCons.Builder.ListEmitter([
            base_emitter,
            header_graph_emitter,
            link_graph_emitter,
        ])
        builder.emitter = new_emitter
