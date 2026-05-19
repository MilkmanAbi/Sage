# EXPECT: Hi, I am Rex
# EXPECT: Rex says woof!
class Animal:
    proc init(self, name):
        self.name = name

impl Animal:
    proc greet(self):
        return "Hi, I am " + self.name
    proc speak(self):
        return self.name + " says woof!"

var a = Animal("Rex")
println(a.greet())
println(a.speak())
