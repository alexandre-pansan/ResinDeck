#!/bin/sh
# Builds and opens the native ResinDeck (C++ and Qt) on Linux.
#
#   ./build.sh              a menu with the commands below (in a terminal; otherwise: build)
#   ./build.sh setup        first time: CMake, Ninja and Qt, without admin rights (Qt goes to ~/Qt)
#   ./build.sh build        build, optimized, into build/release
#   ./build.sh run [ARGS]   build, then open the window; ARGS go to resindeck (e.g. a .cxdlpv4 file)
#   ./build.sh test         build with checks (warnings as errors, sanitizers) and run the tests
#   ./build.sh samples DIR  check the .cxdlpv4 writer against the real files in DIR (slow)
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

# sets cmd (and arg); `set --` inside a function would change only the function's arguments
menu() {
    arg=
    echo "ResinDeck"
    echo "  1) Build and open"
    echo "  2) Build and open a file"
    echo "  3) Build only"
    echo "  4) Run the tests"
    echo "  5) Check the file writer against real .cxdlpv4 files"
    echo "  6) Setup (first time)"
    echo "  7) Clean"
    echo "  0) Quit"
    printf "Choose: "
    read -r choice || exit 0
    case "$choice" in
    1) cmd=run ;;
    2)
        printf ".cxdlpv4 file (drag it here): "
        read -r file || exit 0
        file=$(printf '%s' "$file" | sed "s/^['\"]//; s/['\"] *$//")  # file managers quote dragged paths
        cmd=run arg=$file
        ;;
    3) cmd=build ;;
    4) cmd=test ;;
    5)
        printf "Folder with .cxdlpv4 files: "
        read -r folder || exit 0
        folder=$(printf '%s' "$folder" | sed "s/^['\"]//; s/['\"] *$//; s#^~#$HOME#")
        cmd=samples arg=$folder
        ;;
    6) cmd=setup ;;
    7) cmd=clean ;;
    0 | "") exit 0 ;;
    *) die "No option $choice" ;;
    esac
}

if [ $# -eq 0 ]; then
    if [ -t 0 ] && [ -t 1 ]; then
        menu
        if [ -n "$arg" ]; then set -- "$cmd" "$arg"; else set -- "$cmd"; fi
    else
        set -- build
    fi
fi

case "$1" in
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
samples)
    [ -d "${2:-}" ] || die "Usage: ./build.sh samples FOLDER (a folder of .cxdlpv4 files)"
    build build/release Release
    RESINDECK_SAMPLE_FILES=$2 QTEST_FUNCTION_TIMEOUT=3600000 \
        build/release/core/tests/test_cxdlp rewritesRealFilesExactly
    ;;
clean)
    rm -rf build/release build/cpp
    ;;
*)
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
