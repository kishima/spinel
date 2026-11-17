# A call no class in the program answers, on a poly receiver: still reported.
class A
  def initialize(v); @v = v; end
end
class C
  def initialize(v); @v = v; end
end
x = [A.new(1), C.new(2)][1]
p x.bar?
