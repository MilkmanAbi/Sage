gc_disable()
# net/ip — IPv4 address parsing, CIDR, classification

proc _parse_v4(s):
    let parts = s.split(".")
    if parts.length != 4:
        return nil
    let nums = []
    for i in 0..4:
        let n = int(parts[i])
        if n < 0 or n > 255:
            return nil
        nums.push(n)
    return nums

proc is_valid_v4(addr):
    return _parse_v4(addr) != nil

proc _dotted(parts):
    return str(parts[0]) + "." + str(parts[1]) + "." + str(parts[2]) + "." + str(parts[3])

proc _to_int(parts):
    return parts[0] * 16777216 + parts[1] * 65536 + parts[2] * 256 + parts[3]

proc _from_int(n):
    let a = (n / 16777216) | 0
    let b = ((n % 16777216) / 65536) | 0
    let c = ((n % 65536) / 256) | 0
    let d = n % 256
    return [a, b, c, d]

proc parse_cidr(cidr):
    var slash = 0
    var i = 0
    while i < cidr.length:
        if cidr[i] == "/":
            slash = i
        i = i + 1
    var addr_str = ""
    var j = 0
    while j < slash:
        addr_str = addr_str + cidr[j]
        j = j + 1
    var prefix_str = ""
    var k = slash + 1
    while k < cidr.length:
        prefix_str = prefix_str + cidr[k]
        k = k + 1
    let prefix = int(prefix_str)
    var mask_n = 0
    if prefix > 0:
        mask_n = (4294967295 | 0)
        if prefix < 32:
            mask_n = (mask_n ^ ((1 << (32 - prefix)) - 1)) | 0
    let addr = _parse_v4(addr_str)
    let addr_int = _to_int(addr)
    let mask_parts = _from_int(mask_n)
    let network_int = addr_int & mask_n
    let network_parts = _from_int(network_int)
    let host_count = (1 << (32 - prefix)) - 2
    let result = {}
    result["network_str"] = _dotted(network_parts)
    result["mask_str"] = _dotted(mask_parts)
    result["prefix"] = prefix
    result["host_count"] = host_count
    return result

proc in_subnet(addr, cidr):
    let cidr_info = parse_cidr(cidr)
    let a = _parse_v4(addr)
    if a == nil:
        return false
    let a_int = _to_int(a)
    let net_int = _to_int(_parse_v4(cidr_info["network_str"]))
    let prefix = cidr_info["prefix"]
    if prefix == 0:
        return true
    var mask = (4294967295 | 0)
    if prefix < 32:
        mask = mask ^ ((1 << (32 - prefix)) - 1)
    return (a_int & mask) == net_int

proc is_private(addr):
    let a = _parse_v4(addr)
    if a == nil:
        return false
    if a[0] == 10:
        return true
    if a[0] == 172 and a[1] >= 16 and a[1] <= 31:
        return true
    if a[0] == 192 and a[1] == 168:
        return true
    if a[0] == 127:
        return true
    return false

proc address_class(addr):
    let a = _parse_v4(addr)
    if a == nil:
        return "?"
    if a[0] < 128:
        return "A"
    if a[0] < 192:
        return "B"
    if a[0] < 224:
        return "C"
    if a[0] < 240:
        return "D"
    return "E"

proc mask_to_prefix(mask_str):
    let a = _parse_v4(mask_str)
    if a == nil:
        return 0
    let n = _to_int(a)
    var count = 0
    var i = 31
    while i >= 0:
        if ((n >> i) & 1) == 1:
            count = count + 1
        i = i - 1
    return count

proc prefix_to_mask(prefix):
    var mask_n = 0
    if prefix > 0:
        mask_n = 4294967295 | 0
        if prefix < 32:
            mask_n = mask_n ^ ((1 << (32 - prefix)) - 1)
    return _dotted(_from_int(mask_n))
