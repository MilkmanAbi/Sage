# FrogPond — Sage's sandboxing module
# User-facing API for LilyBox/LilyKnight
#
# Usage:
#   import sandbox
#   let box = sandbox.create("my_plugin.manifest")
#   sandbox.run(box, "plugin.sage")
#   sandbox.destroy(box)
#
# Or use the decorator-style for inline sandboxing:
#   sandbox.restricted():
#       # code here runs with no net/fs/ffi access
#
# FrogPond — because frogs in a pond are safe, contained, 
# and they only leap where the lily pads take them.

# These are backed by native functions registered in sandbox_module.c
# The module is loaded by the native module system.

# Permission constants for programmatic sandbox creation
let PERM_NONE       = 0x00000000
let PERM_NET        = 0x0000001F
let PERM_FS_READ    = 0x00000100
let PERM_FS_WRITE   = 0x00000200
let PERM_FS         = 0x00000700
let PERM_PROC       = 0x00007000
let PERM_FFI        = 0x00040000
let PERM_ENV_READ   = 0x00080000
let PERM_ALL        = 0xFFFFFFFF
