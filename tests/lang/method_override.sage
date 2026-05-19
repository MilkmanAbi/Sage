# EXPECT: base
# EXPECT: child
class Base:
    proc who(self):
        return "base"
class Child(Base):
    proc who(self):
        return "child"
var b = Base()
var c = Child()
print(b.who())
print(c.who())
