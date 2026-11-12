"""Check the docs against each other and against the code (the docs-check skill).

Mechanical checks only; the judgement part (contradictions, duplicated facts) is in
.claude/skills/docs-check/SKILL.md. Run from anywhere: python tools/check_docs.py
Exit code 1 if anything is reported.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Dated or historical files: their links are checked, their code references are not (they describe the past).
HISTORICAL = ("docs/history.md", "docs/research/")

# Names in backticks that live outside the repo, so the code-name check skips them.
EXTERNAL_NAMES = {
    "idf.py", "compile_commands.json",          # ESP-IDF tool, build output
    "NWK_TABLE_FULL", "UNSUPPORTED_ATTRIBUTE",  # Zigbee / ZCL status names
}
EXTERNAL_PREFIXES = ("config/", "data/", "custom_templates/",  # paths on the HA / Z2M host
                     "ha_config_")                             # ha-mcp tools (the repo's own ha_input_* stay checked)

problems = []


def report(where, msg):
    problems.append(f"{where}: {msg}")


def tracked_files():
    out = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
    return [p for p in out.splitlines() if (ROOT / p).is_file()]


FILES = tracked_files()
FILE_SET = set(FILES)
BASENAMES = {Path(p).name for p in FILES}
TEXT_EXT = {".md", ".c", ".h", ".py", ".mjs", ".yaml", ".yml", ".jinja", ".txt", ".html", ".json"}


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8", errors="replace")


# ---------------------------------------------------------------- anchors

def slug(heading):
    """GitHub's heading anchor: lower case, drop punctuation except - and _, spaces to -."""
    s = re.sub(r"`|\*\*|\*|\[|\]\([^)]*\)", "", heading.strip()).lower()
    s = re.sub(r"[^\w\- ]", "", s)
    return s.replace(" ", "-")


_anchor_cache = {}


def anchors(rel):
    if rel not in _anchor_cache:
        found, counts, in_code = set(), {}, False
        for line in read(rel).splitlines():
            if line.startswith("```"):
                in_code = not in_code
            m = None if in_code else re.match(r"#{1,6}\s+(.*?)\s*#*\s*$", line)
            if m:
                s = slug(m.group(1))
                n = counts.get(s, 0)
                counts[s] = n + 1
                found.add(s if n == 0 else f"{s}-{n}")
        _anchor_cache[rel] = found
    return _anchor_cache[rel]


def check_target(where, base_dir, target):
    """target: a relative path with an optional #anchor, resolved against base_dir (a repo-relative dir)."""
    path, _, anchor = target.partition("#")
    if path:
        rel = (Path(base_dir) / path).as_posix()
        rel = Path(rel).as_posix()
        norm = (ROOT / rel).resolve()
        try:
            rel = norm.relative_to(ROOT).as_posix()
        except ValueError:
            report(where, f"link leaves the repo: {target}")
            return
        if not norm.exists():
            report(where, f"broken link: {target}")
            return
        if norm.is_dir() or not rel.endswith(".md"):
            if anchor:
                report(where, f"anchor on a non-markdown target: {target}")
            return
    else:
        rel = where.split(":")[0]
    if anchor and anchor not in anchors(rel):
        report(where, f"no heading #{anchor} in {rel}")


# ---------------------------------------------------------------- 1. markdown links

MD_LINK = re.compile(r"(?<!!)\[[^\]]*\]\(([^)\s]+)\)|!\[[^\]]*\]\(([^)\s]+)\)")


def iter_lines_outside_code(rel):
    in_code = False
    for n, line in enumerate(read(rel).splitlines(), 1):
        if line.lstrip().startswith("```"):
            in_code = not in_code
            continue
        if not in_code:
            yield n, line


def check_markdown_links():
    for rel in FILES:
        if not rel.endswith(".md"):
            continue
        base = Path(rel).parent.as_posix()
        for n, line in iter_lines_outside_code(rel):
            for m in MD_LINK.finditer(line):
                target = m.group(1) or m.group(2)
                if re.match(r"[a-z]+:", target):  # http:, https:, mailto:
                    continue
                check_target(f"{rel}:{n}", base, target)


# ---------------------------------------------------------------- 2. bare docs/... references (code comments, skills)

DOC_REF = re.compile(r"(?<![\w/])(?<!\]\()docs/[\w./-]+\.md(?:#[\w-]+)?((?:\s*,\s*#[\w-]+)*)")


