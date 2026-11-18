Colour fixes for textured scenery (reported in Road Rash).

- See-through and shaded graphics now blend like the real console: "halve" mode averages the two colours instead of only halving one, which made blended scenery look washed out and too bright
- Per-channel colour multipliers, subtract mode, XOR mode and the console's "never pure black" rule are now emulated
- 16-bit palette-coded textures read their colours from the right place
