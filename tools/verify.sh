#!/usr/bin/env bash
#
# Everything that can be checked without a host, in the order that fails
# fastest.
#
# Each check answers a question none of the others can:
#
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out. A shader that will not
#                 compile presents to an operator as "the effect does
#                 nothing", with the real message buried in the diagnostics
#                 log.
#   demo shaders  is the browser demo running the plugin's own GLSL, character
#                 for character, or has its copy drifted. Nothing else checks
#                 that: vctest has no idea the page exists.
#   identity      does the pyramid reconstruct its input -- exactly, on the
#                 defaults -- and do its bands and residual actually partition
#                 the picture. The first is the plugin's null and the second is
#                 the claim that the bands are a decomposition rather than
#                 merely something that cancels.
#   band          is each band the band-pass the maths says it is: the shipped
#                 shader against the CPU pyramid in Pyramid.cpp, and the
#                 measured peak of its power spectrum.
#   audio         does a sine in audio band k drive picture band k and no
#                 other, under both mappings -- checked as gains, and then
#                 again through the picture against the slider that should be
#                 its equal.
#   envelope      do the followers keep the attack and release times the
#                 controls promise.
#   sweep         does every control change the picture. A GLSL uniform whose
#                 name does not match the C++ is ignored without a word, so
#                 this is the only thing standing between a typo and a shipped
#                 slider that does nothing.
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain.
#   lipo          is the macOS build really universal, or did CMake latch the
#                 architecture list before -DCMAKE_OSX_ARCHITECTURES arrived
#                 and report success anyway.
#   plist         does CFBundleExecutable name the binary that is actually on
#                 disk -- if it does not, codesign reports "code object is not
#                 signed at all" about a *nested* object and mentions neither
#                 the plist nor the cause.
#   codesign      the exact command the release job runs, against a copy.
#   oxbow         what a real host reads out of the bundle: the name, the id
#                 and the type. Nothing else here goes through plugMain.
#   bench         the render cost. Not pass/fail -- there is no threshold
#                 worth asserting on somebody else's GPU -- but a verify run
#                 leaves a timing on the record, which is what turns "it feels
#                 slower" into a comparison.
#
# The last four are release-job work done locally on purpose. A check that only
# runs in CI, after a tag, is a check that will catch you after the tag.
#
# The build is a FRESH UNIVERSAL RELEASE build, from scratch, into build-verify.
# A dev build is arm64 and half the point of the binary checks is the slice it
# does not have.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-verify}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

# A shader may be several adjacent raw strings -- MSVC caps one literal at
# about 16 KB -- so everything up to the terminating semicolon is joined. None
# of this repo's shaders is near the cap today; the join is here so that the
# day one is split, this keeps compiling it rather than silently testing the
# first half.
named = {}
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(\w+)\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is
	# a fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL, and a check
		# that silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

step "shaders"
if shaders_compile; then
	pass "every shader compiles"
else
	fail "a shader does not compile"
fi

#---------------------------------------------------------------------------
# The browser demo's copy of the shaders, against the plugin's.
#
# demo/plugin.js carries a second copy of every shader in source/Shaders.cpp,
# because a browser cannot include a C++ file. Two copies drift, and the drift
# is invisible from both sides -- the plugin keeps working and the page keeps
# working, and they quietly stop being the same effect. The page's whole claim
# is that it runs the plugin's own code.
#---------------------------------------------------------------------------
step "demo shaders"
if [ -f demo/tools/check_shaders.py ]; then
	if python3 demo/tools/check_shaders.py >/tmp/vocoder-demo-shaders.txt 2>&1; then
		pass "$( tail -1 /tmp/vocoder-demo-shaders.txt )"
	else
		fail "the demo's shaders have drifted -- see /tmp/vocoder-demo-shaders.txt"
		grep -E '^FAIL|^ ' /tmp/vocoder-demo-shaders.txt | head -6 | sed 's/^/        /'
	fi
else
	fail "demo/tools/check_shaders.py is missing -- the demo's shader copies are unchecked"
fi

step "build (fresh, universal, Release)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
   && cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "builds from scratch"
else
	fail "build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

step "checks"
for check in identity band audio envelope; do
	if "$BUILD/vctest" --$check >/tmp/vocoder-$check.txt 2>&1; then
		pass "vctest --$check    $( tail -1 /tmp/vocoder-$check.txt )"
	else
		fail "vctest --$check -- see /tmp/vocoder-$check.txt"
		tail -4 /tmp/vocoder-$check.txt | sed 's/^/        /'
	fi
done

step "sweep"
# --binary, because the sweep defaults to build/ and this is build-verify.
if python3 tools/sweep.py --binary "$BUILD/vctest" >/tmp/vocoder-sweep.txt 2>&1; then
	pass "$( tail -1 /tmp/vocoder-sweep.txt )"
else
	fail "a control is dead -- see /tmp/vocoder-sweep.txt"
	tail -4 /tmp/vocoder-sweep.txt | sed 's/^/        /'
fi

BUNDLE="$BUILD/Vocoder.bundle"
BIN="$BUNDLE/Contents/MacOS/Vocoder"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ "$ident" = "com.stoatworks.ffgl.vocoder" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident', expected com.stoatworks.ffgl.vocoder"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Vocoder.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		# What a host reads out of the bundle. `probe` and not `selftest`:
		# selftest renders with no input texture, and this is an EFFECT whose
		# ProcessOpenGL correctly returns FF_FAIL when handed no picture, so a
		# selftest FAIL would say nothing about the plugin.
		out=$("$OXBOW" probe "$( cd "$( dirname "$BUNDLE" )" && pwd )/$( basename "$BUNDLE" )" 2>&1)
		if [ $? -ne 0 ]; then
			fail "oxbow could not load the bundle"
			printf '%s\n' "$out" | sed 's/^/        /'
		else
			for want in "name:        SW Vocoder" "id:          VC01" "type:        effect"; do
				case "$out" in
					*"$want"*) pass "${want}" ;;
					*) fail "oxbow does not report '${want}'"; printf '%s\n' "$out" | head -6 | sed 's/^/        /' ;;
				esac
			done
		fi
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

step "bench (for the record, not pass/fail)"
"$BUILD/vctest" --bench 2>&1 | sed -n '3,8p' | sed 's/^/   /'

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
