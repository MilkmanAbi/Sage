# EXPECT: greeting
var cmd = "hello"
match cmd:
    case "quit":
        print("quitting")
    case "hello":
        print("greeting")
    case "help":
        print("showing help")
