# A user method named with `?`, called on a poly receiver. The dispatch tries
# the builtin surface for its default arm (the analysis records a bool answer
# for any `?` name), finds no builtin `foo?`, and throws the trial away. The
# trial must not report an unresolved call: the program has none.
class W
  def initialize(id); @id = id; end
  def hit(x); x > 0; end
  def foo?; @id > 1; end
end
class B < W
end
class S < W
  def hit(x); x > 1; end
end
a = [B.new(1), S.new(2)]
w = a[1]
p w.hit(3)
p w.foo?
