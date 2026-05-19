# EXPECT: red
# EXPECT: blue
class Box:
    proc init(self, color):
        self.color = color
var b = Box("red")
print(b.color)
b.color = "blue"
print(b.color)
