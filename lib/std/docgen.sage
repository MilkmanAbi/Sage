gc_disable()
# Documentation generator
# Extracts doc comments from Sage source files and generates documentation

# ============================================================================
# Source parsing
# ============================================================================

# Extract doc comments (lines starting with # before proc/class/let)
proc extract_docs(source):
    let entries = []
    let lines = split_lines(source)
    var i = 0
    while i < lines.length:
        let line = lines[i]
        let trimmed = trim_line(line)
        # Check if this is a proc or class definition
        let is_proc = starts_with_word(trimmed, "proc")
        let is_class = starts_with_word(trimmed, "class")
        let is_let = starts_with_word(trimmed, "let")
        if is_proc or is_class or is_let:
            # Collect preceding comments
            let doc_lines = []
            var j = i - 1
            while j >= 0 and starts_with_char(trim_line(lines[j]), "#"):
                let comment = trim_line(lines[j])
                # Remove leading # and space
                var text = ""
                var k = 1
                if k < comment.length and comment[k] == " ":
                    k = 2
                while k < comment.length:
                    text = text + comment[k]
                    k = k + 1
                doc_lines.push(text)
                j = j - 1
            # Reverse doc_lines (we collected bottom-up)
            let doc = []
            var di = doc_lines.length - 1
            while di >= 0:
                doc.push(doc_lines[di])
                di = di - 1
            let entry = {}
            entry["line"] = i + 1
            entry["signature"] = trimmed
            entry["doc"] = doc
            if is_proc:
                entry["type"] = "proc"
                entry["name"] = extract_name(trimmed, "proc")
            if is_class:
                entry["type"] = "class"
                entry["name"] = extract_name(trimmed, "class")
            if is_let:
                entry["type"] = "let"
                entry["name"] = extract_name(trimmed, "let")
            entries.push(entry)
        i = i + 1
    return entries

# ============================================================================
# Output formatting
# ============================================================================

# Generate markdown documentation
proc to_markdown(entries, module_name):
    let nl = chr(10)
    var output = "# " + module_name + nl + nl
    # Group by type
    let procs = []
    let classes = []
    let constants = []
    for i in 0..entries.length:
        if entries[i]["type"] == "proc":
            procs.push(entries[i])
        if entries[i]["type"] == "class":
            classes.push(entries[i])
        if entries[i]["type"] == "let":
            constants.push(entries[i])
    if constants.length > 0:
        output = output + "## Constants" + nl + nl
        for i in 0..constants.length:
            output = output + "### `" + constants[i]["signature"] + "`" + nl
            let doc = constants[i]["doc"]
            for j in 0..doc.length:
                output = output + doc[j] + nl
            output = output + nl
    if classes.length > 0:
        output = output + "## Classes" + nl + nl
        for i in 0..classes.length:
            output = output + "### `" + classes[i]["name"] + "`" + nl
            let doc = classes[i]["doc"]
            for j in 0..doc.length:
                output = output + doc[j] + nl
            output = output + nl
    if procs.length > 0:
        output = output + "## Functions" + nl + nl
        for i in 0..procs.length:
            output = output + "### `" + procs[i]["signature"] + "`" + nl
            let doc = procs[i]["doc"]
            for j in 0..doc.length:
                output = output + doc[j] + nl
            output = output + nl
    return output

# ============================================================================
# Helpers
# ============================================================================

proc split_lines(text):
    let lines = []
    var current = ""
    for i in 0..text.length:
        if text[i] == chr(10):
            lines.push(current)
            current = ""
        else:
            if text[i] != chr(13):
                current = current + text[i]
    if current.length > 0:
        lines.push(current)
    return lines

proc trim_line(line):
    var start = 0
    while start < line.length and (line[start] == " " or line[start] == chr(9)):
        start = start + 1
    var result = ""
    for i in 0..line.length - start:
        result = result + line[start + i]
    return result

proc starts_with_char(s, ch):
    if s.length == 0:
        return false
    return s[0] == ch

proc starts_with_word(s, word):
    if s.length < word.length:
        return false
    for i in 0..word.length:
        if s[i] != word[i]:
            return false
    if s.length > word.length:
        let next = s[word.length]
        return next == " " or next == "("
    return true

proc extract_name(signature, keyword):
    var start = keyword.length + 1
    var name = ""
    while start < signature.length:
        let c = signature[start]
        if c == "(" or c == ":" or c == " ":
            return name
        name = name + c
        start = start + 1
    return name
