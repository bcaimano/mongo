import SCons
import shlex
import subprocess

SEEN_FILES = set()

def header_graph(env, target, source):
    """Generate a header graph for source."""
    if not isinstance(source, list):
        source = [source]
    source = [str(s) for s in source]
    args = env.subst(
        'gcc $CFLAGS $CCFLAGS $CXXFLAGS -Isrc {} -H {}'.format(
            " ".join(["-I" + env.Dir(d).get_abspath() for d in env['CPPPATH']]),
            " ".join(source),
        )
    )

    print(args)
    output = subprocess.check_output(shlex.split(args))
    import pdb; pdb.set_trace()

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
