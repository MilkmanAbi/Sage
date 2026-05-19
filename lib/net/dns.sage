gc_disable()
# net/dns — DNS message encoding/decoding

proc type_name(t):
    if t == 1:
        return "A"
    if t == 2:
        return "NS"
    if t == 5:
        return "CNAME"
    if t == 6:
        return "SOA"
    if t == 12:
        return "PTR"
    if t == 15:
        return "MX"
    if t == 16:
        return "TXT"
    if t == 28:
        return "AAAA"
    if t == 33:
        return "SRV"
    if t == 255:
        return "ANY"
    return "UNKNOWN"

proc rcode_name(r):
    if r == 0:
        return "NOERROR"
    if r == 1:
        return "FORMERR"
    if r == 2:
        return "SERVFAIL"
    if r == 3:
        return "NXDOMAIN"
    if r == 4:
        return "NOTIMP"
    if r == 5:
        return "REFUSED"
    return "UNKNOWN"

# Encode a domain name into DNS wire format
# "example.com" -> [7, 'e','x','a','m','p','l','e', 3, 'c','o','m', 0]
proc encode_name(name):
    let result = []
    let parts = name.split(".")
    for i in 0..parts.length:
        let label = parts[i]
        result.push(label.length)
        var j = 0
        while j < label.length:
            result.push(ord(label[j]))
            j = j + 1
    result.push(0)  # terminator
    return result

# Read a domain name from a byte array starting at offset
proc read_name(data, offset):
    let result = {}
    var name = ""
    var pos = offset
    var jumped = false
    var jumpstr_end = 0
    var steps = 0
    while pos < data.length and steps < 128:
        let length = data[pos]
        if length == 0:
            pos = pos + 1
            if not jumped:
                jumpstr_end = pos
            steps = 128  # exit loop
        elif (length & 192) == 192:
            # Pointer
            let ptr = ((length & 63) * 256) + data[pos + 1]
            if not jumped:
                jumpstr_end = pos + 2
            jumped = true
            pos = ptr
        else:
            if name.length > 0:
                name = name + "."
            var i = 1
            while i <= length:
                if pos + i < data.length:
                    name = name + chr(data[pos + i])
                i = i + 1
            pos = pos + length + 1
        steps = steps + 1
    result["name"] = name
    result["_end"] = jumpstr_end
    return result

# Build a DNS query message
proc build_query(name, qtype, txid):
    let msg = []
    # Header: txid (2), flags (2), qdcount (2), ancount ancount (2), nscount (2), arcount (2)
    msg.push((txid / 256) | 0)
    msg.push(txid % 256)
    msg.push(1)  # QR=0, Opcode=0, RD=1
    msg.push(0)  # flags low
    msg.push(0)
    msg.push(1)  # qdcount = 1
    msg.push(0)
    msg.push(0)  # ancount = 0
    msg.push(0)
    msg.push(0)  # nscount = 0
    msg.push(0)
    msg.push(0)  # arcount = 0
    # Question
    let encoded = encode_name(name)
    for i in 0..encoded.length:
        msg.push(encoded[i])
    msg.push(0)
    msg.push(qtype)   # qtype
    msg.push(0)
    msg.push(1)       # qclass = IN
    return msg

# Parse a DNS message header
proc parse_message(data):
    if data.length < 12:
        return nil
    let msg = {}
    let header = {}
    header["txid"]    = data[0] * 256 + data[1]
    header["flags"]   = data[2] * 256 + data[3]
    header["qr"]      = (data[2] & 128) != 0
    header["rcode"]   = data[3] & 15
    header["qdcount"] = data[4] * 256 + data[5]
    header["ancount"] = data[6] * 256 + data[7]
    header["nscount"] = data[8] * 256 + data[9]
    header["arcount"] = data[10] * 256 + data[11]
    msg["header"] = header
    msg["questions"] = []
    msg["answers"] = []
    var pos = 12
    var i = 0
    while i < header["qdcount"] and pos < data.length:
        let nr = read_name(data, pos)
        pos = nr["_end"]
        let q = {}
        q["name"] = nr["name"]
        if pos + 4 <= data.length:
            q["qtype"]  = data[pos] * 256 + data[pos+1]
            q["qclass"] = data[pos+2] * 256 + data[pos+3]
            pos = pos + 4
        msg["questions"].push(q)
        i = i + 1
    return msg
