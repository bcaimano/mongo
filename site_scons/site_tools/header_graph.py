import SCons
import shlex
import subprocess

SEEN_FILES = set()

def header_graph(env, target, source):
    """Generate a header graph for source."""
    from graphviz import Digraph

    if not isinstance(source, list):
        source = [source]
    source = [str(s) for s in source]
    args = env.subst(
        'gcc -w -std=c11 -std=c++17 -S -Isrc {} -H {}'.format(
            " ".join(["-I" + env.Dir(d).get_abspath() for d in env['CPPPATH']]),
            " ".join(source),
        )
    )

    output = subprocess.run(shlex.split(args), capture_output=True)
    header_graph_lines = []
    for line in output.stderr.decode('utf-8').split('\n'):
        if line == "Multiple include guards may be useful for:":
            break
        header_graph_lines.append(line)

    dot = Digraph(comment='')
    parent_stack = []
    level = 0
    for header in header_graph_lines:
        h_level = header.count(".")
        header_name = header.replace(".", "").strip()
        if h_level <= level:
            for i in range(level, h_level + 1):
                parent_stack.pop()
        dot.node(header_name)
        if parent_stack and parent_stack[-1]:
            dot.edge(parent_stack[-1], header_name)
        parent_stack.append(header_name)
        level = h_level

    dot.render('mats_graph.gv', view=True)
    return []
    

def header_graph_emitter(target, source, env):
    """For each appropriate source file emit a graph builder."""
    for s in source:
        for child in s.sources:
            entry = env.Entry(child)
            suffix = entry.get_suffix()
            if suffix in [".h", ".cpp", ".c", ".hpp"]:
                fn = "graphs/" + str(entry.name)
                if fn in SEEN_FILES:
                    continue
                SEEN_FILES.add(fn)
                env.Alias(
                    "header-graph-{}".format(str(entry.name)),
                    env.HeaderGraph(
                        target=fn,
                        source=entry.get_abspath(),
                    )
                )
    return (target, source)

def exists(env):
    """Always on"""
    return True

def generate(env):
    """Add our builder"""
    env.Append(BUILDERS = {'HeaderGraph': SCons.Builder.Builder(action = header_graph)})

    for builder in ["Program", "SharedLibrary", "LoadableModule", "StaticLibrary"]:
        builder = env["BUILDERS"][builder]
        base_emitter = builder.emitter
        new_emitter = SCons.Builder.ListEmitter([
            base_emitter,
            header_graph_emitter,
        ])
        builder.emitter = new_emitter
