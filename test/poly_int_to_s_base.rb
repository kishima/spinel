# Integer#to_s(base) must dispatch on a poly receiver that is really an Integer
# (a byte held in a poly slot). Regression for msgpack's "unknown prefix
# 0x#{c.to_s(16)}" error message where c is a poly getbyte result.
h = { byte: 203, n: 255, dec: 42 }
puts h[:byte].to_s(16)     # cb
puts h[:n].to_s(16)        # ff
puts h[:n].to_s(2)         # 11111111
puts h[:dec].to_s          # 42 (argc==0 still works via poly-to-string)
puts "0x#{h[:byte].to_s(16)}"   # 0xcb
