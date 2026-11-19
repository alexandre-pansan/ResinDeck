#!/bin/sh
# Builds and opens the native ResinDeck (C++ and Qt) on Linux.
#
#   ./build.sh setup        first time: CMake, Ninja and Qt, without admin rights (Qt goes to ~/Qt)
#   ./build.sh              build, optimized, into build/release
#   ./build.sh run [ARGS]   build, then open the window; ARGS go to resindeck (e.g. a .cxdlpv4 file)
#   ./build.sh test         build with checks (warnings as errors, sanitizers) and run the tests
#   ./build.sh clean        delete the build folders
#
# Qt is QT_DIR when set, else the newest ~/Qt/<version>/gcc_64 that has the modules ResinDeck needs.
set -eu
cd "$(dirname "$0")"

QT_VERSION=6.10.3  # the version CI uses, installed by `setup`
QT_MODULES="qtwebsockets qthttpserver qtshadertools"

die() {
    echo "$*" >&2
    exit 1
}

# CMake and Ninja from the system, or from .venv
tools() {
    if [ -x .venv/bin/cmake ]; then PATH="$PWD/.venv/bin:$PATH"; export PATH; fi
    command -v cmake >/dev/null 2>&1 && command -v ninja >/dev/null 2>&1 ||
        die "CMake and Ninja are missing. Run: ./build.sh setup"
}

find_qt() {
    if [ -n "${QT_DIR:-}" ]; then
        [ -f "$QT_DIR/lib/cmake/Qt6HttpServer/Qt6HttpServerConfig.cmake" ] || die "QT_DIR=$QT_DIR has no Qt HttpServer module"
        return
    fi
    for qt_candidate in $(ls -d "$HOME"/Qt/*/gcc_64 2>/dev/null | sort -rV); do
        if [ -f "$qt_candidate/lib/cmake/Qt6HttpServer/Qt6HttpServerConfig.cmake" ] &&
            [ -f "$qt_candidate/lib/cmake/Qt6ShaderTools/Qt6ShaderToolsConfig.cmake" ]; then
            QT_DIR=$qt_candidate
            return
        fi
    done
    die "Qt with the HttpServer and ShaderTools modules was not found in ~/Qt. Run: ./build.sh setup (or set QT_DIR)"
}

check_system() {
    [ -e /usr/include/GL/gl.h ] && [ -e /usr/include/EGL/egl.h ] ||
        die "Qt needs the OpenGL development files. On Debian and Ubuntu: sudo apt install libgl-dev libegl-dev"
}

# sh has no local variables: the names here must not be used by the functions it calls
build() {  # build DIR TYPE [extra cmake options...]
    build_dir=$1 build_type=$2
    shift 2
    case "$build_dir" in build/*) ;; *) die "build folder must be under build/: $build_dir" ;; esac
    tools
    find_qt
    check_system
    echo "Qt: $QT_DIR"
    cmake -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE="$build_type" -DCMAKE_PREFIX_PATH="$QT_DIR" "$@" >/dev/null
    cmake --build "$build_dir"
}

case "${1:-build}" in
setup)
    if [ ! -x .venv/bin/python ]; then
        if command -v uv >/dev/null 2>&1; then uv venv .venv; else python3 -m venv .venv; fi
    fi
    if command -v uv >/dev/null 2>&1; then
        uv pip install --python .venv/bin/python cmake ninja aqtinstall
    else
        .venv/bin/pip install cmake ninja aqtinstall
    fi
    if [ -d "$HOME/Qt/$QT_VERSION/gcc_64" ]; then
        echo "Qt $QT_VERSION is already in ~/Qt"
    else
        (cd "$HOME" && "$OLDPWD/.venv/bin/aqt" install-qt linux desktop "$QT_VERSION" linux_gcc_64 -O "$HOME/Qt" -m $QT_MODULES)
        rm -f "$HOME/aqtinstall.log"
    fi
    check_system
    echo "Ready. Now: ./build.sh run"
    ;;
build)
    build build/release Release
    echo "Built: build/release/app/desktop/resindeck"
    ;;
run)
    shift
    build build/release Release
    exec build/release/app/desktop/resindeck "$@"
    ;;
test)
    build build/cpp Debug -DRESINDECK_WERROR=ON -DRESINDECK_SANITIZE=ON
    ctest --test-dir build/cpp --output-on-failure
    ;;
clean)
    rm -rf build/release build/cpp
    ;;
*)
    sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
