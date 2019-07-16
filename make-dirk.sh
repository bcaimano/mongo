#!/bin/bash

unset SCONSFLAGS

MONGO_MODULES_DIR="${MONGO_MODULES_DIR:-./src/mongo/db/modules}"

MODULES="dirk"
if [[ -d "${MONGO_MODULES_DIR}/enterprise" ]]; then
    MODULES="${MODULES},enterprise"
fi

set -x

VARIANT_DIR="dirk"

USE_GCC="${USE_GCC:-yes}"
if [[ $USE_GCC == yes ]]; then
    CXXFLAGS+=" -Wno-class-memaccess"
    CXXFLAGS+=" -fno-var-tracking"
    CXXFLAGS+=" -I/opt/mongodbtoolchain/v3/include"
    CXXFLAGS+=" -gsplit-dwarf"
    VAR_FILE="etc/scons/mongodbtoolchain_v3_gcc.vars"
else
    VAR_FILE="etc/scons/mongodbtoolchain_v3_clang.vars"
    VARIANT_DIR="dirk-clang"
fi
CXXFLAGS+=" -fstack-check"

python3 ./buildscripts/scons.py \
    MONGO_VERSION="0.0.0" MONGO_GIT_HASH="unknown" \
    CXXFLAGS="${CXXFLAGS}" \
    CPPPATH=/usr/local/opt/openssl/include:/usr/include/openssl-1.0/ \
    LIBPATH=/usr/local/opt/openssl/lib:/usr/lib/openssl-1.0/ \
    --variables-files=${VAR_FILE} \
    --ssl \
    VERBOSE=on \
    --link-model=dynamic \
    --disable-warnings-as-errors \
    --modules="${MODULES}" \
    VARIANT_DIR="${VARIANT_DIR}" \
    --config=force \
    "${@}" \
    build.ninja
