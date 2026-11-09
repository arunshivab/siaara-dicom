#!/usr/bin/env python3
"""CAPI-03 check (review R-013): no C++ exception may cross the C boundary.

Every exported function (SIAARA_API) defined in src/capi/*.cpp must either
  - have a body that is exactly one statement, `return guarded(...);` (or
    `return guarded_value<T>(...);` for exports that return a value), or
  - be declared SIAARA_NOEXCEPT (noexcept in C++) and have a one-statement body
    (trivial getters and *_free functions that only delete).
Exits 1 and lists the offending functions otherwise.

Usage: check_capi_guarded.py [repo-root]
"""
import os
import re
import sys


def strip_comments_and_literals(src):
    """Blanks out comments, string and character literals, keeping offsets (and newlines)."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c == 'R' and src.startswith('R"', i):
            m = re.match(r'R"([^(]*)\(', src[i:])
            if not m:
                i += 1
                continue
            end = src.find(")" + m.group(1) + '"', i)
            end = n if end < 0 else end + len(m.group(1)) + 2
            for k in range(i + 1, end):
                if out[k] != "\n":
                    out[k] = " "
            i = end
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):
                out[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(out)


def top_level_statements(body):
    """Counts ';' at brace/paren depth 0 inside a function body."""
    depth = 0
    count = 0
    for ch in body:
        if ch in "({[":
            depth += 1
        elif ch in ")}]":
            depth -= 1
        elif ch == ";" and depth == 0:
            count += 1
    return count


def check_file(path):
    src = open(path, encoding="utf-8").read()
    clean = strip_comments_and_literals(src)
    problems = []
    checked = 0
    for m in re.finditer(r"\bSIAARA_API\b", clean):
        # Signature: up to the first '{' or ';' at paren depth 0.
        i, depth = m.end(), 0
        while i < len(clean):
            ch = clean[i]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            elif depth == 0 and ch in "{;":
                break
            i += 1
        if i >= len(clean) or clean[i] == ";":
            continue  # a declaration, not a definition
        sig = clean[m.end():i]
        name_m = re.search(r"(\w+)\s*\(", sig)
        name = name_m.group(1) if name_m else "?"
        # Body: matching brace.
        j, depth = i, 0
        while j < len(clean):
            if clean[j] == "{":
                depth += 1
            elif clean[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        body = clean[i + 1:j].strip()
        checked += 1
        line = src.count("\n", 0, m.start()) + 1
        if re.fullmatch(r"return\s+guarded(_value\s*<[^>]*>)?\s*\(.*\)\s*;", body, re.S) and top_level_statements(body) == 1:
            continue
        noexcept = re.search(r"\bSIAARA_NOEXCEPT\b|\bnoexcept\b", sig) is not None
        if noexcept and top_level_statements(body) <= 1 and "{" not in body:
            continue
        problems.append("%s:%d: %s: body is neither `return guarded(...);` nor a one-statement SIAARA_NOEXCEPT function"
                        % (os.path.relpath(path), line, name))
    return checked, problems


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    capi = os.path.join(root, "src", "capi")
    total, problems = 0, []
    for fn in sorted(os.listdir(capi)):
        if fn.endswith(".cpp"):
            c, p = check_file(os.path.join(capi, fn))
            total += c
            problems += p
    for p in problems:
        print(p)
    print("check_capi_guarded: %d exported functions checked, %d not guarded" % (total, len(problems)))
    if total == 0:
        print("check_capi_guarded: no exported functions found (wrong path?)")
        return 1
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
