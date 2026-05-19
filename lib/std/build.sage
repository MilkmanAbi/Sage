gc_disable()
# Build configuration and project management
# Provides project metadata, dependency declaration, and build task definitions

# ============================================================================
# Project definition
# ============================================================================

proc create_project(name, version):
    let proj = {}
    proj["name"] = name
    proj["version"] = version
    proj["description"] = ""
    proj["author"] = ""
    proj["license"] = ""
    proj["dependencies"] = []
    proj["dev_dependencies"] = []
    proj["scripts"] = {}
    proj["sources"] = []
    proj["entry_point"] = ""
    proj["build_dir"] = "build"
    proj["targets"] = {}
    return proj

proc set_description(proj, desc):
    proj["description"] = desc

proc set_author(proj, author):
    proj["author"] = author

proc set_license(proj, license):
    proj["license"] = license

proc set_entry(proj, entry):
    proj["entry_point"] = entry

# ============================================================================
# Dependencies
# ============================================================================

proc add_dep(proj, name, version):
    let dep = {}
    dep["name"] = name
    dep["version"] = version
    proj["dependencies"].push(dep)

proc add_dev_dep(proj, name, version):
    let dep = {}
    dep["name"] = name
    dep["version"] = version
    proj["dev_dependencies"].push(dep)

# ============================================================================
# Build targets
# ============================================================================

proc add_target(proj, name, target_type, sources):
    let target = {}
    target["name"] = name
    target["type"] = target_type
    target["sources"] = sources
    target["flags"] = []
    target["deps"] = []
    proj["targets"][name] = target

proc add_script(proj, name, command):
    proj["scripts"][name] = command

# ============================================================================
# Version parsing (semver)
# ============================================================================

proc parse_version(version_str):
    let parts = []
    var current = ""
    for i in 0..version_str.length:
        if version_str[i] == ".":
            parts.push(tonumber(current))
            current = ""
        else:
            current = current + version_str[i]
    if current.length > 0:
        parts.push(tonumber(current))
    let ver = {}
    ver["major"] = 0
    ver["minor"] = 0
    ver["patch"] = 0
    if parts.length >= 1:
        ver["major"] = parts[0]
    if parts.length >= 2:
        ver["minor"] = parts[1]
    if parts.length >= 3:
        ver["patch"] = parts[2]
    ver["string"] = version_str
    return ver

proc version_compare(a, b):
    if a["major"] != b["major"]:
        return a["major"] - b["major"]
    if a["minor"] != b["minor"]:
        return a["minor"] - b["minor"]
    return a["patch"] - b["patch"]

proc version_gte(a, b):
    return version_compare(a, b) >= 0

proc bump_major(ver):
    return parse_version(str(ver["major"] + 1) + ".0.0")

proc bump_minor(ver):
    return parse_version(str(ver["major"]) + "." + str(ver["minor"] + 1) + ".0")

proc bump_patch(ver):
    return parse_version(str(ver["major"]) + "." + str(ver["minor"]) + "." + str(ver["patch"] + 1))

# ============================================================================
# Project serialization
# ============================================================================

proc to_string(proj):
    let nl = chr(10)
    var out = "[project]" + nl
    out = out + "name = " + chr(34) + proj["name"] + chr(34) + nl
    out = out + "version = " + chr(34) + proj["version"] + chr(34) + nl
    if proj["description"].length > 0:
        out = out + "description = " + chr(34) + proj["description"] + chr(34) + nl
    if proj["author"].length > 0:
        out = out + "author = " + chr(34) + proj["author"] + chr(34) + nl
    if proj["entry_point"].length > 0:
        out = out + "entry = " + chr(34) + proj["entry_point"] + chr(34) + nl
    if proj["dependencies"].length > 0:
        out = out + nl + "[dependencies]" + nl
        let deps = proj["dependencies"]
        for i in 0..deps.length:
            out = out + deps[i]["name"] + " = " + chr(34) + deps[i]["version"] + chr(34) + nl
    return out
