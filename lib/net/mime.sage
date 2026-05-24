gc_disable()
# net/mime — MIME type lookup and classification

proc lookup(ext):
    let e = ext
    if e == "html" or e == "htm":
        return "text/html"
    if e == "json":
        return "application/json"
    if e == "xml":
        return "application/xml"
    if e == "js":
        return "application/javascript"
    if e == "css":
        return "text/css"
    if e == "txt":
        return "text/plain"
    if e == "csv":
        return "text/csv"
    if e == "md":
        return "text/markdown"
    if e == "png":
        return "image/png"
    if e == "jpg" or e == "jpeg":
        return "image/jpeg"
    if e == "gif":
        return "image/gif"
    if e == "svg":
        return "image/svg+xml"
    if e == "webp":
        return "image/webp"
    if e == "ico":
        return "image/x-icon"
    if e == "pdf":
        return "application/pdf"
    if e == "zip":
        return "application/zip"
    if e == "gz":
        return "application/gzip"
    if e == "tar":
        return "application/x-tar"
    if e == "mp4":
        return "video/mp4"
    if e == "mp3":
        return "audio/mpeg"
    if e == "wav":
        return "audio/wav"
    if e == "wasm":
        return "application/wasm"
    return "application/octet-stream"

proc from_filename(filename):
    let parts = filename.split(".")
    if parts.length < 2:
        return "application/octet-stream"
    let ext = parts[parts.length - 1]
    # Lowercase
    var lower = ""
    for i in 0..ext.length:
        let c = ext[i]
        if c >= "A" and c <= "Z":
            lower = lower + chr(ord(c) + 32)
        else:
            lower = lower + c
    return lookup(lower)

proc is_text(mime_type):
    if mime_type == "application/json":
        return true
    if mime_type == "application/xml":
        return true
    if mime_type == "application/javascript":
        return true
    return mime_type.length >= 4 and mime_type[0] == "t" and mime_type[1] == "e" and mime_type[2] == "x" and mime_type[3] == "t"

proc is_image(mime_type):
    return mime_type.length >= 5 and mime_type[0] == "i" and mime_type[1] == "m" and mime_type[2] == "a" and mime_type[3] == "g" and mime_type[4] == "e"

proc category(mime_type):
    var i = 0
    while i < mime_type.length:
        if mime_type[i] == "/":
            var cat = ""
            var j = 0
            while j < i:
                cat = cat + mime_type[j]
                j = j + 1
            return cat
        i = i + 1
    return mime_type
