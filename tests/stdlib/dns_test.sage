gc_disable()
# EXPECT: A
# EXPECT: AAAA
# EXPECT: NXDOMAIN
# EXPECT: 13
# EXPECT: example.com
# EXPECT: true
# EXPECT: 1

import net.dns

println(dns.type_name(1))
println(dns.type_name(28))
println(dns.rcode_name(3))

# Test name encoding
var encoded = dns.encode_name("example.com")
println(encoded.length)

# Test name reading from encoded bytes
var nr = dns.read_name(encoded, 0)
println(nr["name"])

# Test query building
var query = dns.build_query("example.com", 1, 1234)
println(query.length > 0)

# Parse the query we just built
var msg = dns.parse_message(query)
println(msg["header"]["qdcount"])