def check_doc_refs():
    for rel in FILES:
        if Path(rel).suffix not in TEXT_EXT:
            continue
        for n, line in enumerate(read(rel).splitlines(), 1):
            for m in DOC_REF.finditer(line):
                ref = m.group(0).split(",")[0].strip()
                check_target(f"{rel}:{n}", ".", ref)
                doc = ref.split("#")[0]
                for extra in re.findall(r"#([\w-]+)", m.group(1) or ""):
                    check_target(f"{rel}:{n}", ".", f"{doc}#{extra}")


# ---------------------------------------------------------------- 3. code names in the design docs

PATHLIKE = re.compile(r"^[\w.-]+(/[\w.-]+)*/?$")
KNOWN_EXT = {".c", ".h", ".py", ".md", ".mjs", ".yaml", ".yml", ".jinja", ".json", ".html", ".png",
             ".txt", ".csv", ".ps1", ".sh"}


def design_docs():
    docs = [p for p in FILES if p.endswith(".md") and (p.startswith("docs/") or p.startswith(".claude/skills/"))]
    docs += ["CLAUDE.md", "README.md"]
    return [p for p in docs if p in FILE_SET and not p.startswith(HISTORICAL)]


def code_corpus():
    exts = {".c", ".h", ".py", ".mjs", ".yaml", ".jinja", ".txt"}
    return "\n".join(read(p) for p in FILES if Path(p).suffix in exts or p.endswith("CMakeLists.txt"))


def path_exists(token, doc_rel):
    token = token.rstrip("/")
    for base in ("", "main/", Path(doc_rel).parent.as_posix() + "/"):
        if (ROOT / (base + token)).exists():
            return True
    return "/" not in token and token in BASENAMES


def check_code_names():
    corpus = code_corpus()
    words = set(re.findall(r"\w+", corpus))
    for rel in design_docs():
        for n, line in iter_lines_outside_code(rel):
            for tok in re.findall(r"`([^`]+)`", line):
                tok = tok.strip()
                if tok in EXTERNAL_NAMES or tok.startswith(EXTERNAL_PREFIXES) or "*" in tok:
                    continue
                # file paths
                if PATHLIKE.match(tok) and not tok.startswith(".") or tok.startswith(".claude/"):
                    looks_like_path = Path(tok.rstrip("/")).suffix in KNOWN_EXT or (tok.endswith("/") and "/" in tok[:-1] or tok.endswith("/"))
                    if looks_like_path and "%" not in tok and not path_exists(tok, rel):
                        report(f"{rel}:{n}", f"no such file: `{tok}`")
                    if looks_like_path:
                        continue
                # function calls: name() or name(args)
                m = re.match(r"^([a-z_][a-z0-9_]*)\(", tok)
                if m and m.group(1) not in words:
                    report(f"{rel}:{n}", f"function not found in the code: `{tok}`")
                # UPPER_CASE constants / macros
                if re.fullmatch(r"[A-Z][A-Z0-9]*_[A-Z0-9_]+", tok) and tok not in words:
                    report(f"{rel}:{n}", f"name not found in the code: `{tok}`")


# ---------------------------------------------------------------- 4. tables that mirror the code

def section(rel, heading):
    text = read(rel)
    m = re.search(rf"^(#+) {re.escape(heading)}\s*$", text, re.M)
    if not m:
        report(rel, f"heading '{heading}' not found (the check needs it)")
        return ""
    rest = text[m.end():]
    end = re.search(rf"^#{{1,{len(m.group(1))}}} ", rest, re.M)
    return rest[:end.start()] if end else rest


def check_source_layout():
    sec = section("docs/firmware.md", "Source layout")
    listed = set()
    for row in re.findall(r"^\|([^|]*)\|", sec, re.M):
        listed |= set(re.findall(r"`([\w/.]+\.[ch])`", row))
    have = {p[len("main/"):] for p in FILES if p.startswith("main/") and p.endswith(".c")}
    have |= {p[len("main/"):] for p in FILES if p.startswith("main/") and p.endswith(".h")
             and p[:-2] + ".c" not in FILE_SET}
    for f in sorted(have - listed):
        report("docs/firmware.md#source-layout", f"main/{f} is not listed")
    for f in sorted(listed - have):
        if not (ROOT / "main" / f).exists():
            report("docs/firmware.md#source-layout", f"lists main/{f}, which does not exist")


