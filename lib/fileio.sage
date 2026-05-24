# io.sage -- File I/O utilities
# The native io module provides read_file and write_file.
# This module adds higher-level conveniences.

import io as _io

proc read(path):
    return _io.read_file(path)

proc write(path, content):
    return _io.write_file(path, content)

proc append(path, content):
    return _io.append_file(path, content)

proc exists(path):
    try:
        _io.read_file(path)
        return true
    catch e:
        return false

proc read_lines(path):
    let content = _io.read_file(path)
    return content.split("\n")

proc write_lines(path, lines):
    let content = lines.join("\n")
    return _io.write_file(path, content)
