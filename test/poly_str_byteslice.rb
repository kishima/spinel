# byteslice(start, len) must dispatch on a poly receiver that is really a
# String (sym-hash value; a method param widened to poly by a poly caller),
# the same way ljust/rjust/center do. Regression for the launcher two-line
# label wrap (undefined method 'byteslice' for an instance of String).

# 1) sym-hash value is held as poly
h = { label: "abcdefghij" }
label = h[:label]
n = 3
puts label.byteslice(n, label.bytesize - n)
puts label.byteslice(0, 4)

# 2) method param widened to poly by a poly-passing caller
def tail(str, from)
  # str is poly here because the second call below passes a poly value
  str.byteslice(from, str.bytesize - from)
end
puts tail("hello world", 6)
puts tail(h[:label], 5)

# 3) out-of-range start yields nil (CRuby parity)
p label.byteslice(100, 2)