def check_nvs_keys():
    code = dict(re.findall(r'#define\s+(STORE_KEY_\w+)\s+"([^"]+)"', read("main/persistent_store.h")))
    keys = {v for k, v in code.items() if "RETIRED" not in k}
    sec = section("docs/firmware.md", "Persistent state")
    documented = set()
    for row in re.findall(r"^\|([^|]*)\|", sec, re.M):
        documented |= set(re.findall(r"`([^`]+)`", row))
    for k in sorted(keys - documented):
        report("docs/firmware.md#persistent-state", f"NVS key `{k}` is not documented")
    for k in sorted(documented - keys):
        report("docs/firmware.md#persistent-state", f"documents NVS key `{k}`, which the code does not define")


def check_build_switches():
    cmake = read("main/CMakeLists.txt")
    code = set(re.findall(r"^if\((\w+)\)", cmake, re.M))
    sec = section("docs/firmware.md", "Build switches")
    documented = set(re.findall(r"`-D(\w+)=", sec))
    for s in sorted(code - documented):
        report("docs/firmware.md#build-switches", f"switch -D{s} is not documented")
    for s in sorted(documented - code):
        report("docs/firmware.md#build-switches", f"documents -D{s}, which main/CMakeLists.txt does not handle")
    for rel in design_docs():
        for n, line in iter_lines_outside_code(rel):
            for s in re.findall(r"-D([A-Z_]+)=", line):
                if s not in code:
                    report(f"{rel}:{n}", f"unknown build switch -D{s}")


def check_attribute_ids():
    hdr = read("main/zigbee/zigbee_internal.h")
    zdoc = read("docs/zigbee.md")
    for name, val in re.findall(r"#define\s+(ATTR_\w+)\s+(0x[0-9A-Fa-f]{4})", hdr):
        if name == "ATTR_OCCUPANCY":
            continue
        if not re.search(rf"\b{val}\b", zdoc, re.I):
            report("docs/zigbee.md", f"{name} = {val} is not documented")
    cluster = re.search(r"#define\s+CUSTOM_CLUSTER_ID\s+(0x[0-9A-Fa-f]{4})", hdr).group(1)
    for rel in design_docs():
        for n, line in iter_lines_outside_code(rel):
            for c in re.findall(r"\b0xFC[0-9A-F]{2}\b(?!-0x)", line):  # not a range like 0xFC00-0xFFFF
                if c.upper() != cluster.upper():
                    report(f"{rel}:{n}", f"custom cluster {c}, the code uses {cluster}")


def check_line_icons():
    gen = read("tools/gen_bitmaps.py")
    block = re.search(r"^LINE_ICONS = \[(.*?)^\]", gen, re.M | re.S).group(1)
    names = re.findall(r'^\s*\("([\w-]+)"', block, re.M)
    sec = section("docs/data-model.md", "Line icons")
    documented = {}
    for row in re.findall(r"^\|(.*)\|\s*$", sec, re.M):
        cells = [c.strip() for c in row.split("|")]
        for i in range(0, len(cells) - 1, 2):
            if re.fullmatch(r"[0-9A-F]{2}", cells[i]):
                documented[int(cells[i], 16)] = cells[i + 1].split(" ")[0]
    for i, name in enumerate(names):
        if documented.get(i) != name:
            report("docs/data-model.md#line-icons", f"id {i:02X}: code has {name}, doc has {documented.get(i)}")
    for i in sorted(set(documented) - set(range(len(names)))):
        report("docs/data-model.md#line-icons", f"id {i:02X} ({documented[i]}) is not in LINE_ICONS")


def check_skill_index():
    readme = read("docs/README.md")
    for skill in sorted({p.split("/")[2] for p in FILES if p.startswith(".claude/skills/") and p.endswith("SKILL.md")}):
        if f".claude/skills/{skill}/SKILL.md" not in readme:
            report("docs/README.md#how-to", f"skill {skill} is not listed")
        if f"`{skill}`" not in read("CLAUDE.md"):
            report("CLAUDE.md#skills", f"skill {skill} is not listed")
    for rel in [p for p in FILES if p.startswith("docs/") and p.endswith(".md") and p != "docs/README.md"]:
        if Path(rel).relative_to("docs").as_posix() not in readme:
            report("docs/README.md", f"{rel} is not listed")


def main():
    check_markdown_links()
    check_doc_refs()
    check_code_names()
    check_source_layout()
    check_nvs_keys()
    check_build_switches()
    check_attribute_ids()
    check_line_icons()
    check_skill_index()
    for p in sorted(set(problems)):
        print(p)
    print(f"{len(set(problems))} problem(s)" if problems else "docs check: OK")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
