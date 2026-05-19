# EXPECT: Drawable
# EXPECT: 2
# Test trait declaration
trait Drawable:
    proc draw(self):
        pass
    proc resize(self, w, h):
        pass

println(Drawable["__name__"])
println(Drawable["__methods__"].length)
