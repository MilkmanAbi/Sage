# Sage Package Manifest (sage.toml) support
# Provides functions to read and validate project configuration

import io

proc parse_toml_line(line):
    # Simple TOML line parser: key = "value" or key = number or [section]
    let trimmed = line
    if trimmed.length == 0:
        return nil
    if trimmed[0] == "#":
        return nil
    if trimmed[0] == "[":
        let last_idx = trimmed.length - 1
        if last_idx > 0:
            var section = ""
            var i = 1
            while i < trimmed.length:
                if trimmed[i] == "]":
                    return {"type": "section", "name": section}
                section = section + trimmed[i]
                i = i + 1
        return nil
    # key = value
    let parts = trimmed.split("=")
    if parts.length < 2:
        return nil
    var key = parts[0]
    # Strip whitespace from key (simple trim)
    while key.length > 0 and key[key.length - 1] == " ":
        key = slice(key, 0, key.length - 1)
    while key.length > 0 and key[0] == " ":
        key = slice(key, 1, key.length)
    var val = parts[1]
    while val.length > 0 and val[0] == " ":
        val = slice(val, 1, val.length)
    while val.length > 0 and val[val.length - 1] == " ":
        val = slice(val, 0, val.length - 1)
    # Strip quotes
    if val.length >= 2 and val[0] == chr(34):
        val = slice(val, 1, val.length - 1)
    return {"type": "kv", "key": key, "value": val}

proc read_manifest(path):
    # Read a sage.toml file and return a dict
    if not io.exists(path):
        return nil
    var content = io.readfile(path)
    let lines = content.split(chr(10))
    let result = {}
    var current_section = "package"
    var i = 0
    while i < lines.length:
        let parsed = parse_toml_line(lines[i])
        if parsed != nil:
            if parsed["type"] == "section":
                current_section = parsed["name"]
            if parsed["type"] == "kv":
                let full_key = current_section + "." + parsed["key"]
                result[full_key] = parsed["value"]
        i = i + 1
    return result

proc init_manifest(name, version, description):
    # Generate a new sage.toml content
    let nl = chr(10)
    let q = chr(34)
    var content = "[package]" + nl
    content = content + "name = " + q + name + q + nl
    content = content + "version = " + q + version + q + nl
    content = content + "description = " + q + description + q + nl
    content = content + nl
    content = content + "[dependencies]" + nl
    return content
