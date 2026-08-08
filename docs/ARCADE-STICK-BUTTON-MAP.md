# 8BitDo Arcade Stick extended-button map

Send `payload/ghost-control-8bitdo-arcade-stick-button-map.elf` while the
2.4G receiver is directly connected to the PS5 and the stick is in Switch mode.
This payload is USB-only: it does not create a virtual controller or write to
the receiver.

It presents one control every four seconds. When each prompt appears, press and
hold that exact physical control for one second, then release it before the next
prompt. The sequence is:

1. D-pad Up, Right, Down, Left
2. Y, X, B, A, L, R, ZL, ZR
3. Minus/Select, Plus/Start, Home, Turbo, P1, P2

After the completion notification, return the complete
`/data/ghostpad/gc_status.log` file. Each input line is tagged with the active
prompt. The captured Switch profile shows that Turbo, P1, and P2 have no
native USB input bit: they are firmware macro controls. A P1-to-Touch-Pad
feature would therefore require configuring P1 to emit an unused standard
input (such as L3) and adding an explicit controller-profile translation for
that surrogate input.
