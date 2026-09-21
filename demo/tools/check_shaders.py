"""The demo's GLSL must be the plugin's GLSL, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.

------------------------------------------------------------------- the point

`demo/plugin.js` carries a second copy of all five shaders in
`source/Shaders.cpp`, because a browser cannot include a C++ file. Two copies of
a shader is exactly the arrangement that drifts, and the drift is invisible from
both sides: the plugin keeps working, the page keeps working, and they quietly
stop being the same effect. The page's whole claim is that what it runs is the
plugin's own code, so the moment that stops being checkable the page is a lie.

Nothing else can check it. `vctest` drives the real plugin class through the
real FFGL sequence and has no idea this page exists.

This compares the text, not the behaviour. Reformatting counts as drift, and
that is deliberate -- "it is only whitespace" is how a real change gets waved
through, and the comments in these shaders carry the reasoning that justifies
the code: why the expand loops over three candidates rather than branching on
parity, why the difference at every tap is what makes the null exact.

--------------------------------------------------------------- two mechanics

**Adjacent literals are joined.** MSVC caps a single raw string at about 16 KB,
so a shader that outgrows it has to be written as several `R"(...)"` pieces in a
row. None of this repo's shaders is near the cap today; the join is here so that
the day one is split, this keeps comparing the whole thing rather than silently
comparing the first half.

**One escape is decoded.** `kExpandHShader` quotes a variable name in a comment
with backticks, and a backtick cannot appear raw inside a JavaScript template
literal, so `plugin.js` escapes it as \\`. This undoes that one escape and
*rejects any other backslash on the JS side* -- there is no backslash anywhere
in the C++, so a second escape could only be somebody hiding a difference.

--------------------------------------------------------------- what it cannot

Nothing here checks the *ported* arithmetic. `bandGain`, `tiltDbPerBand`,
`masterGain`, `compose`, `reducedSize` and `activeLevels` in plugin.js are a
hand translation of `source/Controls.cpp` and `source/Pyramid.cpp`, and only a
reader can tell whether they still agree. When you change a mapping, change it
there too -- and remember that a wrong one shows up on the page as a band gain
that is subtly the wrong multiplier, which nobody will notice.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# JS constant, C++ file, C++ symbol.
SHADERS = [
    ("VERTEX", "source/Shaders.cpp", "kVertexShader"),
    ("COPY", "source/Shaders.cpp", "kCopyShader"),
    ("REDUCE", "source/Shaders.cpp", "kReduceShader"),
    ("EXPAND_V", "source/Shaders.cpp", "kExpandVShader"),
    ("EXPAND_H", "source/Shaders.cpp", "kExpandHShader"),
]


def from_cpp(path, symbol):
    """The body of `const char* const symbol = R"( ... )";`, with any adjacent
    raw-string pieces joined."""
    with open(os.path.join(REPO, path)) as handle:
        source = handle.read()
    match = re.search(
        r'const char\* const\s+' + re.escape(symbol)
        + r'\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;',
        source,
        re.S,
    )
    if match is None:
        return None
    return "".join(re.findall(r'R"\((.*?)\)"', match.group(1), re.S))


def from_js(source, name):
    """The body of ``const NAME = `...`;``, with the one backtick escape undone."""
    match = re.search(r'^const ' + re.escape(name) + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None

    body = match.group(1)

    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        line = body[: stray.start()].count("\n") + 1
        return None, f"backslash that is not an escaped backtick, at line {line}"

    return body.replace("\\`", "`"), None


def main():
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()

    problems = 0
    for name, path, symbol in SHADERS:
        cpp_text = from_cpp(path, symbol)
        js_text, complaint = from_js(js, name)

        if cpp_text is None:
            print(f"FAIL  {symbol} not found in {path}")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue
        if "${" in cpp_text:
            # A `${` in the GLSL would be a template substitution on the JS side,
            # and the mismatch would be reported here rather than at its cause.
            print(f"FAIL  {symbol} contains ${{ -- it cannot be a JS template literal")
            problems += 1
            continue

        if cpp_text == js_text:
            print(f"ok    {name:<10} matches {symbol} ({len(cpp_text)} chars)")
            continue

        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in {path}")

        cpp_lines = cpp_text.splitlines()
        js_lines = js_text.splitlines()
        for i in range(max(len(cpp_lines), len(js_lines))):
            a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
            b = js_lines[i] if i < len(js_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a!r}")
                print(f"          js : {b!r}")
                break

    print()
    if problems:
        print(f"{problems} shader(s) differ -- copy the C++ across, do not edit plugin.js by hand")
        return 1

    print(f"all {len(SHADERS)} shaders are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
