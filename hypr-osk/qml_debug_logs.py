#!/usr/bin/python3
"""Keep or drop shell console.log calls based on the hypr-osk build type.

Quickshell loads QML as source, so a runtime check cannot remove a log.
Release deploys delete every console.log statement. Debug deploys replace
`// @@name@@` anchors with the matching block from qml-debug-snippets.txt.
console.warn is left alone.
"""

import argparse
import pathlib
import sys

ANCHOR = "// @@"
SNIPPETS = pathlib.Path(__file__).with_name("qml-debug-snippets.txt")


def load_snippets(path):
    snippets = {}
    name = None
    buf = []
    for line in path.read_text().splitlines():
        if line.startswith("@@") and line.endswith("@@") and len(line) > 4:
            if name is not None:
                snippets[name] = "\n".join(buf).rstrip("\n")
            name = line[2:-2]
            buf = []
        elif name is not None:
            buf.append(line)
    if name is not None:
        snippets[name] = "\n".join(buf).rstrip("\n")
    return snippets


def strip_console_log(text):
    out = []
    i = 0
    needle = "console.log"
    while True:
        j = text.find(needle, i)
        if j < 0:
            out.append(text[i:])
            break
        # Keep the call when it is inside a comment. Anchors never contain it.
        line_start = text.rfind("\n", 0, j) + 1
        prefix = text[line_start:j]
        if "//" in prefix:
            out.append(text[i:j + len(needle)])
            i = j + len(needle)
            continue
        paren = text.find("(", j)
        if paren < 0:
            raise SystemExit("console.log without '('")
        depth = 0
        k = paren
        quote = None
        while k < len(text):
            c = text[k]
            if quote:
                if c == "\\" and k + 1 < len(text):
                    k += 2
                    continue
                if c == quote:
                    quote = None
            elif c in ("'", '"', "`"):
                quote = c
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    k += 1
                    break
            k += 1
        else:
            raise SystemExit("unclosed console.log call")
        # Drop the indent on this line when the call is the whole statement.
        # A bare call, or `Handler: console.log(...)`, is the whole statement.
        start = j
        if prefix.strip() == "" or prefix.strip().endswith(":"):
            start = line_start
        end = k
        if end < len(text) and text[end] == "\n":
            end += 1
        out.append(text[i:start])
        i = end
    return "".join(out)


def apply_anchors(text, snippets, debug):
    lines = text.splitlines(keepends=True)
    out = []
    for line in lines:
        bare = line.strip()
        if bare.startswith(ANCHOR) and bare.endswith("@@"):
            name = bare[len(ANCHOR):-2]
            if debug:
                if name not in snippets:
                    raise SystemExit(f"missing qml debug snippet: {name}")
                nl = "\n" if line.endswith("\n") else ""
                out.append(snippets[name] + nl)
                continue
        out.append(line)
    return "".join(out)


def process_tree(root, debug, snippets):
    for path in sorted(root.rglob("*.qml")):
        text = path.read_text()
        if not debug:
            text = strip_console_log(text)
        text = apply_anchors(text, snippets, debug)
        if not debug and "console.log" in text:
            raise SystemExit(f"console.log survived release filter: {path}")
        path.write_text(text)


def self_test():
    sample = (
        '    console.log("a" +\n'
        '                "b")\n'
        "    send()\n"
        "    // @@loaded@@\n"
        '    console.warn("keep")\n'
        '    Component.onCompleted: console.log("gone")\n'
    )
    stripped = strip_console_log(sample)
    if "console.log" in stripped or "console.warn" not in stripped or "send()" not in stripped:
        raise SystemExit(f"strip failed:\n{stripped}")
    snippets = {"loaded": '    console.log("rev")'}
    debug = apply_anchors(stripped, snippets, True)
    if 'console.log("rev")' not in debug:
        raise SystemExit("anchor insert failed")
    release = apply_anchors(sample, snippets, False)
    release = strip_console_log(release)
    if "console.log" in release:
        raise SystemExit("release still has console.log")
    print("qml_debug_logs self-test ok")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("release", "debug"))
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("tree", nargs="?", type=pathlib.Path)
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if args.mode is None or args.tree is None:
        parser.error("--mode and tree are required")
    snippets = load_snippets(SNIPPETS)
    process_tree(args.tree, args.mode == "debug", snippets)


if __name__ == "__main__":
    main()
