"""Builds the LSL function index: every function, constant and event of the
Homeworldz compatibility target (Second Life LSL plus the Halcyon/InWorldz
extensions, ADR 0021), where each definition comes from, the region subsystem
it routes to, and whether Falcon runs it today.

    python -I falcon/tools/gen_function_index.py
    python -I falcon/tools/gen_falcon_tables.py     (then, for Falcon's tables)

Written by Ashley of Rookery Isles for the Rookery scripting package
(2026-11-14) and adapted here: OpenSimulator's os* functions and constants are
not read, since OpenSimulator-only extensions are excluded (docs/SCRIPTING.md).

Reads (nothing is modified):
  third_party/lsl-definitions/lsl_definitions.yaml
      Linden Lab's master LSL definitions (MIT, secondlife/lsl-definitions):
      every ll* function with typed, described arguments, its description,
      sleep delay, categories and flags; every constant with its value; every
      event with its described arguments.
  <Halcyon>/InWorldz/InWorldz.Phlox.Engine/LSLSystemAPI.cs
      InWorldz's library (BSD): the iw* and bot* functions, and for every
      function the line of its C# body, the behaviour reference. Bodies that
      only call NotImplemented() or Deprecated() are marked as Halcyon stubs.
  <Halcyon>/doc/bot LSL Functions/Function Documentation
      InWorldz's documentation of the bot* functions: their true signatures.
  <Halcyon>/OpenSim/Region/ScriptEngine/Shared/Api/Runtime/LSL_Constants.cs
      The constants Halcyon defines beyond Second Life's.
  third_party/halcyon-phlox/functions.json, constants.json
      Halcyon's own compiler tables: the authority for the iw* and bot*
      signatures and for the constants only the compiler defines. Where an SL
      constant has another value in Halcyon, it is kept as "halcyon_value".
  falcon/src/compiler.cpp
      The functions and events Falcon runs (implemented_functions,
      implemented_events).
  falcon/tools/areas.json
      Ordered rules sending each function to the subsystem it routes to.

Writes:
  falcon/index/functions.json, constants.json, events.json
  docs/LSL-FUNCTIONS.md (counts and the full table, for people)

The Halcyon tree is not in this repository; --halcyon_root says where it is.
Needs PyYAML.
"""
import argparse
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FALCON = os.path.dirname(HERE)
REPO = os.path.dirname(FALCON)
DEFAULTS = {
    "definitions": os.path.join(REPO, "third_party", "lsl-definitions", "lsl_definitions.yaml"),
    # the Halcyon source tree (github.com/InWorldz/halcyon), a behavioural reference kept outside this repository
    "halcyon_root": "F:/halcyon",
    # Halcyon's compiler tables
    "phlox": os.path.join(REPO, "third_party", "halcyon-phlox"),
    "falcon": os.path.join(FALCON, "src", "compiler.cpp"),
    "areas": os.path.join(HERE, "areas.json"),
    "out": os.path.join(FALCON, "index"),
    "doc": os.path.join(REPO, "docs", "LSL-FUNCTIONS.md"),
}
HALCYON_API = "InWorldz/InWorldz.Phlox.Engine/LSLSystemAPI.cs"
HALCYON_DOCS = "doc/bot LSL Functions/Function Documentation"
HALCYON_CONSTANTS = "OpenSim/Region/ScriptEngine/Shared/Api/Runtime/LSL_Constants.cs"

LSL_TYPES = {"void", "integer", "float", "string", "key", "vector", "rotation", "list", "quaternion"}

# Halcyon's C# types as LSL types. C# `string` is both LSL string and key;
# such arguments are marked for review rather than guessed.
CS_TO_LSL = {
    "void": "void", "int": "integer", "float": "float", "double": "float", "string": "string",
    "LSL_Vector": "vector", "Vector3": "vector", "LSL_Rotation": "rotation", "Quaternion": "rotation",
    "LSL_List": "list", "bool": "integer", "UUID": "key", "LSL_Key": "key", "LSL_String": "string",
    "LSL_Integer": "integer", "LSLInteger": "integer", "LSL_Float": "float", "LSLFloat": "float",
}

