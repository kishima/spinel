# Array#index/find_index(x) must dispatch on an Array held in a poly slot
# (a symbol-keyed hash value), the same way include? does. Regression for the
# config dialog's `opts = s[:options]; idx = opts.index(cur) || 0`. The storage
# kind is only known at runtime -- homogeneous arrays are typed (StrArray /
# IntArray), mixed ones are PolyArray -- so all kinds must work.
h = { strs: ["low", "mid", "high"], ints: [10, 20, 30], mixed: [1, "x", :y] }

p h[:strs].index("mid")        # 1
p h[:strs].index("nope")       # nil
p h[:strs].find_index("high")  # 2
p h[:ints].index(20)           # 1
p h[:ints].index(99)           # nil
p h[:mixed].index("x")         # 1
p h[:mixed].index(:y)          # 2

# String#index on a poly must still work (TAG_STR pre-arm, not the array case)
p ({ s: "abcdef" }[:s]).index("cd")   # 2

# the config-enum cycle pattern
opts = h[:strs]
cur = "mid"
idx = opts.index(cur) || 0
p (idx + 1 + opts.size) % opts.size    # 2
