# Regression: source_references_set used to match the "Set" token inside
# comments and string literals, splicing an implicit require "set" (and its
# bundled shim) into every program that merely mentioned the word -- e.g. a UI
# label "Set Clock". The scan now skips comments and strings, so this program
# compiles without pulling in the set library.
label = "Set Clock"                       # "Set" is data here, not a constant
menu = { set_clock: "Set the clock now" } # ditto inside a string value
puts label
puts menu[:set_clock]
puts "ok"
