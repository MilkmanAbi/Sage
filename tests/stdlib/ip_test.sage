gc_disable()
# EXPECT: true
# EXPECT: 192.168.1.0
# EXPECT: 255.255.255.0
# EXPECT: 254
# EXPECT: true
# EXPECT: true
# EXPECT: false
# EXPECT: C
# EXPECT: 24
# EXPECT: 255.255.255.0

import net.ip

println(ip.is_valid_v4("192.168.1.1"))

var cidr = ip.parse_cidr("192.168.1.0/24")
println(cidr["network_str"])
println(cidr["mask_str"])
println(cidr["host_count"])

println(ip.in_subnet("192.168.1.100", "192.168.1.0/24"))
println(ip.is_private("10.0.0.1"))
println(ip.is_private("8.8.8.8"))

println(ip.address_class("192.168.1.1"))
println(ip.mask_to_prefix("255.255.255.0"))
println(ip.prefix_to_mask(24))
