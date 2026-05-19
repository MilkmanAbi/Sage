# EXPECT: medium
# EXPECT: negative
# EXPECT: other
# Test match with guard clauses
var x = 50
match x:
    case 50 if x > 100:
        println("big")
    case 50 if x > 10:
        println("medium")
    case 50:
        println("small")

var y = -5
match y:
    case -5 if y < 0:
        println("negative")
    default:
        println("positive")

var z = 99
match z:
    case 1 if true:
        println("one")
    default:
        println("other")
