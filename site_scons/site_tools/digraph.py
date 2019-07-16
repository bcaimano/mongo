# TODO: Versioned libraries
# TODO: library dependency chaining for windows dynamic builds, static dev packages
# TODO: Injectible component dependencies (jscore -> resmoke, etc.)
# TODO: Handle chmod state
# TODO: Installing resmoke and configurations
# TODO: package decomposition
# TODO: Install/package target help text
# TODO: implement sdk_headers

import os
import shlex
import itertools
from collections import defaultdict, namedtuple

import SCons
from SCons.Tool import install

def do_nothing(target, src, env):
    return []

def digraph_emitter(target, src, env):
    print("{} {}", target, src)
    return (target,src)

def exists(_env):
    """Always activate this tool."""
    return True

def generate(env):  # pylint: disable=too-many-statements
    """Generate the auto install builders."""

    for builder in ["Program", "SharedLibrary", "LoadableModule", "StaticLibrary"]:
        builder = env["BUILDERS"][builder]
        base_emitter = builder.emitter
        new_emitter = SCons.Builder.ListEmitter([
            base_emitter,
            digraph_emitter,
        ])
        builder.emitter = new_emitter
    env.Alias("dummy", [], [do_nothing])
