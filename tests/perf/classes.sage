# EXPECT: Rex
# EXPECT: Woof!
# EXPECT: Animal moves
# EXPECT: <instance of Dog>
# Conformance: Classes, Inheritance, Super (Spec §12)
class Animal:
    proc init(self, name):
        self.name = name
    proc speak(self):
        return "..."
    proc move(self):
        println("Animal moves")

class Dog(Animal):
    proc init(self, name, breed):
        super.init(name)
        self.breed = breed
    proc speak(self):
        return "Woof!"
    proc __str__(self):
        return "Dog(" + self.name + ", " + self.breed + ")"

var d = Dog("Rex", "Lab")
println(d.name)
println(d.speak())
d.move()
println(d)