FUNC_RE = re.compile(r"^(\w+)\s+(\w+)\s*\((.*)\)\s*$")
CONST_RE = re.compile(r"^const\s+(\w+)\s+(\w+)\s*=\s*(.+?)\s*$")
EVENT_RE = re.compile(r"^event\s+(\w+)\s*\((.*)\)\s*$")
CS_METHOD_RE = re.compile(r"^\s*public\s+([\w<>\[\]]+)\s+((?:ll|iw|bot)\w+)\s*\(([^)]*)\)", re.M)


def parse_params(text, where):
    params = []
    for raw in [p.strip() for p in text.split(",") if p.strip()]:
        parts = raw.split()
        if len(parts) != 2 or parts[0] not in LSL_TYPES:
            raise ValueError("%s: cannot read parameter %r" % (where, raw))
        params.append({"type": "rotation" if parts[0] == "quaternion" else parts[0], "name": parts[1]})
    return params


def read_builtins(path):
    """The older one-line-per-definition file (SLua's builtins.txt); kept for
    comparing against the YAML. Not used for the index since 2026-11-14."""
    functions, constants, events = {}, {}, {}
    with open(path, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            line = line.strip()
            if not line or line.startswith("//"):
                continue
            where = "%s:%d" % (os.path.basename(path), number)
            match = CONST_RE.match(line)
            if match:
                constants[match.group(2)] = {"name": match.group(2), "type": match.group(1),
                                             "value": match.group(3), "source": "sl"}
                continue
            match = EVENT_RE.match(line)
            if match:
                events[match.group(1)] = {"name": match.group(1),
                                          "params": parse_params(match.group(2), where), "source": "sl"}
                continue
            match = FUNC_RE.match(line)
            if match and match.group(1) in LSL_TYPES:
                functions[match.group(2)] = {"name": match.group(2), "returns": match.group(1),
                                             "params": parse_params(match.group(3), where), "source": "sl"}
                continue
            raise ValueError("%s: unrecognised line %r" % (where, line))
    return functions, constants, events


def yaml_args(raw, where):
    """lsl_definitions.yaml arguments: a list of one-key maps."""
    params = []
    for entry in raw or []:
        if not isinstance(entry, dict) or len(entry) != 1:
            raise ValueError("%s: argument %r is not a one-key map" % (where, entry))
        (name, info), = entry.items()
        lsl = (info or {}).get("type")
        if lsl not in LSL_TYPES:
            raise ValueError("%s: argument %s has type %r" % (where, name, lsl))
        params.append({"type": "rotation" if lsl == "quaternion" else lsl, "name": name,
                       "description": (info or {}).get("tooltip", "")})
    return params


# lsl_definitions.yaml flags carried into the index (yes/no or a value)
FUNCTION_FLAGS = ("deprecated", "god-mode", "private", "experience", "linden-experience", "pure",
                  "must-use", "requires-permission", "native")


def read_definitions(path):
    import yaml  # PyYAML; only this reader needs it
    with open(path, encoding="utf-8") as handle:
        data = yaml.safe_load(handle)
    functions, constants, events = {}, {}, {}
    for name, info in data["functions"].items():
        info = info or {}
        returns = info.get("return", "void")
        if returns not in LSL_TYPES:
            raise ValueError("%s: return type %r" % (name, returns))
        entry = {"name": name, "returns": "rotation" if returns == "quaternion" else returns,
                 "params": yaml_args(info.get("arguments"), name), "source": "sl",
                 "description": info.get("tooltip", ""), "sleep": info.get("sleep", 0.0),
                 "categories": list(info.get("categories", []))}
        flags = {flag: info[flag] for flag in FUNCTION_FLAGS if info.get(flag)}
        if flags:
            entry["flags"] = flags
        functions[name] = entry
    for name, info in data["constants"].items():
        info = info or {}
        entry = {"name": name, "type": info.get("type"), "value": str(info.get("value")), "source": "sl",
                 "description": info.get("tooltip", ""), "member_of": list(info.get("member-of", []))}
        if info.get("deprecated"):
            entry["deprecated"] = True
        constants[name] = entry
    for name, info in data["events"].items():
        info = info or {}
        entry = {"name": name, "params": yaml_args(info.get("arguments"), name), "source": "sl",
                 "description": info.get("tooltip", ""), "categories": list(info.get("categories", []))}
        if info.get("deprecated"):
            entry["deprecated"] = True
        events[name] = entry
    return functions, constants, events


def read_halcyon(path):
    """{name: {line, returns, params, review, stub}} for every public ll/iw/bot method."""
    with open(path, encoding="utf-8", errors="replace") as handle:
        source = handle.read()
    matches = list(CS_METHOD_RE.finditer(source))
    found = {}
    for i, match in enumerate(matches):
        name = match.group(2)
        # counted from the name: the pattern's leading \s* can start on the line above
        line = source.count("\n", 0, match.start(2)) + 1
        body = source[match.end(): matches[i + 1].start() if i + 1 < len(matches) else len(source)]
        review = []
        params = []
        for raw in [p.strip() for p in match.group(3).split(",") if p.strip()]:
            parts = raw.split()
            if parts[0] == "out" or len(parts) < 2:
                review.append("parameter %r has no LSL form" % raw)
                continue
            cs_type, pname = parts[0], parts[-1]
            lsl = CS_TO_LSL.get(cs_type)
            if lsl is None:
                review.append("C# type %s of %s" % (cs_type, pname))
                lsl = "string"
            elif cs_type == "string":
                review.append("%s: string or key" % pname)
            params.append({"type": lsl, "name": pname})
        returns = CS_TO_LSL.get(match.group(1))
        if returns is None:
            review.append("C# return type %s" % match.group(1))
            returns = "string"
        elif match.group(1) == "string":
            review.append("return: string or key")
        stub = bool(re.search(r"\b(NotImplemented|Deprecated)\s*\(", body)) and len(body.strip()) < 400
        # A void C# method that answers later through SysReturn returns that
        # value to the script: its type is the true return type.
        if match.group(1) == "void":
            answered = async_return_type(body)
            if answered is not None:
                returns, note = answered
                if note:
                    review.append(note)
        if name not in found:
            found[name] = {"line": line, "returns": returns, "params": params, "review": review, "stub": stub}
    return found


def async_return_type(body):
    """(lsl type, review note or "") of the value a void Halcyon method hands
    back through SysReturn, or None when it hands back nothing (null, or no
    SysReturn at all)."""
    found = re.search(r"SysReturn\(\s*m_itemID\s*,\s*([^,)]+)", body)
    if not found:
        return None
    value = found.group(1).strip()
    if value == "null":
        return None
    if value.startswith("new LSL_List"):
        return "list", ""
    if ".ToString(" in value:
        return "string", "return: string or key (answered through SysReturn)"
    if re.fullmatch(r"\w+", value):
        declared = re.search(r"\b([\w.]+)\s+" + re.escape(value) + r"\s*(?:=|;)", body)
        if declared:
            cs_type = declared.group(1).split(".")[-1]
            lsl = CS_TO_LSL.get(cs_type)
            if lsl:
                note = "return: string or key (answered through SysReturn)" if cs_type == "string" else ""
                return lsl, note
        return "string", "return type of SysReturn value %r not found" % value
    return "string", "return type of SysReturn value %r not found" % value


DOC_TYPE_NAMES = {"int": "integer", "bool": "integer", "double": "float", "LSL_List": "list",
                  "LSL_Vector": "vector", "LSL_Rotation": "rotation", "UUID": "key"}

# "Function: key botCreateBot(...);", also "Function botSensorRemove();" and
# "event bot_update(key botID, ...);" as two of the files write it
DOC_SIG_RE = re.compile(r"^(Function|Event)\s*:?\s*(?:(\w+)\s+)?(\w+)\s*\((.*)\)\s*;?\s*$", re.IGNORECASE)


def read_halcyon_docs(folder):
    """{name: {kind, returns, params, description}} from InWorldz's function
    documentation files ("Function: key botCreateBot(string firstName, ...);")."""
    found = {}
    if not folder or not os.path.isdir(folder):
        return found
    for file_name in sorted(os.listdir(folder)):
        if not file_name.lower().endswith(".txt"):
            continue
        with open(os.path.join(folder, file_name), encoding="utf-8", errors="replace") as handle:
            lines = handle.read().replace("\r\n", "\n").split("\n")
        head = DOC_SIG_RE.match(lines[0].strip()) if lines else None
        if not head:
            continue
        kind, returns, name, raw = head.group(1).lower(), head.group(2) or "void", head.group(3), head.group(4)
        # the documentation sometimes writes C# type names (int, bool)
        returns = DOC_TYPE_NAMES.get(returns, returns)
        params = []
        for part in [p.strip() for p in raw.split(",") if p.strip()]:
            bits = part.split()
            if len(bits) == 2:
                bits[0] = DOC_TYPE_NAMES.get(bits[0], bits[0])
            if len(bits) != 2 or bits[0] not in LSL_TYPES:
                raise ValueError("%s: cannot read parameter %r" % (file_name, part))
            params.append({"type": "rotation" if bits[0] == "quaternion" else bits[0], "name": bits[1],
                           "description": ""})
        text = []
        for line in lines[1:]:
            if line.strip().lower().startswith("examples:"):
                break
            text.append(line.rstrip())
        description = " ".join(" ".join(text).split())
        if returns not in LSL_TYPES:
            raise ValueError("%s: return type %r" % (file_name, returns))
        found[name] = {"kind": kind, "returns": "rotation" if returns == "quaternion" else returns,
                       "params": params, "description": description, "doc": file_name}
    return found


CS_CONST_RE = re.compile(
    r"public\s+(?:const|static\s+readonly)\s+(\w+)\s+(\w+)\s*=\s*(?:new\s+\w+\s*\(\s*)?([^;]+?)\s*\)?\s*;")


def read_halcyon_constants(path):
    """{name: {type, value}} for Halcyon's integer and float constants."""
    found = {}
    if not path or not os.path.exists(path):
        return found
    with open(path, encoding="utf-8", errors="replace") as handle:
        source = handle.read()
    for cs_type, name, raw in CS_CONST_RE.findall(source):
        lsl = CS_TO_LSL.get(cs_type)
        if lsl not in ("integer", "float"):
            continue
        value = raw.strip().rstrip(")").strip()
        try:
            number = int(value, 0) if lsl == "integer" else float(value.rstrip("fF"))
        except ValueError:
            continue
        found[name] = {"type": lsl, "value": str(number)}
    return found


def read_phlox(folder):
    """({name: function}, {name: constant}) from Halcyon's compiler tables, or
    two empty maps when the folder is not there."""
    functions, constants = {}, {}
    if not folder or not os.path.isdir(folder):
        return functions, constants
    with open(os.path.join(folder, "functions.json"), encoding="utf-8") as handle:
        for entry in json.load(handle):
            for lsl in [entry["returns"]] + [p["type"] for p in entry["params"]]:
                if lsl not in LSL_TYPES:
                    raise ValueError("halcyon-phlox/functions.json: %s has type %r" % (entry["name"], lsl))
            functions[entry["name"]] = entry
    with open(os.path.join(folder, "constants.json"), encoding="utf-8") as handle:
        for entry in json.load(handle):
            constants[entry["name"]] = entry
    return functions, constants


def same_value(lsl_type, a, b):
    """True when two written constant values are the same value."""
    a, b = str(a).strip(), str(b).strip()
    try:
        if lsl_type == "integer":
            return int(a, 0) == int(b, 0)
        if lsl_type == "float":
            return abs(float(a.rstrip("fF")) - float(b.rstrip("fF"))) <= 1e-6 * max(1.0, abs(float(b.rstrip("fF"))))
        if lsl_type in ("vector", "rotation"):
            pa = [float(x) for x in a.strip("<>").split(",")]
            pb = [float(x) for x in b.strip("<>").split(",")]
            return len(pa) == len(pb) and all(abs(x - y) <= 1e-6 for x, y in zip(pa, pb))
    except ValueError:
        return False

    # Linden Lab's definitions write some characters as \u{FDD0}
    def text(value):
        return re.sub(r"\\u\{([0-9A-Fa-f]+)\}", lambda m: chr(int(m.group(1), 16)), value.strip('"'))
    return text(a) == text(b)


def read_falcon(path):
    with open(path, encoding="utf-8") as handle:
        source = handle.read()
    functions = source[source.index("implemented_functions"): source.index("implemented_events")]
    events = source[source.index("implemented_events"): source.index("lookup_host")]
    return set(re.findall(r'"(\w+)"', functions)), set(re.findall(r'"(\w+)"', events))


def area_of(name, rules):
    for rule in rules:
        if re.search(rule["match"], name):
            return rule["area"]
    return "other"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for key, value in DEFAULTS.items():
        parser.add_argument("--" + key, default=value)
    args = parser.parse_args()

    functions, constants, events = read_definitions(args.definitions)
    halcyon = read_halcyon(os.path.join(args.halcyon_root, HALCYON_API))
    falcon_functions, falcon_events = read_falcon(args.falcon)
    with open(args.areas, encoding="utf-8") as handle:
        rules = json.load(handle)["rules"]
    halcyon_rel = HALCYON_API
    for name, entry in functions.items():
        entry["family"] = "ll"
    for name, info in halcyon.items():
        if name.startswith("ll"):
            if name in functions:
                functions[name]["halcyon"] = "%s:%d" % (halcyon_rel, info["line"])
                functions[name]["halcyon_stub"] = info["stub"]
            else:
                functions[name] = {"name": name, "returns": info["returns"], "params": info["params"],
                                   "source": "halcyon", "family": "ll", "review": info["review"],
                                   "halcyon": "%s:%d" % (halcyon_rel, info["line"]), "halcyon_stub": info["stub"]}
            continue
        functions[name] = {"name": name, "returns": info["returns"], "params": info["params"],
                           "source": "halcyon", "family": "iw" if name.startswith("iw") else "bot",
                           "review": info["review"], "halcyon": "%s:%d" % (halcyon_rel, info["line"]),
                           "halcyon_stub": info["stub"]}

    # InWorldz's documentation is the authority for the functions and events it documents
    docs = read_halcyon_docs(os.path.join(args.halcyon_root, HALCYON_DOCS))
    docs_rel = HALCYON_DOCS + "/"
    for name, doc in docs.items():
        if doc["kind"] == "event":
            if name not in events:
                events[name] = {"name": name, "params": doc["params"], "source": "halcyon",
                                "description": doc["description"], "categories": [],
                                "doc": docs_rel + doc["doc"]}
            continue
        entry = functions.get(name)
        if entry is None:
            # documented by InWorldz but not a public method of LSLSystemAPI.cs
            entry = functions[name] = {"name": name, "source": "halcyon",
                                       "family": "iw" if name.startswith("iw") else "bot",
                                       "halcyon": None, "halcyon_stub": False}
        elif entry.get("source") == "sl":
            continue  # Linden Lab's definitions stay the authority for SL functions
        entry["returns"] = doc["returns"]
        entry["params"] = doc["params"]
        entry["description"] = doc["description"]
        entry["review"] = []
        entry["doc"] = docs_rel + doc["doc"]

    # Halcyon's constants beyond SL's (IW_*, BOT_*, InWorldz camera and vehicle types)
    for name, info in read_halcyon_constants(os.path.join(args.halcyon_root, HALCYON_CONSTANTS)).items():
        if name not in constants:
            constants[name] = {"name": name, "type": info["type"], "value": info["value"], "source": "halcyon",
                               "description": "", "member_of": []}

    # Halcyon's compiler: the signatures InWorldz scripts compiled against, and its own constants
    phlox_functions, phlox_constants = read_phlox(args.phlox)
    for name, info in phlox_functions.items():
        entry = functions.get(name)
        if entry is None:
            raise ValueError("halcyon-phlox/functions.json: %s is in no other source" % name)
        entry["phlox_syscall"] = info["syscall"]
        if entry.get("source") == "sl":
            continue  # Linden Lab's definitions stay the authority for SL functions
        old = entry.get("params", [])
        entry["returns"] = info["returns"]
        entry["params"] = [{"type": p["type"],
                            "name": old[i]["name"] if len(old) == len(info["params"]) else p["name"],
                            "description": old[i].get("description", "") if len(old) == len(info["params"]) else ""}
                           for i, p in enumerate(info["params"])]
        entry["review"] = []  # the compiler's own types settle string versus key
    for name, info in phlox_constants.items():
        value = str(info["int32"]) if info["type"] == "integer" else info["value"]
        existing = constants.get(name)
        if existing is None:
            constants[name] = {"name": name, "type": info["type"], "value": value, "source": "halcyon",
                               "description": "", "member_of": []}
        elif existing.get("source") == "halcyon":
            existing["value"] = value  # what the compiler gave a script
        elif not same_value(info["type"], existing["value"], value):
            existing["halcyon_value"] = value

    for name, entry in functions.items():
        entry["area"] = area_of(name, rules)
        entry.setdefault("halcyon", None)
        entry.setdefault("halcyon_stub", False)
        entry.setdefault("review", [])
        # Halcyon's iw*/bot* carry no description: written as each is ported
        entry.setdefault("description", "")
        entry.setdefault("sleep", None)
        entry.setdefault("categories", [])
        entry["falcon"] = name in falcon_functions
        entry.setdefault("lua", "ll." + name[2:] if name.startswith("ll") else name)
    for name, entry in events.items():
        entry["falcon"] = name in falcon_events

    os.makedirs(args.out, exist_ok=True)
    ordered = [functions[k] for k in sorted(functions, key=lambda n: (n[:2] != "ll", n.lower()))]
    for path, payload in (("functions.json", ordered),
                          ("constants.json", [constants[k] for k in sorted(constants)]),
                          ("events.json", [events[k] for k in sorted(events)])):
        with open(os.path.join(args.out, path), "w", encoding="utf-8", newline="\n") as handle:
            json.dump(payload, handle, indent=1, ensure_ascii=False)
            handle.write("\n")

    write_doc(args.doc, ordered, constants, events)
    by_family = collections.Counter(f["family"] for f in ordered)
    print(json.dumps({"functions": len(ordered), "by_family": by_family, "constants": len(constants),
                      "events": len(events), "falcon_functions": sum(f["falcon"] for f in ordered),
                      "sl_not_in_halcyon": sum(1 for f in ordered if f["source"] == "sl" and not f["halcyon"]),
                      "halcyon_stubs": sum(f["halcyon_stub"] for f in ordered),
                      "needs_review": sum(1 for f in ordered if f["review"])}))


def signature(entry):
    params = ", ".join("%s %s" % (p["type"], p["name"]) for p in entry["params"])
    return "%s %s(%s)" % (entry["returns"], entry["name"], params)


def write_doc(path, functions, constants, events):
    families = collections.Counter(f["family"] for f in functions)
    areas = collections.Counter(f["area"] for f in functions)
    lines = [
        "# Function index",
        "",
        "Generated by `falcon/tools/gen_function_index.py` from the files it names; do not edit by hand.",
        "The machine-readable index is `falcon/index/` (functions.json, constants.json, events.json).",
        "",
        "Columns: **area** is the region subsystem a call routes to (rules in `falcon/tools/areas.json`);",
        "**Halcyon** is the line of InWorldz's C# body, the behaviour reference (\"stub\" when Halcyon never",
        "implemented it); **Falcon** marks what Falcon compiles and the region answers today.",
        "",
        "## Totals",
        "",
        "| What | Count |",
        "|---|---|",
        "| Functions | %d |" % len(functions),
    ]
    for family in ("ll", "iw", "bot"):
        lines.append("| of which %s* | %d |" % (family, families.get(family, 0)))
    lines += [
        "| Constants | %d |" % len(constants),
        "| Events | %d |" % len(events),
        "| Functions Falcon runs today | %d |" % sum(f["falcon"] for f in functions),
        "| SL functions with no Halcyon body (newer than InWorldz) | %d |"
        % sum(1 for f in functions if f["source"] == "sl" and not f["halcyon"]),
        "| Halcyon stubs | %d |" % sum(f["halcyon_stub"] for f in functions),
        "| Signatures needing review (C# string may be key) | %d |" % sum(1 for f in functions if f["review"]),
        "",
        "## By area",
        "",
        "| Area | Functions |",
        "|---|---|",
    ]
    lines += ["| %s | %d |" % (area, count) for area, count in sorted(areas.items())]
    lines += ["", "## Functions", "", "| Function | Area | Halcyon | Falcon |",
              "|---|---|---|---|"]
    for entry in functions:
        halcyon = entry["halcyon"].split(":")[-1] if entry["halcyon"] else "none"
        if entry["halcyon_stub"]:
            halcyon += " (stub)"
        lines.append("| `%s` | %s | %s | %s |" % (signature(entry), entry["area"], halcyon,
                                                  "yes" if entry["falcon"] else ""))
    lines += ["", "## Events", "", "| Event | Falcon |", "|---|---|"]
    for name in sorted(events):
        entry = events[name]
        params = ", ".join("%s %s" % (p["type"], p["name"]) for p in entry["params"])
        lines.append("| `%s(%s)` | %s |" % (name, params, "yes" if entry["falcon"] else ""))
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print("gen_function_index: %s" % error, file=sys.stderr)
        sys.exit(1)
