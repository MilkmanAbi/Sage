# EXPECT: Hello, World!
# EXPECT: Hi, Abi!
# EXPECT: Hey, Sage!
proc greet(greeting, person = "World"):
    return greeting + ", " + person + "!"
print(greet(greeting: "Hello"))
print(greet(greeting: "Hi", person: "Abi"))
print(greet("Hey", person: "Sage"))
