# Conformance: Structs, Enums, Traits, Match Guards (Spec §12)
# EXPECT: 3
# EXPECT: 4
# EXPECT: {}
# EXPECT: {}
# EXPECT: medium
# Struct
struct Vec2:
    x: Float
    y: Float
var v = Vec2(3, 4)
println(v.x)
println(v.y)

# Enum
enum Status:
    Ok
    Pending
    Error
println(Status["Ok"])
println(Status["Error"])

# Match with guard
var score = 75
match score:
    case 75 if score >= 90:
        println("excellent")
    case 75 if score >= 50:
        println("medium")
    default:
        println("low")
