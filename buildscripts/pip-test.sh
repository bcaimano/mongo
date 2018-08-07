#!/bin/bash

set -euo pipefail

showUsage() {
    cat <<EOF
USAGE:
    pip-test.sh ...
    pip-test.sh -h

This command passes all arguments (besides -h/--help) to pip-install.sh for two invocations,
one for a python2 virtual environment and one for a python3 virtual environment. It then
forms a unified multi-version constrains file at constraints.txt in the current directory.
EOF
}

SCRIPT_DIR="$(cd "$(dirname ${BASH_SOURCE[0]})" && pwd)"
ARGS=()
while [[ $# -gt 0 ]]
do KEY="${1}"
    case "$KEY" in
        (-h|--help) printUsage; exit 0;;
        (*) ARGS+=("${KEY}"); shift;;
    esac
done

testPip(){
    EXE="${1}"
    DIR="${2}"
    if ! (
        virtualenv --python "${EXE}" "${DIR}"
        . (find "${DIR}" --name "activate" -print -quit)
        pip install "${ARGS[@]}"
    )
    then RC=$?
        1>&2 echo "Errors occured while attempting
          to install all requirements into '${DIR}'
          with python executable '${EXE}'"
        exit $RC
    fi
}

PIP2_DIR="${SCRIPT_DIR}/../build/pip-test/python2"
PIP3_DIR="${SCRIPT_DIR}/../build/pip-test/python3"

testPip python2 "${PIP2_DIR}"
testPip python3 "${PIP3_DIR}"

CON_FILE="`pwd`/constraints.txt"
(
    echo "# Common requirements"
    comm -12 "${PIP2_DIR}/requirements.txt" "${PIP3_DIR}/requirements.txt"

    echo -e "\n# Python2 requirements"
    comm -23 "${PIP2_DIR}/requirements.txt" "${PIP3_DIR}/requirements.txt" |
        sed -e 's/$/; python_version < "3"/'

    echo -e "\n# Python3 requirements"
    comm -13 "${PIP2_DIR}/requirements.txt" "${PIP3_DIR}/requirements.txt" |
        sed -e 's/$/; python_version > "3"/'

    echo -e ""
    cat "${SCRIPT_DIR}/../etc/pip/components/platform.req"
) >"${CON_FILE}"

1>&2 echo "All pip requirements were successfully installed in a virtual environment.
See '${CON_FILE}' for all installed packages."
