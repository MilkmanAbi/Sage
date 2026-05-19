# Test super.init() for calling parent class constructor
# EXPECT: Rex
# EXPECT: German Shepherd
# EXPECT: Max
# EXPECT: Labrador
# EXPECT: 6
# EXPECT: 1
# EXPECT: 2
# EXPECT: 3

class Animal:
    proc init(self, name):
        self.name = name

class Dog(Animal):
    proc init(self, name, breed):
        super.init(name)
        self.breed = breed

var d = Dog("Rex", "German Shepherd")
println(d.name)
println(d.breed)

# Chained: 3-level inheritance
class Puppy(Dog):
    proc init(self, name, breed, age):
        super.init(name, breed)
        self.age = age

var p = Puppy("Max", "Labrador", 6)
println(p.name)
println(p.breed)
println(p.age)

# Deep chain
class A:
    proc init(self, x):
        self.x = x

class B(A):
    proc init(self, x, y):
        super.init(x)
        self.y = y

class C(B):
    proc init(self, x, y, z):
        super.init(x, y)
        self.z = z

var c = C(1, 2, 3)
println(c.x)
println(c.y)
println(c.z)
