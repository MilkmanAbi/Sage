# Test -> arrow operator (alias for .)
# EXPECT: 3
# EXPECT: 7
# EXPECT: 10
# EXPECT: 20
# EXPECT: (1, 2, 3)

class Point:
    proc init(self, x, y):
        self.x = x
        self.y = y

var p = Point(3, 7)

# Arrow for get
println(p->x)
println(p->y)

# Arrow for set
p->x = 10
p->y = 20
println(p->x)
println(p->y)

# Arrow with method call
class Vec3:
    proc init(self, x, y, z):
        self->x = x
        self->y = y
        self->z = z
    proc to_string(self):
        return "(" + str(self->x) + ", " + str(self->y) + ", " + str(self->z) + ")"

var v = Vec3(1, 2, 3)
println(v->to_string())
