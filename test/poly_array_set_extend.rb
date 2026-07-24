# Array#[]= past the end must auto-extend with nil (CRuby semantics), the same
# way the typed IntArray/StrArray/FloatArray sets already do. PolyArray_set used
# to silently no-op when i >= len, so building an array by index from empty
# (the launcher's @icon_sprite_instances[idx] = sprite) stored nothing.
a = []                       # empty PolyArray
a[0] = "x"
a[2] = "z"
p a.length                   # 3
p a                          # ["x", nil, "z"]

# index-build from empty (launcher icon-sprite pattern)
insts = []
i = 0
while i < 5
  insts[i] = "s#{i}"
  i += 1
end
p insts.length               # 5
p insts[3]                   # "s3"

# negative index still sets in place; too-negative still raises
b = [1, 2, 3]
b[-1] = 9
p b                          # [1, 2, 9]
