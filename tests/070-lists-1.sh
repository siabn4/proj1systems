# Unlike in most conventional programming languages where && has higher precedence than ||,
# in bash, the operators "&&" and "||" shall have equal precedence and shall be evaluated 
# with left associativity.
false && echo skipped || echo test andor ok
# Interpreted as
# ((true || echo skipped) && echo test orand ok)
true || echo skipped && echo test orand ok
