# A binary String with multi-byte bytes in it: index / rindex answer in bytes,
# the unit [] and length count a binary String in. A UTF-8 String counts
# characters everywhere, as before.
s = "\xE6\x96\x87\xE5\xAD\x97 <<en>> draw".b
i = s.index("<<en>>")
p i
p s[i + 6, s.length]
p s.index("<<en>>", 2)
p s.rindex("d")
p s.length

# a UTF-8 String counts characters everywhere, as before
u = "文字 <<en>> draw"
j = u.index("<<en>>")
p j
p u[j + 6, u.length]
p u.rindex("d")
p s.rindex("d", 13)
p s.rindex("<", 8)
p u.rindex("<", 4)
