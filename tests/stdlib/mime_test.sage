gc_disable()
# EXPECT: text/html
# EXPECT: application/json
# EXPECT: image/png
# EXPECT: application/pdf
# EXPECT: text/css
# EXPECT: true
# EXPECT: true
# EXPECT: image

import net.mime

println(mime.lookup("html"))
println(mime.lookup("json"))
println(mime.from_filename("photo.png"))
println(mime.from_filename("document.pdf"))
println(mime.from_filename("styles.CSS"))

println(mime.is_text("application/json"))
println(mime.is_image("image/png"))
println(mime.category("image/png"))
