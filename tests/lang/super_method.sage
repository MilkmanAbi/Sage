# Test super.method() for calling parent class methods
# EXPECT: Animal speaks
# EXPECT: Dog barks
# EXPECT: Animal speaks
# EXPECT: Puppy yaps
# EXPECT: Dog barks
# EXPECT: Animal speaks

class Animal:
    proc init(self, name):
        self.name = name
    proc speak(self):
        println("Animal speaks")

class Dog(Animal):
    proc init(self, name):
        super.init(name)
    proc speak(self):
        println("Dog barks")
        super.speak()

class Puppy(Dog):
    proc init(self, name):
        super.init(name)
    proc speak(self):
        println("Puppy yaps")
        super.speak()

var a = Animal("A")
a.speak()

var d = Dog("D")
d.speak()

var p = Puppy("P")
p.speak()
