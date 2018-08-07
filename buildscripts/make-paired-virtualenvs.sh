#/bin/bash

set -eu

showUsage() {
    cat <<EOF
USAGE:
    make-paired-virtualenvs.sh
    make-paired-virtualenvs.sh -h

This command creates python2/python3 virtualenvs at \$VENV2_DIR/\$VENV3_DIR using \$VIRTUALENV.
It then enforces python2 and python3 symlinks in the directory that \$VENV2_DIR adds to the path when activated.

On Windows, it defaults to the following python executables:
c:/python/Python2*/python.exe
c:/python/Python3*/python.exe

Otherwise, it defaults to the following python executables:
/opt/mongodbtoolchain/v2/bin/python2
/opt/mongodbtoolchain/v2/bin/python3

These defaults can be override via \$PY2_EXE and/or \$PY3_EXE.
EOF
}

if [[ -n ${1:-} ]]; then
    if [[ ${1} == '-h' ]]; then
        showUsage
        exit 0
    else
        1>&2 echo "Unrecognized arguments: ${@}"
        exit 1
    fi
fi

set -v

# exit immediately if virtualenv is not found
virtualenv_loc=$(which ${VIRTUALENV})
venv2_dir="$(mkdir -p "${VENV2_DIR}" && cd "${VENV2_DIR}" && pwd)"
venv3_dir="$(mkdir -p "${VENV3_DIR}" && cd "${VENV3_DIR}" && pwd)"

# Chose our python2 executable
if [[ -n "${PY2_EXE:-}" ]]; then
    python2_loc="${PY2_EXE}"
elif [[ "Windows_NT" == "${OS:-}" ]]; then
    python2_loc="$(cygpath -w "$(ls c:/python/Python2*/python.exe | tail -n1)")"
else
    python2_loc=/opt/mongodbtoolchain/v2/bin/python2
fi

# Chose our python3 executable
if [[ -n "${PY3_EXE:-}" ]]; then
    python3_loc="${PY3_EXE}"
elif [[ "Windows_NT" == "${OS:-}" ]]; then
    python3_loc="$(cygpath -w "$(ls c:/python/Python3*/python.exe | tail -n1)")"
else
    python3_loc=/opt/mongodbtoolchain/v2/bin/python3
fi

# Set up virtualenv in ${WORKDIR}
"$virtualenv_loc" --python "$python2_loc" --system-site-packages "$venv2_dir"
# Add virtualenv for python3 in ${WORKDIR}
"$virtualenv_loc" --python "$python3_loc" --system-site-packages "$venv3_dir"

# Find our virtualenv bin dirs by sniffing out python
venv2_bin="$(find "$venv2_dir" -iname python -xtype f -executable -exec dirname {} \;)"
venv3_bin="$(find "$venv3_dir" -iname python -xtype f -executable -exec dirname {} \;)"

if [[ -z "$venv2_bin" || -z "$venv3_bin" ]]; then
1>&2 echo "Unable to find python in virtual environment."
exit 1
fi

# Force pip to a version that has pip.__main__
"$venv2_bin/pip" install --ignore-installed pip>=10.0.0
"$venv3_bin/pip" install --ignore-installed pip>=10.0.0

# Seed our python2 virtualenv with symlinks so that:
# * There is always a python2 link (windows doesn't have one automatically)
# * There is a python3 link (the bin dir will be in the path, the executable doesn't care)
if [[ ! -f "$venv2_bin/python2" ]]; then
ln -sv "$venv2_bin/python" "$venv2_bin/python2"
fi
if [[ ! -f "$venv2_bin/python3" ]]; then
ln -sv "$venv3_bin/python" "$venv2_bin/python3"
fi
