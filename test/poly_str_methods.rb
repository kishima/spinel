# index/rindex/start_with?/end_with?/split must dispatch on a poly receiver
# that is really a String (sym-hash value; poly-widened method param), like
# byteslice/ljust. Regression for the launcher/taskbar/dialog crashes
# (undefined method 'rindex'/'index'/'end_with?'/'split' for a String).

h = { path: "usr/share/icon/foo.icon", csv: "a,b,c", kv: "key=val" }

path = h[:path]
puts path.rindex("/")            # 14
puts path.index("/")             # 3
puts path.end_with?(".icon")     # true
puts path.start_with?("usr")     # true
p path.rindex("zzz")             # nil
p path.index("zzz")              # nil

puts h[:csv].split(",").length   # 3
puts h[:csv].split(",")[1]       # b
puts h[:kv].index("=")           # 3

# poly-widened method param (a poly and a concrete caller unify the param)
def firstseg(s)
  n = s.index("/")
  n ? s.byteslice(0, n) : s
end
puts firstseg(h[:path])          # usr
puts firstseg("no-slash")        # no-slash
